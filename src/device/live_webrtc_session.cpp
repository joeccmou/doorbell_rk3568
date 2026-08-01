#include "device/live_webrtc_session.h"

#include <gst/allocators/gstdmabuf.h>
#include <gst/app/gstappsrc.h>
#include <gst/app/gstappsink.h>
#include <gst/sdp/sdp.h>
#include <gst/video/video.h>
#include <gst/webrtc/webrtc.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <fcntl.h>
#include <linux/videodev2.h>
#include <memory>
#include <mutex>
#include <sstream>
#include <sys/ioctl.h>
#include <thread>
#include <unistd.h>
#include <utility>
#include <vector>

#if __has_include(<linux/dma-heap.h>)
#include <linux/dma-heap.h>
#else
#include "../../../kernel/include/uapi/linux/dma-heap.h"
#endif

#if !defined(DOORBELL_DISABLE_RGA)
#include <rga/im2d.h>
#endif

namespace {

std::once_flag g_gst_init_once;

constexpr const char *kH264RtpCaps =
    "application/x-rtp,media=(string)video,encoding-name=(string)H264,"
    "payload=(int)96,clock-rate=(int)90000,packetization-mode=(string)1";

constexpr const char *kOpusRtpCaps =
    "application/x-rtp,media=(string)audio,encoding-name=(string)OPUS,"
    "payload=(int)97,clock-rate=(int)48000,encoding-params=(string)2";

constexpr size_t kLiveDmaPoolMaxSlots = 6;

// 摄像头的 DMA 双缓冲会被采集线程复用；编码链路必须持有独立缓冲直到 GstMemory 释放。

struct LiveDmaSlot {
    int fd = -1;
    size_t size = 0;
    bool in_use = false;
};

struct LiveDmaPoolState {
    ~LiveDmaPoolState() {
        for (auto &slot : slots) {
            if (slot.fd >= 0) ::close(slot.fd);
        }
        if (heap_fd >= 0) ::close(heap_fd);
    }

    std::mutex mutex;
    int heap_fd = -1;
    std::vector<LiveDmaSlot> slots;
};

struct LiveDmaLease {
    std::shared_ptr<LiveDmaPoolState> state;
    size_t slot_index = 0;
    int fd = -1;
    size_t size = 0;
};

const char *select_live_dma_heap_path() {
    static const char *chosen = [] {
        if (const char *value = std::getenv("DOORBELL_DMA_HEAP")) return value;
        static const char *candidates[] = {
            "/dev/dma_heap/system-dma32",
            "/dev/dma_heap/system",
            "/dev/dma_heap/cma",
        };
        for (const char *path : candidates) {
            if (::access(path, F_OK) == 0) return path;
        }
        return candidates[0];
    }();
    return chosen;
}

int allocate_live_dma_buffer(int heap_fd, size_t size) {
    dma_heap_allocation_data allocation{};
    allocation.len = size;
    allocation.fd_flags = O_RDWR | O_CLOEXEC;
    allocation.heap_flags = 0;
    if (::ioctl(heap_fd, DMA_HEAP_IOCTL_ALLOC, &allocation) < 0) {
        return -1;
    }
    return static_cast<int>(allocation.fd);
}

std::unique_ptr<LiveDmaLease> acquire_live_dma_buffer(
    const std::shared_ptr<LiveDmaPoolState> &state,
    size_t size) {
    if (!state || size == 0) return nullptr;

    std::lock_guard<std::mutex> lock(state->mutex);
    if (state->heap_fd < 0) {
        state->heap_fd = ::open(select_live_dma_heap_path(), O_RDWR | O_CLOEXEC);
        if (state->heap_fd < 0) {
            std::fprintf(stderr,
                         "[live] open DMA heap failed path=%s errno=%d error=%s\n",
                         select_live_dma_heap_path(),
                         errno,
                         std::strerror(errno));
            return nullptr;
        }
    }

    size_t slot_index = state->slots.size();
    for (size_t i = 0; i < state->slots.size(); ++i) {
        if (!state->slots[i].in_use && state->slots[i].size == size) {
            slot_index = i;
            break;
        }
    }
    if (slot_index == state->slots.size()) {
        for (size_t i = 0; i < state->slots.size(); ++i) {
            if (!state->slots[i].in_use) {
                slot_index = i;
                break;
            }
        }
    }
    if (slot_index == state->slots.size()) {
        if (state->slots.size() >= kLiveDmaPoolMaxSlots) return nullptr;
        state->slots.emplace_back();
    }

    LiveDmaSlot &slot = state->slots[slot_index];
    if (slot.fd < 0 || slot.size != size) {
        if (slot.fd >= 0) ::close(slot.fd);
        slot.fd = allocate_live_dma_buffer(state->heap_fd, size);
        slot.size = slot.fd >= 0 ? size : 0;
        if (slot.fd < 0) {
            std::fprintf(stderr,
                         "[live] allocate DMA-BUF failed size=%zu errno=%d error=%s\n",
                         size,
                         errno,
                         std::strerror(errno));
            return nullptr;
        }
    }
    slot.in_use = true;
    return std::make_unique<LiveDmaLease>(
        LiveDmaLease{state, slot_index, slot.fd, slot.size});
}

void release_live_dma_buffer(LiveDmaLease *lease) {
    if (!lease) return;
    if (lease->state) {
        std::lock_guard<std::mutex> lock(lease->state->mutex);
        if (lease->slot_index < lease->state->slots.size()) {
            lease->state->slots[lease->slot_index].in_use = false;
        }
    }
    delete lease;
}

void on_live_dma_buffer_released(gpointer data, GstMiniObject *) {
    release_live_dma_buffer(static_cast<LiveDmaLease *>(data));
}

uint32_t align_live_height(uint32_t height) {
    return (height + 15U) & ~15U;
}

size_t live_dma_frame_size(uint32_t pixfmt, uint32_t width, uint32_t height) {
    const size_t stride = static_cast<size_t>(width);
    const size_t aligned_height = align_live_height(height);
    switch (pixfmt) {
        case V4L2_PIX_FMT_NV12:
        case V4L2_PIX_FMT_NV21:
            return stride * aligned_height * 3 / 2;
        default:
            return 0;
    }
}

GstVideoFormat gst_video_format_for_pixfmt(uint32_t pixfmt) {
    switch (pixfmt) {
        case V4L2_PIX_FMT_NV12:
            return GST_VIDEO_FORMAT_NV12;
        case V4L2_PIX_FMT_NV21:
            return GST_VIDEO_FORMAT_NV21;
        default:
            return GST_VIDEO_FORMAT_UNKNOWN;
    }
}

bool copy_live_frame_to_dma(const LiveWebRtcSession::VideoFrame &frame,
                            uint32_t output_width,
                            uint32_t output_height,
                            int output_fd,
                            int *rga_status) {
#if defined(DOORBELL_DISABLE_RGA)
    (void)frame;
    (void)output_width;
    (void)output_height;
    (void)output_fd;
    if (rga_status) *rga_status = -1;
    return false;
#else
    if (frame.dmabuf_fd < 0 || output_fd < 0 ||
        (frame.pixfmt != V4L2_PIX_FMT_NV12 &&
         frame.pixfmt != V4L2_PIX_FMT_NV21)) {
        if (rga_status) *rga_status = -1;
        return false;
    }

    const int rga_format = frame.pixfmt == V4L2_PIX_FMT_NV12
        ? RK_FORMAT_YCbCr_420_SP
        : RK_FORMAT_YCrCb_420_SP;
    const int source_stride = static_cast<int>(
        frame.stride_y >= frame.width ? frame.stride_y : frame.width);
    const int output_stride = static_cast<int>(output_width);
    const int output_hstride = static_cast<int>(align_live_height(output_height));
    rga_buffer_t source = wrapbuffer_fd(
        frame.dmabuf_fd,
        static_cast<int>(frame.width),
        static_cast<int>(frame.height),
        rga_format,
        source_stride,
        static_cast<int>(frame.height));
    rga_buffer_t output = wrapbuffer_fd(
        output_fd,
        static_cast<int>(output_width),
        static_cast<int>(output_height),
        rga_format,
        output_stride,
        output_hstride);
    rga_buffer_t pattern{};
    im_rect source_rect{
        0, 0, static_cast<int>(frame.width), static_cast<int>(frame.height)};
    im_rect output_rect{
        0, 0, static_cast<int>(output_width), static_cast<int>(output_height)};
    im_rect pattern_rect{};
    const IM_STATUS status = improcess(
        source, output, pattern, source_rect, output_rect, pattern_rect, 0);
    if (rga_status) *rga_status = static_cast<int>(status);
    return status > 0;
#endif
}

const char *gst_format_for_pixfmt(uint32_t pixfmt) {
    switch (pixfmt) {
        case V4L2_PIX_FMT_UYVY:
            return "UYVY";
        case V4L2_PIX_FMT_NV12:
            return "NV12";
        case V4L2_PIX_FMT_NV21:
            return "NV21";
        case V4L2_PIX_FMT_YUYV:
            return "YUY2";
        case V4L2_PIX_FMT_YUV422P:
            return "Y42B";
        default:
            return nullptr;
    }
}

size_t raw_frame_size(uint32_t pixfmt, uint32_t width, uint32_t height) {
    const size_t w = static_cast<size_t>(width);
    const size_t h = static_cast<size_t>(height);
    switch (pixfmt) {
        case V4L2_PIX_FMT_UYVY:
        case V4L2_PIX_FMT_YUYV:
        case V4L2_PIX_FMT_YUV422P:
            return w * h * 2;
        case V4L2_PIX_FMT_NV12:
        case V4L2_PIX_FMT_NV21:
            return w * h * 3 / 2;
        default:
            return 0;
    }
}

std::string caps_to_string(GstCaps *caps) {
    if (!caps) return "(null)";
    gchar *text = gst_caps_to_string(caps);
    std::string result = text ? text : "(null)";
    if (text) g_free(text);
    return result;
}

const char *pad_direction_name(GstPadDirection direction) {
    switch (direction) {
        case GST_PAD_SRC:
            return "src";
        case GST_PAD_SINK:
            return "sink";
        default:
            return "unknown";
    }
}

const char *pad_presence_name(GstPadPresence presence) {
    switch (presence) {
        case GST_PAD_ALWAYS:
            return "always";
        case GST_PAD_SOMETIMES:
            return "sometimes";
        case GST_PAD_REQUEST:
            return "request";
        default:
            return "unknown";
    }
}

const char *state_change_return_name(GstStateChangeReturn ret) {
    switch (ret) {
        case GST_STATE_CHANGE_FAILURE:
            return "failure";
        case GST_STATE_CHANGE_SUCCESS:
            return "success";
        case GST_STATE_CHANGE_ASYNC:
            return "async";
        case GST_STATE_CHANGE_NO_PREROLL:
            return "no-preroll";
        default:
            return "unknown";
    }
}

const char *ice_connection_state_name(GstWebRTCICEConnectionState state) {
    switch (state) {
        case GST_WEBRTC_ICE_CONNECTION_STATE_NEW:
            return "new";
        case GST_WEBRTC_ICE_CONNECTION_STATE_CHECKING:
            return "checking";
        case GST_WEBRTC_ICE_CONNECTION_STATE_CONNECTED:
            return "connected";
        case GST_WEBRTC_ICE_CONNECTION_STATE_COMPLETED:
            return "completed";
        case GST_WEBRTC_ICE_CONNECTION_STATE_FAILED:
            return "failed";
        case GST_WEBRTC_ICE_CONNECTION_STATE_DISCONNECTED:
            return "disconnected";
        case GST_WEBRTC_ICE_CONNECTION_STATE_CLOSED:
            return "closed";
        default:
            return "unknown";
    }
}

void log_pad_template(const char *element_name, GstPadTemplate *templ) {
    if (!templ) {
        std::fprintf(stderr, "[live] %s pad-template: null\n", element_name);
        return;
    }
    GstCaps *caps = gst_pad_template_get_caps(templ);
    const std::string caps_text = caps_to_string(caps);
    std::fprintf(stderr,
                 "[live] %s pad-template name=%s direction=%s presence=%s caps=%s\n",
                 element_name,
                 GST_PAD_TEMPLATE_NAME_TEMPLATE(templ),
                 pad_direction_name(GST_PAD_TEMPLATE_DIRECTION(templ)),
                 pad_presence_name(GST_PAD_TEMPLATE_PRESENCE(templ)),
                 caps_text.c_str());
    if (caps) gst_caps_unref(caps);
}

void log_element_pad_templates(GstElement *element, const char *element_name) {
    if (!element) {
        std::fprintf(stderr, "[live] %s element is null\n", element_name);
        return;
    }
    GstElementClass *klass = GST_ELEMENT_GET_CLASS(element);
    const GList *templates = gst_element_class_get_pad_template_list(klass);
    std::fprintf(stderr,
                 "[live] %s type=%s pad-templates-begin\n",
                 element_name,
                 G_OBJECT_TYPE_NAME(element));
    if (!templates) {
        std::fprintf(stderr, "[live] %s pad-templates: empty\n", element_name);
    }
    for (const GList *item = templates; item; item = item->next) {
        log_pad_template(element_name, GST_PAD_TEMPLATE(item->data));
    }
    std::fprintf(stderr, "[live] %s pad-templates-end\n", element_name);
}

void log_webrtc_request_pad_failure(GstElement *webrtc,
                                    GstCaps *rtp_caps,
                                    GstPadTemplate *sink_template,
                                    GstPad *rtp_src_pad,
                                    GstPad *webrtc_sink_pad) {
    guint major = 0;
    guint minor = 0;
    guint micro = 0;
    guint nano = 0;
    gst_version(&major, &minor, &micro, &nano);
    const std::string request_caps = caps_to_string(rtp_caps);
    std::fprintf(stderr,
                 "[live] request webrtc RTP sink pad failed: gst=%u.%u.%u nano=%u rtp_caps=%s sink_template=%p rtp_src_pad=%p webrtc_sink_pad=%p\n",
                 major,
                 minor,
                 micro,
                 nano,
                 request_caps.c_str(),
                 static_cast<void *>(sink_template),
                 static_cast<void *>(rtp_src_pad),
                 static_cast<void *>(webrtc_sink_pad));
    if (sink_template) log_pad_template("webrtc.selected", sink_template);
    log_element_pad_templates(webrtc, "webrtc");
}

GstPad *request_webrtc_sink_pad_by_method(GstElement *webrtc,
                                          GstPadTemplate *sink_template,
                                          const char *method,
                                          const char *requested_name) {
    GstPad *pad = nullptr;
    if (std::string(method).rfind("get_request_pad", 0) == 0) {
        pad = gst_element_get_request_pad(webrtc, requested_name);
    } else if (sink_template) {
        pad = gst_element_request_pad(webrtc, sink_template, requested_name, nullptr);
    }

    std::fprintf(stderr,
                 "[live] request webrtc sink pad method=%s requested_name=%s result=%p actual_name=%s\n",
                 method,
                 requested_name ? requested_name : "(auto)",
                 static_cast<void *>(pad),
                 pad ? GST_OBJECT_NAME(pad) : "(null)");
    return pad;
}

GstPad *request_webrtc_sink_pad_with_probe(GstElement *pipeline,
                                           GstElement *webrtc,
                                           GstPadTemplate *sink_template,
                                           bool *pipeline_readied) {
    GstPad *pad = request_webrtc_sink_pad_by_method(webrtc, sink_template, "get_request_pad", "sink_%u");
    if (pad) return pad;

    pad = request_webrtc_sink_pad_by_method(webrtc, sink_template, "request_pad_template", nullptr);
    if (pad) return pad;

    pad = request_webrtc_sink_pad_by_method(webrtc, sink_template, "request_pad_template", "sink_0");
    if (pad) return pad;

    GstStateChangeReturn ready_ret = gst_element_set_state(pipeline, GST_STATE_READY);
    if (pipeline_readied) *pipeline_readied = ready_ret != GST_STATE_CHANGE_FAILURE;
    GstState state = GST_STATE_VOID_PENDING;
    GstState pending = GST_STATE_VOID_PENDING;
    GstStateChangeReturn wait_ret = gst_element_get_state(pipeline, &state, &pending, 2 * GST_SECOND);
    std::fprintf(stderr,
                 "[live] set pipeline READY before request pad ret=%s wait=%s state=%s pending=%s\n",
                 state_change_return_name(ready_ret),
                 state_change_return_name(wait_ret),
                 gst_element_state_get_name(state),
                 gst_element_state_get_name(pending));
    if (ready_ret == GST_STATE_CHANGE_FAILURE) return nullptr;

    pad = request_webrtc_sink_pad_by_method(webrtc, sink_template, "get_request_pad_after_ready", "sink_%u");
    if (pad) return pad;

    pad = request_webrtc_sink_pad_by_method(webrtc, sink_template, "request_pad_template_after_ready", nullptr);
    if (pad) return pad;

    return request_webrtc_sink_pad_by_method(webrtc, sink_template, "request_pad_template_after_ready", "sink_0");
}

std::string normalize_stun_url(const std::string &url) {
    if (url.rfind("stun://", 0) == 0) return url;
    if (url.rfind("stun:", 0) == 0) return "stun://" + url.substr(5);
    return url;
}

std::string build_turn_uri(const LiveWebRtcSession::IceServer &server,
                           const std::string &url) {
    std::string endpoint = url;
    if (endpoint.rfind("turn://", 0) == 0) {
        endpoint = endpoint.substr(7);
    } else if (endpoint.rfind("turn:", 0) == 0) {
        endpoint = endpoint.substr(5);
    }

    gchar *escaped_username =
        g_uri_escape_string(server.username.c_str(), nullptr, FALSE);
    gchar *escaped_credential =
        g_uri_escape_string(server.credential.c_str(), nullptr, FALSE);
    const std::string uri =
        "turn://" +
        std::string(escaped_username ? escaped_username : "") + ":" +
        std::string(escaped_credential ? escaped_credential : "") + "@" +
        endpoint;
    g_free(escaped_username);
    g_free(escaped_credential);
    return uri;
}

std::string turn_host_for_log(const std::string &url) {
    std::string host = url;
    if (host.rfind("turn://", 0) == 0) {
        host = host.substr(7);
    } else if (host.rfind("turn:", 0) == 0) {
        host = host.substr(5);
    }
    const size_t query = host.find('?');
    if (query != std::string::npos) host.resize(query);
    return host;
}

std::string turn_transport_for_log(const std::string &url) {
    const std::string marker = "transport=";
    const size_t start = url.find(marker);
    if (start == std::string::npos) return "default";
    const size_t value_start = start + marker.size();
    const size_t end = url.find('&', value_start);
    return url.substr(value_start, end - value_start);
}

std::string turn_expires_for_log(const std::string &username) {
    const size_t separator = username.find(':');
    return username.substr(0, separator);
}

struct TurnServerEntry {
    std::string uri;
    std::string host;
    std::string expires;
    std::string transport;
};

std::vector<TurnServerEntry> build_turn_servers(
    const std::vector<LiveWebRtcSession::IceServer> &ice_servers) {
    std::vector<TurnServerEntry> result;
    for (const auto &server : ice_servers) {
        if (server.username.empty() || server.credential.empty()) continue;
        for (const auto &url : server.urls) {
            if (url.rfind("turn:", 0) != 0 &&
                url.rfind("turn://", 0) != 0) {
                continue;
            }
            result.push_back({
                build_turn_uri(server, url),
                turn_host_for_log(url),
                turn_expires_for_log(server.username),
                turn_transport_for_log(url),
            });
        }
    }
    return result;
}

std::string summarize_ice_candidate(const std::string &candidate) {
    std::string normalized = candidate;
    if (normalized.rfind("candidate:", 0) == 0) {
        normalized = normalized.substr(10);
    }

    std::istringstream in(normalized);
    std::vector<std::string> tokens;
    std::string token;
    while (in >> token) {
        tokens.push_back(token);
    }
    if (tokens.size() < 6) {
        return candidate;
    }

    const std::string &foundation = tokens[0];
    const std::string &component = tokens[1];
    const std::string &transport = tokens[2];
    const std::string &priority = tokens[3];
    const std::string &address = tokens[4];
    const std::string &port = tokens[5];

    std::string candidate_type;
    std::string related_address;
    std::string related_port;
    std::string tcp_type;

    for (size_t i = 6; i + 1 < tokens.size(); ++i) {
        if (tokens[i] == "typ") {
            candidate_type = tokens[i + 1];
            ++i;
        } else if (tokens[i] == "raddr") {
            related_address = tokens[i + 1];
            ++i;
        } else if (tokens[i] == "rport") {
            related_port = tokens[i + 1];
            ++i;
        } else if (tokens[i] == "tcptype") {
            tcp_type = tokens[i + 1];
            ++i;
        }
    }

    std::ostringstream out;
    out << "foundation=" << foundation
        << " component=" << component
        << " transport=" << transport
        << " priority=" << priority
        << " addr=" << address << ':' << port;
    if (!candidate_type.empty()) {
        out << " type=" << candidate_type;
    }
    if (!related_address.empty()) {
        out << " raddr=" << related_address;
    }
    if (!related_port.empty()) {
        out << " rport=" << related_port;
    }
    if (!tcp_type.empty()) {
        out << " tcptype=" << tcp_type;
    }
    return out.str();
}

void log_sdp_candidate_lines(const char *label, const std::string &sdp) {
    std::istringstream in(sdp);
    std::string line;
    size_t candidate_index = 0;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        if (line.rfind("a=candidate:", 0) == 0) {
            ++candidate_index;
            std::fprintf(stderr,
                         "[live] %s sdp-candidate[%zu] %s\n",
                         label,
                         candidate_index,
                         summarize_ice_candidate(line.substr(2)).c_str());
        } else if (line.rfind("c=IN ", 0) == 0) {
            std::fprintf(stderr,
                         "[live] %s sdp-connection %s\n",
                         label,
                         line.c_str());
        }
    }
}

void on_negotiation_needed_cb(GstElement *, gpointer user_data) {
    static_cast<LiveWebRtcSession *>(user_data)->on_negotiation_needed();
}

void on_offer_created_cb(GstPromise *promise, gpointer user_data) {
    static_cast<LiveWebRtcSession *>(user_data)->on_offer_created(promise);
}

void on_ice_candidate_cb(GstElement *, guint mline_index, gchar *candidate, gpointer user_data) {
    static_cast<LiveWebRtcSession *>(user_data)->on_ice_candidate(mline_index, candidate);
}

void on_ice_connection_state_notify_cb(GObject *, GParamSpec *, gpointer user_data) {
    static_cast<LiveWebRtcSession *>(user_data)->on_ice_connection_state_changed();
}

gboolean on_bus_message_cb(GstBus *, GstMessage *message, gpointer user_data) {
    static_cast<LiveWebRtcSession *>(user_data)->on_bus_message(message);
    return G_SOURCE_CONTINUE;
}

GstPadProbeReturn on_rtp_probe_cb(GstPad *, GstPadProbeInfo *info, gpointer user_data) {
    return static_cast<LiveWebRtcSession *>(user_data)->on_rtp_probe(info);
}

void on_incoming_pad_added_cb(GstElement *, GstPad *pad, gpointer user_data) {
    static_cast<LiveWebRtcSession *>(user_data)->on_incoming_pad_added(pad);
}
GstPadProbeReturn on_audio_rtp_probe_cb(GstPad *, GstPadProbeInfo *info, gpointer user_data) {
    return static_cast<LiveWebRtcSession *>(user_data)->on_audio_rtp_probe(info);
}

GstFlowReturn on_remote_audio_sample_cb(GstAppSink *sink, gpointer user_data) {
    return static_cast<LiveWebRtcSession *>(user_data)->on_remote_audio_sample(GST_ELEMENT(sink));
}

const char *message_source_name(GstMessage *message) {
    if (!message || !GST_MESSAGE_SRC(message)) return "unknown";
    return GST_OBJECT_NAME(GST_MESSAGE_SRC(message));
}

bool is_video_branch_source(const char *source) {
    if (!source) return false;
    return std::strcmp(source, "live_appsrc") == 0 ||
           std::strcmp(source, "raw_queue") == 0 ||
           std::strcmp(source, "live_enc") == 0 ||
           std::strcmp(source, "live_parse") == 0 ||
           std::strcmp(source, "live_pay") == 0 ||
           std::strcmp(source, "rtp_queue") == 0;
}

}  // namespace

struct LiveWebRtcSession::RemoteDescriptionContext {
    LiveWebRtcSession *session = nullptr;
    std::string call_id;
};

LiveWebRtcSession::LiveWebRtcSession(SignalPublisher signal_publisher, StatePublisher state_publisher)
    : signal_publisher_(std::move(signal_publisher)), state_publisher_(std::move(state_publisher)) {}

LiveWebRtcSession::~LiveWebRtcSession() {
    stop();
}

LiveWebRtcSession::QualityProfile LiveWebRtcSession::profile_for_quality(const std::string &quality) {
    if (quality == "360p") return {640, 360, 450000};
    if (quality == "720p") return {1280, 720, 1000000};
    if (quality == "1080p") return {1920, 1080, 2000000};
    if (quality == "1440p") return {2560, 1440, 3500000};
    return {1280, 720, 1000000};
}

std::string LiveWebRtcSession::stun_server_for(const std::vector<IceServer> &ice_servers) {
    for (const auto &server : ice_servers) {
        for (const auto &url : server.urls) {
            if (url.rfind("stun:", 0) == 0 || url.rfind("stun://", 0) == 0) {
                return normalize_stun_url(url);
            }
        }
    }
    return "";
}

bool LiveWebRtcSession::start(const StartRequest &request, std::string *error_message) {
    std::call_once(g_gst_init_once, []() { gst_init(nullptr, nullptr); });
    stop();

    const QualityProfile profile = profile_for_quality(request.quality);
    const std::string stun_server = stun_server_for(request.ice_servers);
    const std::vector<TurnServerEntry> turn_servers =
        build_turn_servers(request.ice_servers);
    if (stun_server.empty() && turn_servers.empty()) {
        if (error_message) *error_message = "no valid STUN or TURN server";
        return false;
    }

    FrameProvider provider;
    AudioCaptureManager *audio_manager = nullptr;
    {
        std::lock_guard<std::mutex> lock(mtx_);
        provider = frame_provider_;
        audio_manager = audio_manager_;
    }
    if (!provider) {
        if (error_message) *error_message = "camera frame provider unavailable";
        return false;
    }

    VideoFrame initial_frame;
    bool got_initial_frame = false;
    for (int attempt = 0; attempt < 50 && !got_initial_frame; ++attempt) {
                got_initial_frame = provider(initial_frame) &&
                            initial_frame.data != nullptr &&
                            initial_frame.size != 0 &&
                            initial_frame.width != 0 &&
                            initial_frame.height != 0;
        if (!got_initial_frame) {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
    }
    if (!got_initial_frame) {
        if (error_message) *error_message = "camera frame unavailable";
        return false;
    }

    const char *gst_format = gst_format_for_pixfmt(initial_frame.pixfmt);
    const size_t frame_size = raw_frame_size(initial_frame.pixfmt, initial_frame.width, initial_frame.height);
    const size_t dma_frame_size = live_dma_frame_size(
        initial_frame.pixfmt,
        static_cast<uint32_t>(profile.width),
        static_cast<uint32_t>(profile.height));
    if (!gst_format || frame_size == 0 || dma_frame_size == 0 ||
        initial_frame.size < frame_size || initial_frame.dmabuf_fd < 0) {
        if (error_message) *error_message = "camera frame format unsupported for WebRTC";
        std::fprintf(stderr,
                     "[live] unsupported camera frame fmt=%c%c%c%c size=%ux%u bytes=%zu expected=%zu dmabuf_fd=%d\n",
                     initial_frame.pixfmt & 0xFF,
                     (initial_frame.pixfmt >> 8) & 0xFF,
                     (initial_frame.pixfmt >> 16) & 0xFF,
                     (initial_frame.pixfmt >> 24) & 0xFF,
                     initial_frame.width,
                     initial_frame.height,
                     initial_frame.size,
                     frame_size,
                     initial_frame.dmabuf_fd);
        return false;
    }
    std::ostringstream desc;
    desc << "webrtcbin name=webrtc bundle-policy=max-bundle";
    if (!stun_server.empty()) {
        desc << " stun-server=" << stun_server;
    }
    desc
         << " appsrc name=live_appsrc is-live=true block=false format=time do-timestamp=false "
         << "caps=video/x-raw,format=" << gst_format
         << ",width=" << profile.width
         << ",height=" << profile.height
         << ",framerate=24/1 ! queue name=raw_queue leaky=downstream max-size-buffers=2 ! "
         << "mpph264enc name=live_enc bps=" << profile.bitrate << " gop=24 header-mode=each-idr ! "
         << "h264parse name=live_parse config-interval=-1 ! "
         << "rtph264pay name=live_pay pt=96 config-interval=-1 ! "
         << kH264RtpCaps << " ! queue name=rtp_queue "
         << "appsrc name=live_audio_appsrc is-live=true block=false format=time do-timestamp=false "
         << "caps=audio/x-raw,format=S16LE,layout=interleaved,rate=48000,channels=1 "
         << "! queue name=audio_raw_queue leaky=downstream max-size-buffers=16 ! audioconvert ! audioresample "
         << "! opusenc bitrate=32000 audio-type=voice frame-size=20 ! rtpopuspay name=live_audio_pay pt=97 ! "
         << kOpusRtpCaps << " ! queue name=audio_rtp_queue "
         << "queue name=remote_audio_queue ! rtpopusdepay ! opusdec ! audioconvert ! audioresample "
         << "! audio/x-raw,format=S16LE,layout=interleaved,rate=48000,channels=1 "
         << "! appsink name=remote_audio_sink emit-signals=true sync=false max-buffers=8 drop=true";

    GError *error = nullptr;
    GstElement *pipeline = gst_parse_launch(desc.str().c_str(), &error);
    if (!pipeline) {
        if (error_message) {
            *error_message = error ? error->message : "create GStreamer pipeline failed";
        }
        if (error) g_error_free(error);
        return false;
    }
    if (error) {
        std::fprintf(stderr, "[live] pipeline warning: %s\n", error->message);
        g_error_free(error);
    }

    GstElement *webrtc = gst_bin_get_by_name(GST_BIN(pipeline), "webrtc");
    if (!webrtc) {
        gst_object_unref(pipeline);
        if (error_message) *error_message = "webrtcbin element not found";
        return false;
    }
    GstElement *appsrc = gst_bin_get_by_name(GST_BIN(pipeline), "live_appsrc");
    if (!appsrc) {
        gst_object_unref(webrtc);
        gst_object_unref(pipeline);
        if (error_message) *error_message = "live appsrc element not found";
        return false;
    }
    gst_app_src_set_stream_type(GST_APP_SRC(appsrc), GST_APP_STREAM_TYPE_STREAM);
    const guint64 appsrc_max_bytes =
        static_cast<guint64>(dma_frame_size) * 2;
    g_object_set(G_OBJECT(appsrc),
                 "block", FALSE,
                 "max-bytes", appsrc_max_bytes,
                 nullptr);
    GstElement *video_encoder = gst_bin_get_by_name(GST_BIN(pipeline), "live_enc");
    if (!video_encoder) {
        gst_object_unref(appsrc);
        gst_object_unref(webrtc);
        gst_object_unref(pipeline);
        if (error_message) *error_message = "live video encoder element not found";
        return false;
    }
    for (size_t index = 0; index < turn_servers.size(); ++index) {
        gboolean added = FALSE;
        g_signal_emit_by_name(
            webrtc,
            "add-turn-server",
            turn_servers[index].uri.c_str(),
            &added);
        if (!added) {
            const auto &server = turn_servers[index];
            std::fprintf(
                stderr,
                "[live] add TURN server failed host=%s expires=%s transport=%s\n",
                server.host.c_str(),
                server.expires.c_str(),
                server.transport.c_str());
            gst_object_unref(video_encoder);
            gst_object_unref(appsrc);
            gst_object_unref(webrtc);
            gst_object_unref(pipeline);
            if (error_message) *error_message = "configure TURN server failed";
            return false;
        }
    }

    GstElement *rtp_queue = gst_bin_get_by_name(GST_BIN(pipeline), "rtp_queue");
    if (!rtp_queue) {
        gst_object_unref(video_encoder);
        gst_object_unref(appsrc);
        gst_object_unref(webrtc);
        gst_object_unref(pipeline);
        if (error_message) *error_message = "RTP queue element not found";
        return false;
    }

    GstElement *audio_appsrc = gst_bin_get_by_name(GST_BIN(pipeline), "live_audio_appsrc");
    GstElement *audio_rtp_queue = gst_bin_get_by_name(GST_BIN(pipeline), "audio_rtp_queue");
    GstElement *remote_audio_queue = gst_bin_get_by_name(GST_BIN(pipeline), "remote_audio_queue");
    GstElement *remote_audio_sink = gst_bin_get_by_name(GST_BIN(pipeline), "remote_audio_sink");
    if (!audio_appsrc || !audio_rtp_queue || !remote_audio_queue || !remote_audio_sink) {
        if (audio_appsrc) gst_object_unref(audio_appsrc);
        if (audio_rtp_queue) gst_object_unref(audio_rtp_queue);
        if (remote_audio_queue) gst_object_unref(remote_audio_queue);
        if (remote_audio_sink) gst_object_unref(remote_audio_sink);
        gst_object_unref(rtp_queue);
        gst_object_unref(appsrc);
        gst_object_unref(webrtc);
        gst_object_unref(pipeline);
        if (error_message) *error_message = "WebRTC audio elements not found";
        return false;
    }
    gst_app_src_set_stream_type(GST_APP_SRC(audio_appsrc), GST_APP_STREAM_TYPE_STREAM);
    g_object_set(G_OBJECT(audio_appsrc), "block", FALSE, nullptr);
    GstCaps *rtp_caps = gst_caps_from_string(kH264RtpCaps);
    GstPadTemplate *sink_template = gst_element_class_get_pad_template(GST_ELEMENT_GET_CLASS(webrtc), "sink_%u");
    GstPad *rtp_src_pad = gst_element_get_static_pad(rtp_queue, "src");
    std::fprintf(stderr, "[live] request webrtc RTP sink pad without explicit transceiver\n");
    bool pipeline_readied_for_request = false;
    GstPad *webrtc_sink_pad = request_webrtc_sink_pad_with_probe(pipeline,
                                                                 webrtc,
                                                                 sink_template,
                                                                 &pipeline_readied_for_request);
    if (!rtp_caps || !rtp_src_pad || !webrtc_sink_pad) {
        log_webrtc_request_pad_failure(webrtc, rtp_caps, sink_template, rtp_src_pad, webrtc_sink_pad);
        if (webrtc_sink_pad) {
            gst_element_release_request_pad(webrtc, webrtc_sink_pad);
            gst_object_unref(webrtc_sink_pad);
        }
        if (rtp_src_pad) gst_object_unref(rtp_src_pad);
        if (rtp_caps) gst_caps_unref(rtp_caps);
        gst_object_unref(rtp_queue);
        gst_object_unref(appsrc);
        gst_object_unref(webrtc);
        if (pipeline_readied_for_request) gst_element_set_state(pipeline, GST_STATE_NULL);
        gst_object_unref(pipeline);
        if (error_message) {
            *error_message = "request webrtc RTP sink pad failed";
        }
        return false;
    }

    GstPadLinkReturn link_ret = gst_pad_link(rtp_src_pad, webrtc_sink_pad);
    gst_object_unref(rtp_src_pad);
    gst_caps_unref(rtp_caps);
    gst_object_unref(rtp_queue);
    if (link_ret != GST_PAD_LINK_OK) {
        const char *link_name = gst_pad_link_get_name(link_ret);
        std::fprintf(stderr, "[live] link RTP payloader to webrtc failed: %s\n", link_name);
        gst_element_release_request_pad(webrtc, webrtc_sink_pad);
        gst_object_unref(webrtc_sink_pad);
        gst_object_unref(appsrc);
        gst_object_unref(webrtc);
        if (pipeline_readied_for_request) gst_element_set_state(pipeline, GST_STATE_NULL);
        gst_object_unref(pipeline);
        if (error_message) {
            *error_message = std::string("link RTP payloader to webrtc failed: ") + link_name;
        }
        return false;
    }

    GstCaps *audio_rtp_caps = gst_caps_from_string(kOpusRtpCaps);
    GstPad *audio_rtp_src_pad = gst_element_get_static_pad(audio_rtp_queue, "src");
    std::fprintf(stderr, "[live] request webrtc audio RTP sink pad\n");
    GstPad *webrtc_audio_sink_pad = request_webrtc_sink_pad_with_probe(pipeline,
                                                                       webrtc,
                                                                       sink_template,
                                                                       &pipeline_readied_for_request);
    if (!audio_rtp_caps || !audio_rtp_src_pad || !webrtc_audio_sink_pad) {
        log_webrtc_request_pad_failure(webrtc, audio_rtp_caps, sink_template, audio_rtp_src_pad, webrtc_audio_sink_pad);
        if (webrtc_audio_sink_pad) {
            gst_element_release_request_pad(webrtc, webrtc_audio_sink_pad);
            gst_object_unref(webrtc_audio_sink_pad);
        }
        if (audio_rtp_src_pad) gst_object_unref(audio_rtp_src_pad);
        if (audio_rtp_caps) gst_caps_unref(audio_rtp_caps);
        gst_element_release_request_pad(webrtc, webrtc_sink_pad);
        gst_object_unref(webrtc_sink_pad);
        gst_object_unref(audio_rtp_queue);
        gst_object_unref(remote_audio_queue);
        gst_object_unref(remote_audio_sink);
        gst_object_unref(audio_appsrc);
        gst_object_unref(appsrc);
        gst_object_unref(webrtc);
        if (pipeline_readied_for_request) gst_element_set_state(pipeline, GST_STATE_NULL);
        gst_object_unref(pipeline);
        if (error_message) *error_message = "request webrtc audio RTP sink pad failed";
        return false;
    }

    GstPadLinkReturn audio_link_ret = gst_pad_link(audio_rtp_src_pad, webrtc_audio_sink_pad);
    gst_object_unref(audio_rtp_src_pad);
    gst_caps_unref(audio_rtp_caps);
    gst_object_unref(audio_rtp_queue);
    if (audio_link_ret != GST_PAD_LINK_OK) {
        const char *link_name = gst_pad_link_get_name(audio_link_ret);
        std::fprintf(stderr, "[live] link audio RTP payloader to webrtc failed: %s\n", link_name);
        gst_element_release_request_pad(webrtc, webrtc_audio_sink_pad);
        gst_object_unref(webrtc_audio_sink_pad);
        gst_element_release_request_pad(webrtc, webrtc_sink_pad);
        gst_object_unref(webrtc_sink_pad);
        gst_object_unref(remote_audio_queue);
        gst_object_unref(remote_audio_sink);
        gst_object_unref(audio_appsrc);
        gst_object_unref(appsrc);
        gst_object_unref(webrtc);
        if (pipeline_readied_for_request) gst_element_set_state(pipeline, GST_STATE_NULL);
        gst_object_unref(pipeline);
        if (error_message) {
            *error_message = std::string("link audio RTP payloader to webrtc failed: ") + link_name;
        }
        return false;
    }
    gulong rtp_probe_id = gst_pad_add_probe(webrtc_sink_pad,
                                            GST_PAD_PROBE_TYPE_BUFFER,
                                            on_rtp_probe_cb,
                                            this,
                                            nullptr);
    std::fprintf(stderr,
                 "[live] RTP probe installed pad=%s id=%lu\n",
                 GST_OBJECT_NAME(webrtc_sink_pad),
                 static_cast<unsigned long>(rtp_probe_id));
    gulong audio_rtp_probe_id = gst_pad_add_probe(webrtc_audio_sink_pad,
                                                   GST_PAD_PROBE_TYPE_BUFFER,
                                                   on_audio_rtp_probe_cb,
                                                   this,
                                                   nullptr);

    guint bus_watch_id = 0;
    GstBus *bus = gst_element_get_bus(pipeline);
    if (bus) {
        bus_watch_id = gst_bus_add_watch(bus, on_bus_message_cb, this);
        gst_object_unref(bus);
    }
    std::fprintf(stderr,
                 "[live] bus watch installed id=%u\n",
                 bus_watch_id);
    g_signal_connect(webrtc, "on-negotiation-needed", G_CALLBACK(on_negotiation_needed_cb), this);
    g_signal_connect(webrtc, "on-ice-candidate", G_CALLBACK(on_ice_candidate_cb), this);
    g_signal_connect(webrtc, "notify::ice-connection-state", G_CALLBACK(on_ice_connection_state_notify_cb), this);
    g_signal_connect(webrtc, "pad-added", G_CALLBACK(on_incoming_pad_added_cb), this);
    g_signal_connect(remote_audio_sink, "new-sample", G_CALLBACK(on_remote_audio_sample_cb), this);

    main_loop_ = g_main_loop_new(nullptr, false);
    loop_thread_ = std::thread([this]() {
        if (main_loop_) g_main_loop_run(main_loop_);
    });

    {
        std::lock_guard<std::mutex> lock(mtx_);
        current_ = request;
        pipeline_ = pipeline;
        webrtc_ = webrtc;
        appsrc_ = appsrc;
        video_encoder_ = video_encoder;
        audio_appsrc_ = audio_appsrc;
        remote_audio_queue_ = remote_audio_queue;
        remote_audio_sink_ = remote_audio_sink;
        webrtc_sink_pad_ = webrtc_sink_pad;
        webrtc_audio_sink_pad_ = webrtc_audio_sink_pad;
        bus_watch_id_ = bus_watch_id;
        rtp_probe_id_ = rtp_probe_id;
        audio_rtp_probe_id_ = audio_rtp_probe_id;
        audio_rtp_buffer_count_ = 0;
        audio_rtp_bytes_ = 0;
        audio_timestamp_rebaser_.reset();
        audio_input_count_.store(0);
        remote_audio_count_.store(0);
        video_input_count_.store(0);
        rtp_buffer_count_ = 0;
        rtp_bytes_ = 0;
        appsrc_width_ = static_cast<uint32_t>(profile.width);
        appsrc_height_ = static_cast<uint32_t>(profile.height);
        appsrc_pixfmt_ = initial_frame.pixfmt;
        appsrc_frame_size_ = raw_frame_size(
            initial_frame.pixfmt,
            static_cast<uint32_t>(profile.width),
            static_cast<uint32_t>(profile.height));
        frame_stop_.store(false);
        video_paused_.store(false);
        active_published_ = false;
        offer_requested_ = false;
        pending_quality_switch_.reset();
        quality_switch_deadline_ = {};
        quality_switch_answer_applied_ = false;
        quality_switch_watchdog_stop_ = false;
    }
    quality_switch_watchdog_thread_ =
        std::thread(&LiveWebRtcSession::quality_switch_watchdog_loop, this);

    GstStateChangeReturn ret = gst_element_set_state(pipeline, GST_STATE_PLAYING);
    if (ret == GST_STATE_CHANGE_FAILURE) {
        stop();
        if (error_message) *error_message = "set GStreamer pipeline PLAYING failed";
        return false;
    }

    if (audio_manager) {
        size_t consumer_id = audio_manager->register_consumer(
            [this](const AudioFrame &frame) {
                push_audio_frame(frame);
            });
        std::lock_guard<std::mutex> lock(mtx_);
        audio_consumer_id_ = consumer_id;
    } else {
        std::fprintf(stderr, "[live] audio manager unavailable; device microphone track will be silent\n");
    }

    frame_thread_ = std::thread(&LiveWebRtcSession::frame_push_loop, this);

    std::fprintf(stdout,
                 "[live] started WebRTC H264 quality=%s requested=%dx%d actual=%ux%u fps=24 bps=%d call_id=%s\n",
                 request.quality.c_str(),
                 profile.width,
                 profile.height,
                 static_cast<unsigned int>(profile.width),
                 static_cast<unsigned int>(profile.height),
                 profile.bitrate,
                 request.call_id.c_str());
    request_offer("start");
    return true;
}

bool LiveWebRtcSession::switch_quality(const std::string &trace_id,
                                       const std::string &quality,
                                       QualitySwitchCallback callback,
                                       std::string *error_code) {
    if (quality != "360p" && quality != "720p" &&
        quality != "1080p" && quality != "1440p") {
        if (error_code) *error_code = "INVALID_QUALITY";
        return false;
    }

    GstElement *appsrc = nullptr;
    GstElement *encoder = nullptr;
    QualityProfile profile = profile_for_quality(quality);
    std::string previous_quality;
    std::string call_id;
    uint32_t pixfmt = 0;
    bool unchanged = false;
    {
        std::lock_guard<std::mutex> lock(mtx_);
        if (!webrtc_ || !appsrc_ || !video_encoder_ || !active_published_) {
            if (error_code) *error_code = "CALL_NOT_ACTIVE";
            return false;
        }
        if (pending_quality_switch_) {
            if (error_code) *error_code = "LIVE_QUALITY_SWITCH_IN_PROGRESS";
            return false;
        }
        previous_quality = current_.quality;
        call_id = current_.call_id;
        pixfmt = appsrc_pixfmt_;
        unchanged = previous_quality == quality;
        if (!unchanged) {
            pending_quality_switch_ = PendingQualitySwitch{
                trace_id, previous_quality, quality, std::move(callback)};
            quality_switch_deadline_ =
                std::chrono::steady_clock::now() + std::chrono::seconds(5);
            quality_switch_answer_applied_ = false;
            current_.trace_id = trace_id;
            current_.quality = quality;
            appsrc_width_ = static_cast<uint32_t>(profile.width);
            appsrc_height_ = static_cast<uint32_t>(profile.height);
            appsrc_frame_size_ = raw_frame_size(
                appsrc_pixfmt_, appsrc_width_, appsrc_height_);
            offer_requested_ = false;
            video_paused_.store(true);
            appsrc = appsrc_;
            encoder = video_encoder_;
            gst_object_ref(appsrc);
            gst_object_ref(encoder);
        }
    }
    quality_switch_cv_.notify_all();
    if (unchanged) {
        if (callback) callback(true, quality, previous_quality, "");
        return true;
    }

    gst_object_unref(encoder);
    gst_object_unref(appsrc);

    if (!reconfigure_video_branch(profile, pixfmt, "quality-switch")) {
        std::fprintf(stderr,
                     "[live] quality switch video branch reconfigure failed call_id=%s %s -> %s\n",
                     call_id.c_str(),
                     previous_quality.c_str(),
                     quality.c_str());
        finish_quality_switch(false, "LIVE_QUALITY_SWITCH_FAILED");
        return true;
    }

    std::fprintf(stdout,
                 "[live] quality switch requested call_id=%s %s -> %s (%dx%d bps=%d)\n",
                 call_id.c_str(),
                 previous_quality.c_str(),
                 quality.c_str(),
                 profile.width,
                 profile.height,
                 profile.bitrate);
    request_offer("quality-switch");
    return true;
}

void LiveWebRtcSession::stop() {
    unregister_audio_consumer();

    GstElement *pipeline = nullptr;
    GstElement *webrtc = nullptr;
    GstElement *appsrc = nullptr;
    GstElement *video_encoder = nullptr;
    GstElement *audio_appsrc = nullptr;
    GstElement *remote_audio_queue = nullptr;
    GstElement *remote_audio_sink = nullptr;
    GstPad *webrtc_sink_pad = nullptr;
    GstPad *webrtc_audio_sink_pad = nullptr;
    GMainLoop *main_loop = nullptr;
    guint bus_watch_id = 0;
    gulong rtp_probe_id = 0;
    gulong audio_rtp_probe_id = 0;
    guint64 rtp_buffer_count = 0;
    guint64 rtp_bytes = 0;
    guint64 audio_rtp_buffer_count = 0;
    guint64 audio_rtp_bytes = 0;
    uint64_t audio_input_count = 0;
    uint64_t remote_audio_count = 0;
    uint64_t video_input_count = 0;
    std::thread loop_thread;
    std::thread frame_thread;
    std::thread quality_switch_watchdog_thread;

    frame_stop_.store(true);
    {
        std::lock_guard<std::mutex> lock(mtx_);
        pipeline = pipeline_;
        webrtc = webrtc_;
        appsrc = appsrc_;
        video_encoder = video_encoder_;
        audio_appsrc = audio_appsrc_;
        remote_audio_queue = remote_audio_queue_;
        remote_audio_sink = remote_audio_sink_;
        webrtc_sink_pad = webrtc_sink_pad_;
        webrtc_audio_sink_pad = webrtc_audio_sink_pad_;
        main_loop = main_loop_;
        bus_watch_id = bus_watch_id_;
        rtp_probe_id = rtp_probe_id_;
        audio_rtp_probe_id = audio_rtp_probe_id_;
        rtp_buffer_count = rtp_buffer_count_;
        rtp_bytes = rtp_bytes_;
        audio_rtp_buffer_count = audio_rtp_buffer_count_;
        audio_rtp_bytes = audio_rtp_bytes_;
        audio_input_count = audio_input_count_.load();
        remote_audio_count = remote_audio_count_.load();
        video_input_count = video_input_count_.load();
        pipeline_ = nullptr;
        webrtc_ = nullptr;
        appsrc_ = nullptr;
        video_encoder_ = nullptr;
        audio_appsrc_ = nullptr;
        remote_audio_queue_ = nullptr;
        remote_audio_sink_ = nullptr;
        webrtc_sink_pad_ = nullptr;
        webrtc_audio_sink_pad_ = nullptr;
        main_loop_ = nullptr;
        bus_watch_id_ = 0;
        rtp_probe_id_ = 0;
        audio_rtp_probe_id_ = 0;
        rtp_buffer_count_ = 0;
        rtp_bytes_ = 0;
        audio_rtp_buffer_count_ = 0;
        audio_rtp_bytes_ = 0;
        appsrc_width_ = 0;
        appsrc_height_ = 0;
        appsrc_pixfmt_ = 0;
        appsrc_frame_size_ = 0;
        active_published_ = false;
        offer_requested_ = false;
        pending_quality_switch_.reset();
        quality_switch_deadline_ = {};
        quality_switch_answer_applied_ = false;
        quality_switch_watchdog_stop_ = true;
        video_paused_.store(false);
        if (frame_thread_.joinable()) {
            frame_thread = std::move(frame_thread_);
        }
        if (quality_switch_watchdog_thread_.joinable()) {
            quality_switch_watchdog_thread =
                std::move(quality_switch_watchdog_thread_);
        }
    }

    quality_switch_cv_.notify_all();
    if (quality_switch_watchdog_thread.joinable()) {
        quality_switch_watchdog_thread.join();
    }
    if (frame_thread.joinable()) frame_thread.join();
    if (appsrc) gst_app_src_end_of_stream(GST_APP_SRC(appsrc));
    if (audio_appsrc) gst_app_src_end_of_stream(GST_APP_SRC(audio_appsrc));
    if (webrtc_sink_pad && rtp_probe_id != 0) {
        gst_pad_remove_probe(webrtc_sink_pad, rtp_probe_id);
    }
    if (webrtc_audio_sink_pad && audio_rtp_probe_id != 0) {
        gst_pad_remove_probe(webrtc_audio_sink_pad, audio_rtp_probe_id);
    }
    if (bus_watch_id != 0) {
        g_source_remove(bus_watch_id);
    }
    std::fprintf(stderr,
                 "[live] media final video_in=%llu video_rtp_buffers=%llu video_rtp_bytes=%llu "
                 "audio_in=%llu audio_rtp_buffers=%llu audio_rtp_bytes=%llu remote_audio=%llu\n",
                 static_cast<unsigned long long>(video_input_count),
                 static_cast<unsigned long long>(rtp_buffer_count),
                 static_cast<unsigned long long>(rtp_bytes),
                 static_cast<unsigned long long>(audio_input_count),
                 static_cast<unsigned long long>(audio_rtp_buffer_count),
                 static_cast<unsigned long long>(audio_rtp_bytes),
                 static_cast<unsigned long long>(remote_audio_count));
    if (remote_audio_sink) g_signal_handlers_disconnect_by_data(remote_audio_sink, this);
    if (pipeline) gst_element_set_state(pipeline, GST_STATE_NULL);
    if (main_loop) g_main_loop_quit(main_loop);
    if (loop_thread_.joinable()) {
        loop_thread = std::move(loop_thread_);
    }
    if (loop_thread.joinable()) loop_thread.join();
    if (main_loop) g_main_loop_unref(main_loop);
    if (webrtc && webrtc_audio_sink_pad) {
        gst_element_release_request_pad(webrtc, webrtc_audio_sink_pad);
        gst_object_unref(webrtc_audio_sink_pad);
    }
    if (webrtc && webrtc_sink_pad) {
        gst_element_release_request_pad(webrtc, webrtc_sink_pad);
        gst_object_unref(webrtc_sink_pad);
    }
    if (audio_appsrc) gst_object_unref(audio_appsrc);
    if (remote_audio_queue) gst_object_unref(remote_audio_queue);
    if (remote_audio_sink) gst_object_unref(remote_audio_sink);
    if (appsrc) gst_object_unref(appsrc);
    if (video_encoder) gst_object_unref(video_encoder);
    if (webrtc) gst_object_unref(webrtc);
    if (pipeline) gst_object_unref(pipeline);
}
void LiveWebRtcSession::set_frame_provider(FrameProvider provider) {
    std::lock_guard<std::mutex> lock(mtx_);
    frame_provider_ = std::move(provider);
}


void LiveWebRtcSession::set_audio_manager(AudioCaptureManager *manager) {
    std::lock_guard<std::mutex> lock(mtx_);
    audio_manager_ = manager;
}

void LiveWebRtcSession::unregister_audio_consumer() {
    AudioCaptureManager *manager = nullptr;
    size_t consumer_id = 0;
    {
        std::lock_guard<std::mutex> lock(mtx_);
        manager = audio_manager_;
        consumer_id = audio_consumer_id_;
        audio_consumer_id_ = 0;
    }
    if (manager && consumer_id != 0) {
        manager->unregister_consumer(consumer_id);
    }
}

void LiveWebRtcSession::push_audio_frame(const AudioFrame &frame) {
    if (!frame.data || frame.size == 0) return;

    GstElement *audio_appsrc = nullptr;
    {
        std::lock_guard<std::mutex> lock(mtx_);
        audio_appsrc = audio_appsrc_;
        if (audio_appsrc) gst_object_ref(audio_appsrc);
    }
    if (!audio_appsrc) return;

    GstBuffer *buffer = gst_buffer_new_allocate(nullptr, frame.size, nullptr);
    if (!buffer) {
        gst_object_unref(audio_appsrc);
        return;
    }

    GstMapInfo map;
    if (!gst_buffer_map(buffer, &map, GST_MAP_WRITE)) {
        gst_buffer_unref(buffer);
        gst_object_unref(audio_appsrc);
        return;
    }
    std::memcpy(map.data, frame.data, frame.size);
    gst_buffer_unmap(buffer, &map);

    const uint64_t rebased_pts_ns = audio_timestamp_rebaser_.rebase(frame.pts_ns, frame.duration_ns);
    GST_BUFFER_PTS(buffer) = static_cast<GstClockTime>(rebased_pts_ns);
    GST_BUFFER_DTS(buffer) = static_cast<GstClockTime>(rebased_pts_ns);
    GST_BUFFER_DURATION(buffer) = static_cast<GstClockTime>(frame.duration_ns);

    const uint64_t count = audio_input_count_.fetch_add(1) + 1;
    if (count == 1 || count % 2000 == 0) {
        std::fprintf(stderr,
                     "[live] audio input count=%llu source_pts_ns=%llu rebased_pts_ns=%llu bytes=%zu\n",
                     static_cast<unsigned long long>(count),
                     static_cast<unsigned long long>(frame.pts_ns),
                     static_cast<unsigned long long>(rebased_pts_ns),
                     frame.size);
    }

    GstFlowReturn flow_ret = gst_app_src_push_buffer(GST_APP_SRC(audio_appsrc), buffer);
    if (flow_ret != GST_FLOW_OK && flow_ret != GST_FLOW_FLUSHING) {
        std::fprintf(stderr, "[live] audio appsrc push failed: %d\n", flow_ret);
    }
    gst_object_unref(audio_appsrc);
}

GstFlowReturn LiveWebRtcSession::on_remote_audio_sample(GstElement *sink) {
    GstSample *sample = gst_app_sink_pull_sample(GST_APP_SINK(sink));
    if (!sample) return GST_FLOW_OK;

    AudioCaptureManager *manager = nullptr;
    {
        std::lock_guard<std::mutex> lock(mtx_);
        manager = audio_manager_;
    }

    GstBuffer *buffer = gst_sample_get_buffer(sample);
    GstMapInfo map;
    bool pushed = false;
    if (manager && buffer && gst_buffer_map(buffer, &map, GST_MAP_READ)) {
        AudioFrame frame;
        frame.data = map.data;
        frame.size = map.size;
        frame.duration_ns = GST_CLOCK_TIME_IS_VALID(GST_BUFFER_DURATION(buffer))
            ? GST_BUFFER_DURATION(buffer)
            : 20ULL * 1000ULL * 1000ULL;
        pushed = manager->push_remote_playback_frame(frame);
        gst_buffer_unmap(buffer, &map);
    }
    gst_sample_unref(sample);

    if (pushed) {
        const uint64_t count = remote_audio_count_.fetch_add(1) + 1;
        if (count == 1 || count % 500 == 0) {
            std::fprintf(stderr, "[live] remote audio playback count=%llu\n",
                         static_cast<unsigned long long>(count));
        }
    }
    return GST_FLOW_OK;
}

void LiveWebRtcSession::on_incoming_pad_added(GstPad *pad) {
    if (!pad) return;

    GstCaps *caps = gst_pad_get_current_caps(pad);
    if (!caps) caps = gst_pad_query_caps(pad, nullptr);

    bool is_audio = false;
    std::string caps_text = caps_to_string(caps);
    if (caps && gst_caps_get_size(caps) > 0) {
        const GstStructure *structure = gst_caps_get_structure(caps, 0);
        const char *media = gst_structure_get_string(structure, "media");
        const char *encoding = gst_structure_get_string(structure, "encoding-name");
        is_audio = media && g_ascii_strcasecmp(media, "audio") == 0 &&
                   (!encoding || g_ascii_strcasecmp(encoding, "OPUS") == 0);
    }
    if (caps) gst_caps_unref(caps);

    if (!is_audio) {
        std::fprintf(stderr, "[live] ignore incoming non-audio pad caps=%s\n", caps_text.c_str());
        return;
    }

    GstElement *remote_audio_queue = nullptr;
    {
        std::lock_guard<std::mutex> lock(mtx_);
        remote_audio_queue = remote_audio_queue_;
        if (remote_audio_queue) gst_object_ref(remote_audio_queue);
    }
    if (!remote_audio_queue) return;

    GstPad *sink_pad = gst_element_get_static_pad(remote_audio_queue, "sink");
    if (!sink_pad) {
        gst_object_unref(remote_audio_queue);
        return;
    }
    if (gst_pad_is_linked(sink_pad)) {
        gst_object_unref(sink_pad);
        gst_object_unref(remote_audio_queue);
        return;
    }

    GstPadLinkReturn link_ret = gst_pad_link(pad, sink_pad);
    std::fprintf(stderr,
                 "[live] incoming audio pad link result=%s caps=%s\n",
                 gst_pad_link_get_name(link_ret),
                 caps_text.c_str());
    gst_object_unref(sink_pad);
    gst_object_unref(remote_audio_queue);
}
void LiveWebRtcSession::handle_signal(const std::string &payload) {
    try {
        const auto signal = nlohmann::json::parse(payload);
        if (signal.value("sender", "") == "device") return;
        const std::string type = signal.value("signal_type", "");
        std::fprintf(stderr,
                     "[live] handle signal type=%s call_id=%s sender=%s\n",
                     type.c_str(),
                     signal.value("call_id", "").c_str(),
                     signal.value("sender", "").c_str());
        if (type == "answer") {
            handle_answer(signal);
        } else if (type == "candidate") {
            handle_candidate(signal);
        }
    } catch (const std::exception &e) {
        std::fprintf(stderr, "[live] invalid signal payload: %s\n", e.what());
    }
}

void LiveWebRtcSession::on_bus_message(GstMessage *message) {
    if (!message) return;
    const char *source = message_source_name(message);
    switch (GST_MESSAGE_TYPE(message)) {
        case GST_MESSAGE_ERROR: {
            GError *error = nullptr;
            gchar *debug = nullptr;
            gst_message_parse_error(message, &error, &debug);
            const bool video_branch_error = is_video_branch_source(source);
            std::fprintf(stderr,
                         "[live] bus error source=%s message=%s debug=%s\n",
                         source,
                         error ? error->message : "(none)",
                         debug ? debug : "(none)");
            if (error) g_error_free(error);
            if (debug) g_free(debug);
            if (video_branch_error &&
                !video_branch_reconfiguring_.load() &&
                !frame_stop_.load()) {
                // 编码支路异常后必须立即停帧，避免 appsrc 队列继续吞入整帧导致 OOM。
                video_paused_.store(true);
                bool switching = false;
                {
                    std::lock_guard<std::mutex> lock(mtx_);
                    switching = pending_quality_switch_.has_value();
                }
                if (switching) {
                    finish_quality_switch(
                        false, "LIVE_QUALITY_SWITCH_FAILED");
                } else {
                    std::fprintf(
                        stderr,
                        "[live] video branch paused after fatal error source=%s; audio remains active\n",
                        source);
                }
            }
            break;
        }
        case GST_MESSAGE_WARNING: {
            GError *error = nullptr;
            gchar *debug = nullptr;
            gst_message_parse_warning(message, &error, &debug);
            std::fprintf(stderr,
                         "[live] bus warning source=%s message=%s debug=%s\n",
                         source,
                         error ? error->message : "(none)",
                         debug ? debug : "(none)");
            if (error) g_error_free(error);
            if (debug) g_free(debug);
            break;
        }
        case GST_MESSAGE_STATE_CHANGED: {
            GstState old_state = GST_STATE_NULL;
            GstState new_state = GST_STATE_NULL;
            GstState pending = GST_STATE_NULL;
            gst_message_parse_state_changed(message, &old_state, &new_state, &pending);
            std::fprintf(stderr,
                         "[live] bus state source=%s %s->%s pending=%s\n",
                         source,
                         gst_element_state_get_name(old_state),
                         gst_element_state_get_name(new_state),
                         gst_element_state_get_name(pending));
            break;
        }
        case GST_MESSAGE_EOS:
            std::fprintf(stderr, "[live] bus eos source=%s\n", source);
            break;
        default:
            break;
    }
}

GstPadProbeReturn LiveWebRtcSession::on_rtp_probe(GstPadProbeInfo *info) {
    if (!info || !(GST_PAD_PROBE_INFO_TYPE(info) & GST_PAD_PROBE_TYPE_BUFFER)) {
        return GST_PAD_PROBE_OK;
    }
    GstBuffer *buffer = GST_PAD_PROBE_INFO_BUFFER(info);
    if (!buffer) return GST_PAD_PROBE_OK;

    const gsize size = gst_buffer_get_size(buffer);
    const GstClockTime pts = GST_BUFFER_PTS(buffer);
    const GstClockTime dts = GST_BUFFER_DTS(buffer);

    guint64 count = 0;
    guint64 total_bytes = 0;
    std::string call_id;
    bool quality_switch_ready = false;
    {
        std::lock_guard<std::mutex> lock(mtx_);
        rtp_buffer_count_ += 1;
        rtp_bytes_ += static_cast<guint64>(size);
        count = rtp_buffer_count_;
        total_bytes = rtp_bytes_;
        call_id = current_.call_id;
        quality_switch_ready =
            pending_quality_switch_.has_value() &&
            quality_switch_answer_applied_;
    }

    if (count == 1 || count % 600 == 0) {
        std::fprintf(stderr,
                     "[live] video RTP call_id=%s buffers=%llu size=%zu total_bytes=%llu pts_ns=%llu dts_ns=%llu\n",
                     call_id.c_str(),
                     static_cast<unsigned long long>(count),
                     size,
                     static_cast<unsigned long long>(total_bytes),
                     static_cast<unsigned long long>(pts),
                     static_cast<unsigned long long>(dts));
    }
    if (quality_switch_ready) {
        std::fprintf(
            stderr,
            "[live] quality switch produced first RTP packet call_id=%s\n",
            call_id.c_str());
        finish_quality_switch(true, "");
    }
    return GST_PAD_PROBE_OK;
}
GstPadProbeReturn LiveWebRtcSession::on_audio_rtp_probe(GstPadProbeInfo *info) {
    if (!info || !(GST_PAD_PROBE_INFO_TYPE(info) & GST_PAD_PROBE_TYPE_BUFFER)) {
        return GST_PAD_PROBE_OK;
    }
    GstBuffer *buffer = GST_PAD_PROBE_INFO_BUFFER(info);
    if (!buffer) return GST_PAD_PROBE_OK;

    std::lock_guard<std::mutex> lock(mtx_);
    audio_rtp_buffer_count_ += 1;
    audio_rtp_bytes_ += static_cast<guint64>(gst_buffer_get_size(buffer));
    return GST_PAD_PROBE_OK;
}

void LiveWebRtcSession::frame_push_loop() {
    constexpr uint64_t kFrameDurationNs = 1000000000ULL / 24ULL;
    uint64_t last_seq = 0;
    uint64_t first_ts_ns = 0;
    uint64_t last_pts_ns = 0;
    uint64_t pushed = 0;
    uint64_t queue_full_drops = 0;
    uint64_t dma_pool_drops = 0;
    const auto dma_pool = std::make_shared<LiveDmaPoolState>();
    GstAllocator *dma_allocator = gst_dmabuf_allocator_new();
    if (!dma_allocator) {
        std::fprintf(stderr, "[live] create DMA-BUF allocator failed\n");
        return;
    }

    while (!frame_stop_.load()) {
        if (video_paused_.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
            continue;
        }
        FrameProvider provider;
        GstElement *appsrc = nullptr;
        uint32_t expected_width = 0;
        uint32_t expected_height = 0;
        uint32_t expected_pixfmt = 0;
        size_t expected_size = 0;
        std::string call_id;
        {
            std::lock_guard<std::mutex> lock(mtx_);
            provider = frame_provider_;
            appsrc = appsrc_;
            expected_width = appsrc_width_;
            expected_height = appsrc_height_;
            expected_pixfmt = appsrc_pixfmt_;
            expected_size = appsrc_frame_size_;
            call_id = current_.call_id;
            if (appsrc) gst_object_ref(appsrc);
        }

        if (!provider || !appsrc || expected_size == 0) {
            if (appsrc) gst_object_unref(appsrc);
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            continue;
        }

        VideoFrame frame;
        if (!provider(frame)) {
            gst_object_unref(appsrc);
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            continue;
        }
        if (frame.seq == 0 || frame.seq == last_seq) {
            gst_object_unref(appsrc);
            std::this_thread::sleep_for(std::chrono::milliseconds(3));
            continue;
        }
        const size_t expected_dma_size = live_dma_frame_size(
            expected_pixfmt, expected_width, expected_height);
        if (expected_dma_size == 0) {
            std::fprintf(stderr,
                         "[live] drop camera frame reason=unsupported-dmabuf-format call_id=%s fmt=%c%c%c%c\n",
                         call_id.c_str(),
                         expected_pixfmt & 0xFF,
                         (expected_pixfmt >> 8) & 0xFF,
                         (expected_pixfmt >> 16) & 0xFF,
                         (expected_pixfmt >> 24) & 0xFF);
            last_seq = frame.seq;
            gst_object_unref(appsrc);
            continue;
        }
        guint64 current_level_bytes = 0;
        guint64 max_bytes = 0;
        g_object_get(
            G_OBJECT(appsrc),
            "current-level-bytes", &current_level_bytes,
            "max-bytes", &max_bytes,
            nullptr);
        if (max_bytes > 0 &&
            current_level_bytes + static_cast<guint64>(expected_dma_size) >
                max_bytes) {
            ++queue_full_drops;
            if (queue_full_drops == 1 || queue_full_drops % 120 == 0) {
                std::fprintf(
                    stderr,
                    "[live] drop camera frame reason=appsrc-full call_id=%s drops=%llu level_bytes=%llu max_bytes=%llu frame_bytes=%zu\n",
                    call_id.c_str(),
                    static_cast<unsigned long long>(queue_full_drops),
                    static_cast<unsigned long long>(current_level_bytes),
                    static_cast<unsigned long long>(max_bytes),
                    expected_dma_size);
            }
            last_seq = frame.seq;
            gst_object_unref(appsrc);
            std::this_thread::sleep_for(std::chrono::milliseconds(3));
            continue;
        }
        const size_t source_size =
            raw_frame_size(frame.pixfmt, frame.width, frame.height);
        if (frame.data == nullptr || frame.dmabuf_fd < 0 ||
            frame.pixfmt != expected_pixfmt ||
            source_size == 0 ||
            frame.size < source_size) {
            std::fprintf(stderr,
                         "[live] drop camera frame seq=%llu reason=format-mismatch got=%ux%u fmt=%c%c%c%c bytes=%zu expected_fmt=%c%c%c%c\n",
                         static_cast<unsigned long long>(frame.seq),
                         frame.width,
                         frame.height,
                         frame.pixfmt & 0xFF,
                         (frame.pixfmt >> 8) & 0xFF,
                         (frame.pixfmt >> 16) & 0xFF,
                         (frame.pixfmt >> 24) & 0xFF,
                         frame.size,
                         expected_pixfmt & 0xFF,
                         (expected_pixfmt >> 8) & 0xFF,
                         (expected_pixfmt >> 16) & 0xFF,
                         (expected_pixfmt >> 24) & 0xFF);
            last_seq = frame.seq;
            gst_object_unref(appsrc);
            continue;
        }

        auto dma_lease = acquire_live_dma_buffer(dma_pool, expected_dma_size);
        if (!dma_lease) {
            ++dma_pool_drops;
            if (dma_pool_drops == 1 || dma_pool_drops % 120 == 0) {
                std::fprintf(
                    stderr,
                    "[live] drop camera frame reason=dmabuf-pool-unavailable call_id=%s drops=%llu frame_bytes=%zu\n",
                    call_id.c_str(),
                    static_cast<unsigned long long>(dma_pool_drops),
                    expected_dma_size);
            }
            last_seq = frame.seq;
            gst_object_unref(appsrc);
            std::this_thread::sleep_for(std::chrono::milliseconds(3));
            continue;
        }

        int rga_status = 0;
        if (!copy_live_frame_to_dma(
                frame,
                expected_width,
                expected_height,
                dma_lease->fd,
                &rga_status)) {
            std::fprintf(stderr,
                         "[live] RGA DMA copy failed seq=%llu status=%d %ux%u fd=%d -> %ux%u fd=%d\n",
                         static_cast<unsigned long long>(frame.seq),
                         rga_status,
                         frame.width,
                         frame.height,
                         frame.dmabuf_fd,
                         expected_width,
                         expected_height,
                         dma_lease->fd);
            release_live_dma_buffer(dma_lease.release());
            last_seq = frame.seq;
            gst_object_unref(appsrc);
            continue;
        }

        const int gst_memory_fd = ::dup(dma_lease->fd);
        if (gst_memory_fd < 0) {
            std::fprintf(stderr,
                         "[live] duplicate DMA-BUF fd failed fd=%d errno=%d error=%s\n",
                         dma_lease->fd,
                         errno,
                         std::strerror(errno));
            release_live_dma_buffer(dma_lease.release());
            gst_object_unref(appsrc);
            continue;
        }
        GstMemory *memory = gst_dmabuf_allocator_alloc(
            dma_allocator, gst_memory_fd, expected_dma_size);
        if (!memory) {
            ::close(gst_memory_fd);
            release_live_dma_buffer(dma_lease.release());
            gst_object_unref(appsrc);
            continue;
        }
        GstBuffer *buffer = gst_buffer_new();
        if (!buffer) {
            gst_memory_unref(memory);
            release_live_dma_buffer(dma_lease.release());
            gst_object_unref(appsrc);
            continue;
        }
        gst_buffer_append_memory(buffer, memory);

        gsize offsets[GST_VIDEO_MAX_PLANES] = {};
        gint strides[GST_VIDEO_MAX_PLANES] = {};
        const uint32_t output_hstride = align_live_height(expected_height);
        offsets[0] = 0;
        offsets[1] = static_cast<gsize>(expected_width) * output_hstride;
        strides[0] = static_cast<gint>(expected_width);
        strides[1] = static_cast<gint>(expected_width);
        if (!gst_buffer_add_video_meta_full(
                buffer,
                GST_VIDEO_FRAME_FLAG_NONE,
                gst_video_format_for_pixfmt(expected_pixfmt),
                expected_width,
                expected_height,
                2,
                offsets,
                strides)) {
            gst_buffer_unref(buffer);
            release_live_dma_buffer(dma_lease.release());
            gst_object_unref(appsrc);
            continue;
        }
        const int output_dma_fd = dma_lease->fd;
        gst_mini_object_weak_ref(
            GST_MINI_OBJECT(memory),
            on_live_dma_buffer_released,
            dma_lease.release());

        uint64_t pts_ns = pushed * kFrameDurationNs;
        if (frame.ts_ns != 0) {
            if (first_ts_ns == 0) {
                first_ts_ns = frame.ts_ns;
                pts_ns = 0;
            } else if (frame.ts_ns >= first_ts_ns) {
                pts_ns = frame.ts_ns - first_ts_ns;
            }
        }
        if (pts_ns < last_pts_ns) {
            pts_ns = last_pts_ns + 1;
        }
        uint64_t duration_ns = pushed == 0 || pts_ns <= last_pts_ns
            ? kFrameDurationNs
            : pts_ns - last_pts_ns;
        GST_BUFFER_PTS(buffer) = static_cast<GstClockTime>(pts_ns);
        GST_BUFFER_DTS(buffer) = static_cast<GstClockTime>(pts_ns);
        GST_BUFFER_DURATION(buffer) = static_cast<GstClockTime>(duration_ns);

        GstFlowReturn flow_ret = gst_app_src_push_buffer(GST_APP_SRC(appsrc), buffer);
        gst_object_unref(appsrc);
        last_seq = frame.seq;
        last_pts_ns = pts_ns;
        ++pushed;
        video_input_count_.store(pushed);

        if (pushed <= 5 || pushed % 600 == 0 || flow_ret != GST_FLOW_OK) {
            std::fprintf(stderr,
                         "[live] appsrc push DMA-BUF call_id=%s seq=%llu count=%llu bytes=%zu fd=%d flow=%d pts_ns=%llu\n",
                         call_id.c_str(),
                         static_cast<unsigned long long>(frame.seq),
                         static_cast<unsigned long long>(pushed),
                         expected_dma_size,
                         output_dma_fd,
                         static_cast<int>(flow_ret),
                         static_cast<unsigned long long>(pts_ns));
        }
        if (flow_ret != GST_FLOW_OK && flow_ret != GST_FLOW_FLUSHING) {
            // 非临时流错误后停止继续入队；切档中则立即回滚旧清晰度。
            video_paused_.store(true);
            bool switching = false;
            {
                std::lock_guard<std::mutex> lock(mtx_);
                switching = pending_quality_switch_.has_value();
            }
            if (switching) {
                finish_quality_switch(
                    false, "LIVE_QUALITY_SWITCH_FAILED");
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }
    gst_object_unref(dma_allocator);
}
void LiveWebRtcSession::publish_signal(const std::string &signal_type,
                                       const std::string *sdp,
                                       const nlohmann::json *candidate) {
    StartRequest current;
    {
        std::lock_guard<std::mutex> lock(mtx_);
        current = current_;
    }

    nlohmann::json out;
    out["sender"] = "device";
    out["trace_id"] = current.trace_id;
    out["call_id"] = current.call_id;
    out["device_id"] = current.device_id;
    out["signal_type"] = signal_type;
    out["sdp"] = sdp ? nlohmann::json(*sdp) : nlohmann::json(nullptr);
    out["candidate"] = candidate ? *candidate : nlohmann::json(nullptr);
    std::fprintf(stderr,
                 "[live] publish signal type=%s call_id=%s sdp_len=%zu has_candidate=%d\n",
                 signal_type.c_str(),
                 current.call_id.c_str(),
                 sdp ? sdp->size() : 0,
                 candidate ? 1 : 0);
    signal_publisher_(out.dump());
}

void LiveWebRtcSession::publish_state(const std::string &media_state) {
    StartRequest current;
    {
        std::lock_guard<std::mutex> lock(mtx_);
        current = current_;
    }
    std::fprintf(stderr,
                 "[live] publish state media_state=%s call_id=%s\n",
                 media_state.c_str(),
                 current.call_id.c_str());
    state_publisher_(current.trace_id, current.call_id, media_state);
}

void LiveWebRtcSession::request_offer(const char *reason) {
    GstElement *webrtc = nullptr;
    StartRequest current;
    bool already_requested = false;
    {
        std::lock_guard<std::mutex> lock(mtx_);
        webrtc = webrtc_;
        current = current_;
        if (offer_requested_) {
            already_requested = true;
        } else {
            offer_requested_ = true;
        }
        if (webrtc) gst_object_ref(webrtc);
    }

    if (!webrtc) {
        std::fprintf(stderr, "[live] create offer skipped reason=%s no-webrtc\n", reason ? reason : "unknown");
        return;
    }
    if (already_requested) {
        std::fprintf(stderr,
                     "[live] create offer skipped reason=%s call_id=%s already-requested\n",
                     reason ? reason : "unknown",
                     current.call_id.c_str());
        gst_object_unref(webrtc);
        return;
    }

    std::fprintf(stderr,
                 "[live] create offer requested reason=%s call_id=%s\n",
                 reason ? reason : "unknown",
                 current.call_id.c_str());
    GstPromise *promise = gst_promise_new_with_change_func(on_offer_created_cb, this, nullptr);
    g_signal_emit_by_name(webrtc, "create-offer", nullptr, promise);
    gst_object_unref(webrtc);
}

void LiveWebRtcSession::on_negotiation_needed() {
    std::fprintf(stderr, "[live] negotiation needed\n");
    request_offer("negotiation-needed");
}

void LiveWebRtcSession::on_offer_created(GstPromise *promise) {
    GstElement *webrtc = nullptr;
    {
        std::lock_guard<std::mutex> lock(mtx_);
        webrtc = webrtc_;
    }
    if (!webrtc) {
        gst_promise_unref(promise);
        return;
    }

    const GstStructure *reply = gst_promise_get_reply(promise);
    if (!reply) {
        std::fprintf(stderr, "[live] offer promise has no reply\n");
        gst_promise_unref(promise);
        finish_quality_switch(false, "LIVE_QUALITY_SWITCH_FAILED");
        return;
    }
    GstWebRTCSessionDescription *offer = nullptr;
    gst_structure_get(reply, "offer", GST_TYPE_WEBRTC_SESSION_DESCRIPTION, &offer, nullptr);
    gst_promise_unref(promise);
    if (!offer) {
        std::fprintf(stderr, "[live] offer promise reply has no offer\n");
        finish_quality_switch(false, "LIVE_QUALITY_SWITCH_FAILED");
        return;
    }

    GstPromise *local_promise = gst_promise_new();
    g_signal_emit_by_name(webrtc, "set-local-description", offer, local_promise);
    gst_promise_interrupt(local_promise);
    gst_promise_unref(local_promise);

    gchar *sdp_text = gst_sdp_message_as_text(offer->sdp);
    if (sdp_text) {
        std::string sdp(sdp_text);
        std::fprintf(stderr, "[live] offer created sdp_len=%zu\n", sdp.size());
        log_sdp_candidate_lines("local-offer", sdp);
        publish_signal("offer", &sdp, nullptr);
        g_free(sdp_text);
    }
    else {
        std::fprintf(stderr, "[live] offer created but SDP text is null\n");
    }
    gst_webrtc_session_description_free(offer);
}

void LiveWebRtcSession::on_ice_candidate(unsigned int mline_index, const char *candidate) {
    if (!candidate || candidate[0] == '\0') return;
    const std::string candidate_text(candidate);
    std::fprintf(stderr,
                 "[live] ICE local candidate mline=%u len=%zu %s\n",
                 mline_index,
                 candidate_text.size(),
                 summarize_ice_candidate(candidate_text).c_str());
    nlohmann::json value;
    value["candidate"] = candidate_text;
    value["sdpMid"] = "0";
    value["sdpMLineIndex"] = mline_index;
    publish_signal("candidate", nullptr, &value);
}

void LiveWebRtcSession::on_ice_connection_state_changed() {
    GstElement *webrtc = nullptr;
    {
        std::lock_guard<std::mutex> lock(mtx_);
        webrtc = webrtc_;
    }
    if (!webrtc) return;

    GstWebRTCICEConnectionState state;
    g_object_get(webrtc, "ice-connection-state", &state, nullptr);
    std::fprintf(stderr, "[live] ICE state changed state=%s\n", ice_connection_state_name(state));
    if (state == GST_WEBRTC_ICE_CONNECTION_STATE_CONNECTED ||
        state == GST_WEBRTC_ICE_CONNECTION_STATE_COMPLETED) {
        bool should_publish = false;
        {
            std::lock_guard<std::mutex> lock(mtx_);
            if (!active_published_) {
                active_published_ = true;
                should_publish = true;
            }
        }
        if (should_publish) publish_state("active");
    } else if (state == GST_WEBRTC_ICE_CONNECTION_STATE_FAILED ||
               state == GST_WEBRTC_ICE_CONNECTION_STATE_DISCONNECTED ||
               state == GST_WEBRTC_ICE_CONNECTION_STATE_CLOSED) {
        publish_state("idle");
    }
}

bool LiveWebRtcSession::reconfigure_video_branch(
    const QualityProfile &profile,
    uint32_t pixfmt,
    const char *reason) {
    std::lock_guard<std::mutex> branch_lock(video_branch_mtx_);
    video_branch_reconfiguring_.store(true);

    GstElement *pipeline = nullptr;
    {
        std::lock_guard<std::mutex> lock(mtx_);
        pipeline = pipeline_;
        if (pipeline) gst_object_ref(pipeline);
    }
    if (!pipeline) {
        video_branch_reconfiguring_.store(false);
        return false;
    }

    // 只重置视频支路，webrtcbin 和音频支路始终保持 PLAYING。
    const char *element_names[] = {
        "rtp_queue",
        "live_pay",
        "live_parse",
        "live_enc",
        "raw_queue",
        "live_appsrc",
    };
    std::vector<GstElement *> elements;
    elements.reserve(sizeof(element_names) / sizeof(element_names[0]));
    bool ok = true;
    for (const char *name : element_names) {
        GstElement *element = gst_bin_get_by_name(GST_BIN(pipeline), name);
        if (!element) {
            std::fprintf(stderr,
                         "[live] video branch element missing name=%s reason=%s\n",
                         name,
                         reason ? reason : "unknown");
            ok = false;
            break;
        }
        elements.push_back(element);
    }

    if (ok) {
        for (GstElement *element : elements) {
            const GstStateChangeReturn ret =
                gst_element_set_state(element, GST_STATE_READY);
            if (ret == GST_STATE_CHANGE_FAILURE) {
                std::fprintf(stderr,
                             "[live] set video element READY failed name=%s reason=%s\n",
                             GST_OBJECT_NAME(element),
                             reason ? reason : "unknown");
                ok = false;
            }
        }
    }

    if (ok) {
        GstElement *appsrc = elements.back();
        GstElement *encoder = elements[3];
        const char *format = gst_format_for_pixfmt(pixfmt);
        const size_t frame_size = raw_frame_size(
            pixfmt,
            static_cast<uint32_t>(profile.width),
            static_cast<uint32_t>(profile.height));
        const size_t dma_frame_size = live_dma_frame_size(
            pixfmt,
            static_cast<uint32_t>(profile.width),
            static_cast<uint32_t>(profile.height));
        if (!format || frame_size == 0 || dma_frame_size == 0) {
            ok = false;
        } else {
            GstCaps *caps = gst_caps_new_simple(
                "video/x-raw",
                "format", G_TYPE_STRING, format,
                "width", G_TYPE_INT, profile.width,
                "height", G_TYPE_INT, profile.height,
                "framerate", GST_TYPE_FRACTION, 24, 1,
                nullptr);
            gst_app_src_set_caps(GST_APP_SRC(appsrc), caps);
            gst_caps_unref(caps);
            g_object_set(
                G_OBJECT(appsrc),
                "max-bytes", static_cast<guint64>(dma_frame_size) * 2,
                nullptr);
            g_object_set(
                G_OBJECT(encoder),
                "bps", profile.bitrate,
                nullptr);
        }
    }

    if (ok) {
        // 下游先恢复，避免上游开始送帧时编码链路还未就绪。
        for (GstElement *element : elements) {
            if (!gst_element_sync_state_with_parent(element)) {
                std::fprintf(stderr,
                             "[live] restore video element state failed name=%s reason=%s\n",
                             GST_OBJECT_NAME(element),
                             reason ? reason : "unknown");
                ok = false;
            }
        }
    }

    for (GstElement *element : elements) {
        gst_object_unref(element);
    }
    gst_object_unref(pipeline);
    video_branch_reconfiguring_.store(false);

    std::fprintf(stderr,
                 "[live] video branch reconfigure reason=%s result=%s size=%dx%d bps=%d\n",
                 reason ? reason : "unknown",
                 ok ? "ok" : "failed",
                 profile.width,
                 profile.height,
                 profile.bitrate);
    return ok;
}

void LiveWebRtcSession::request_video_keyframe() {
    GstElement *encoder = nullptr;
    {
        std::lock_guard<std::mutex> lock(mtx_);
        encoder = video_encoder_;
        if (encoder) gst_object_ref(encoder);
    }
    if (!encoder) return;
    GstStructure *structure = gst_structure_new(
        "GstForceKeyUnit",
        "all-headers", G_TYPE_BOOLEAN, TRUE,
        "count", G_TYPE_UINT, 0,
        nullptr);
    GstEvent *event = gst_event_new_custom(
        GST_EVENT_CUSTOM_UPSTREAM, structure);
    const gboolean sent = gst_element_send_event(encoder, event);
    std::fprintf(stderr, "[live] force video keyframe sent=%d\n", sent ? 1 : 0);
    gst_object_unref(encoder);
}

void LiveWebRtcSession::finish_quality_switch(bool ok,
                                              const std::string &error_code) {
    std::optional<PendingQualitySwitch> pending;
    QualityProfile rollback_profile{};
    uint32_t pixfmt = 0;
    {
        std::lock_guard<std::mutex> lock(mtx_);
        if (!pending_quality_switch_) return;
        pending = std::move(pending_quality_switch_);
        pending_quality_switch_.reset();
        quality_switch_deadline_ = {};
        quality_switch_answer_applied_ = false;
        offer_requested_ = false;
        if (!ok) {
            rollback_profile = profile_for_quality(pending->previous_quality);
            current_.quality = pending->previous_quality;
            appsrc_width_ = static_cast<uint32_t>(rollback_profile.width);
            appsrc_height_ = static_cast<uint32_t>(rollback_profile.height);
            appsrc_frame_size_ = raw_frame_size(
                appsrc_pixfmt_, appsrc_width_, appsrc_height_);
            pixfmt = appsrc_pixfmt_;
        }
    }

    bool video_ready = ok;
    if (!ok) {
        video_ready = reconfigure_video_branch(
            rollback_profile, pixfmt, "quality-switch-rollback");
        if (!video_ready) {
            std::fprintf(
                stderr,
                "[live] rollback video branch failed; keep video paused previous_quality=%s\n",
                pending->previous_quality.c_str());
        }
    }

    quality_switch_cv_.notify_all();
    if (video_ready) {
        request_video_keyframe();
        video_paused_.store(false);
    }
    if (pending->callback) {
        pending->callback(
            ok,
            pending->quality,
            pending->previous_quality,
            ok ? "" : error_code);
    }
}

void LiveWebRtcSession::quality_switch_watchdog_loop() {
    std::unique_lock<std::mutex> lock(mtx_);
    while (!quality_switch_watchdog_stop_) {
        if (!pending_quality_switch_) {
            quality_switch_cv_.wait(lock, [this] {
                return quality_switch_watchdog_stop_ ||
                       pending_quality_switch_.has_value();
            });
            continue;
        }

        const auto deadline = quality_switch_deadline_;
        const bool changed = quality_switch_cv_.wait_until(
            lock, deadline, [this, deadline] {
                return quality_switch_watchdog_stop_ ||
                       !pending_quality_switch_ ||
                       quality_switch_deadline_ != deadline;
            });
        if (changed) continue;

        lock.unlock();
        std::fprintf(stderr,
                     "[live] quality switch timed out, rolling back\n");
        finish_quality_switch(false, "LIVE_QUALITY_SWITCH_FAILED");
        lock.lock();
    }
}

void LiveWebRtcSession::on_remote_description_set_cb(
    GstPromise *promise,
    gpointer user_data) {
    std::unique_ptr<RemoteDescriptionContext> context(
        static_cast<RemoteDescriptionContext *>(user_data));
    if (!context || !context->session) {
        gst_promise_unref(promise);
        return;
    }
    context->session->on_remote_description_set(
        promise, context->call_id);
}

void LiveWebRtcSession::on_remote_description_set(
    GstPromise *promise,
    const std::string &call_id) {
    // change_func 被调用时 Promise 已结束，wait 只读取结果，不会阻塞 MQTT 回调线程。
    const GstPromiseResult result = gst_promise_wait(promise);
    gst_promise_unref(promise);

    bool current_session = false;
    {
        std::lock_guard<std::mutex> lock(mtx_);
        current_session =
            webrtc_ != nullptr && current_.call_id == call_id;
    }
    if (!current_session) {
        std::fprintf(
            stderr,
            "[live] ignore stale remote answer completion call_id=%s result=%d\n",
            call_id.c_str(),
            static_cast<int>(result));
        return;
    }
    if (result == GST_PROMISE_RESULT_REPLIED) {
        std::fprintf(
            stderr,
            "[live] set remote answer completed call_id=%s\n",
            call_id.c_str());
        bool switching = false;
        {
            std::lock_guard<std::mutex> lock(mtx_);
            if (pending_quality_switch_) {
                quality_switch_answer_applied_ = true;
                switching = true;
            }
        }
        if (switching) {
            // 等新分辨率实际产出首包 RTP 后再确认成功；首帧协商失败仍可回滚。
            request_video_keyframe();
            video_paused_.store(false);
        }
        return;
    }

    std::fprintf(stderr,
                 "[live] set remote answer failed call_id=%s promise_result=%d\n",
                 call_id.c_str(),
                 static_cast<int>(result));
    finish_quality_switch(false, "LIVE_QUALITY_SWITCH_FAILED");
}

void LiveWebRtcSession::handle_answer(const nlohmann::json &signal) {
    GstElement *webrtc = nullptr;
    {
        std::lock_guard<std::mutex> lock(mtx_);
        webrtc = webrtc_;
        if (webrtc) gst_object_ref(webrtc);
    }
    if (!webrtc) return;

    const std::string sdp = signal.value("sdp", "");
    const std::string call_id = signal.value("call_id", "");
    std::fprintf(stderr,
                 "[live] handle answer call_id=%s sdp_len=%zu\n",
                 call_id.c_str(),
                 sdp.size());
    log_sdp_candidate_lines("remote-answer", sdp);
    if (sdp.empty()) {
        gst_object_unref(webrtc);
        finish_quality_switch(false, "LIVE_QUALITY_SWITCH_FAILED");
        return;
    }

    GstSDPMessage *sdp_msg = nullptr;
    if (gst_sdp_message_new(&sdp_msg) != GST_SDP_OK) {
        gst_object_unref(webrtc);
        finish_quality_switch(false, "LIVE_QUALITY_SWITCH_FAILED");
        return;
    }
    GstSDPResult parse_result = gst_sdp_message_parse_buffer(
        reinterpret_cast<const guint8 *>(sdp.data()), sdp.size(), sdp_msg);
    if (parse_result != GST_SDP_OK) {
        gst_sdp_message_free(sdp_msg);
        gst_object_unref(webrtc);
        std::fprintf(stderr, "[live] parse answer SDP failed\n");
        finish_quality_switch(false, "LIVE_QUALITY_SWITCH_FAILED");
        return;
    }

    GstWebRTCSessionDescription *answer = gst_webrtc_session_description_new(GST_WEBRTC_SDP_TYPE_ANSWER, sdp_msg);
    auto *context = new RemoteDescriptionContext{this, call_id};
    GstPromise *promise = gst_promise_new_with_change_func(
        &LiveWebRtcSession::on_remote_description_set_cb,
        context,
        nullptr);
    g_signal_emit_by_name(webrtc, "set-remote-description", answer, promise);
    std::fprintf(stderr,
                 "[live] set remote answer requested asynchronously call_id=%s\n",
                 call_id.c_str());
    gst_webrtc_session_description_free(answer);
    gst_object_unref(webrtc);
}

void LiveWebRtcSession::handle_candidate(const nlohmann::json &signal) {
    GstElement *webrtc = nullptr;
    {
        std::lock_guard<std::mutex> lock(mtx_);
        webrtc = webrtc_;
    }
    if (!webrtc) return;

    const auto candidate_value = signal.value("candidate", nlohmann::json(nullptr));
    if (!candidate_value.is_object()) return;
    const std::string candidate = candidate_value.value("candidate", "");
    if (candidate.empty()) return;
    const unsigned int mline_index = candidate_value.value("sdpMLineIndex", 0u);
    std::fprintf(stderr,
                 "[live] ICE remote candidate mline=%u len=%zu %s\n",
                 mline_index,
                 candidate.size(),
                 summarize_ice_candidate(candidate).c_str());
    g_signal_emit_by_name(webrtc, "add-ice-candidate", mline_index, candidate.c_str());
}
