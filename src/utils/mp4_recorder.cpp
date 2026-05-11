#include "mp4_recorder.h"
#include "perf_logger.h"

#include <chrono>
#include <cstdarg>
#include <cstring>
#include <ctime>
#include <cstdlib>
#include <linux/videodev2.h>
#include <unistd.h>
#include <filesystem>
#include <sstream>
#include <system_error>

#include <gst/gst.h>
#include <gst/app/gstappsrc.h>

namespace {
void ensure_gstreamer_initialized() {
    if (!gst_is_initialized()) {
        gst_init(nullptr, nullptr);
    }
}

bool recorder_trace_enabled() {
    static int enabled = [] {
        const char *env = std::getenv("LVGL_CAMERA_REC_TRACE");
        return (env && env[0] == '1') ? 1 : 0;
    }();
    return enabled != 0;
}

uint64_t mono_time_ns() {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}

void rec_trace(const char *fmt, ...) {
    if (!recorder_trace_enabled()) return;

    va_list args;
    va_start(args, fmt);
    va_list args_copy;
    va_copy(args_copy, args);

    std::fprintf(stdout, "[rec-trace] ");
    std::vfprintf(stdout, fmt, args);
    std::fprintf(stdout, "\n");
    std::fflush(stdout);

    char line[1024];
    std::vsnprintf(line, sizeof(line), fmt, args_copy);
    perf_logger_log("rec_trace %s\n", line);

    va_end(args_copy);
    va_end(args);
}
}

Mp4Recorder::~Mp4Recorder() {
    stop();
}

bool Mp4Recorder::start(const std::string &output_dir, uint32_t width, uint32_t height, uint32_t fps, uint32_t pixfmt) {
    uint64_t start_begin_ns = mono_time_ns();
    rec_trace("start enter dir=%s w=%u h=%u fps=%u pixfmt=%c%c%c%c",
              output_dir.c_str(),
              width,
              height,
              fps,
              pixfmt & 0xFF,
              (pixfmt >> 8) & 0xFF,
              (pixfmt >> 16) & 0xFF,
              (pixfmt >> 24) & 0xFF);
    if (running()) return true;
    if (output_dir.empty() || width == 0 || height == 0 || fps == 0) return false;

    std::error_code ec;
    std::filesystem::create_directories(output_dir, ec);
    if (ec) {
        g_printerr("[recorder] failed to create output dir %s: %s\n", output_dir.c_str(), ec.message().c_str());
        return false;
    }
    if (::access(output_dir.c_str(), W_OK) != 0) {
        g_printerr("[recorder] output dir is not writable: %s\n", output_dir.c_str());
        return false;
    }

    std::time_t now = std::time(nullptr);
    std::tm tm_now{};
    localtime_r(&now, &tm_now);
    char ts[32] = {0};
    std::strftime(ts, sizeof(ts), "%Y%m%d_%H%M%S", &tm_now);

    std::ostringstream path;
    path << output_dir << "/person_" << ts << ".mp4";
    last_file_ = path.str();

    ensure_gstreamer_initialized();

    const char *gst_format = nullptr;
    size_t frame_size = 0;
    switch (pixfmt) {
        case V4L2_PIX_FMT_UYVY:
            gst_format = "UYVY";
            frame_size = static_cast<size_t>(width) * static_cast<size_t>(height) * 2;
            break;
        case V4L2_PIX_FMT_NV12:
            gst_format = "NV12";
            frame_size = static_cast<size_t>(width) * static_cast<size_t>(height) * 3 / 2;
            break;
        case V4L2_PIX_FMT_NV21:
            gst_format = "NV21";
            frame_size = static_cast<size_t>(width) * static_cast<size_t>(height) * 3 / 2;
            break;
        case V4L2_PIX_FMT_YUYV:
            gst_format = "YUY2";
            frame_size = static_cast<size_t>(width) * static_cast<size_t>(height) * 2;
            break;
        default:
            g_printerr("[recorder] unsupported pixfmt: %c%c%c%c\n",
                       pixfmt & 0xFF,
                       (pixfmt >> 8) & 0xFF,
                       (pixfmt >> 16) & 0xFF,
                       (pixfmt >> 24) & 0xFF);
            return false;
    }

    const bool enable_audio = []() {
        const char *env = std::getenv("LVGL_CAMERA_RECORD_AUDIO");
        return env==nullptr || env[0] != '0';
    }();

    gchar *pipeline_desc = nullptr;
    if (enable_audio) {
        pipeline_desc = g_strdup_printf(
            "appsrc name=vsrc is-live=true block=false format=time do-timestamp=true "
            "caps=video/x-raw,format=%s,width=%u,height=%u,framerate=%u/1 "
            "! videoconvert "
            "! queue leaky=downstream max-size-buffers=2 "
            "! mpph264enc ! h264parse ! queue ! mux. "
            "alsasrc device=hw:0,0 do-timestamp=true "
            "! queue ! audioconvert ! audioresample ! voaacenc bitrate=128000 ! aacparse ! queue ! mux. "
            "mp4mux name=mux faststart=true ! filesink location=%s sync=false async=false",
            gst_format, width, height, fps, last_file_.c_str());
    } else {
        pipeline_desc = g_strdup_printf(
            "appsrc name=vsrc is-live=true block=false format=time do-timestamp=true "
            "caps=video/x-raw,format=%s,width=%u,height=%u,framerate=%u/1 "
            "! videoconvert "
            "! queue leaky=downstream max-size-buffers=2 "
            "! mpph264enc ! h264parse ! mp4mux faststart=true "
            "! filesink location=%s sync=false async=false",
            gst_format, width, height, fps, last_file_.c_str());
    }

    GError *error = nullptr;
    uint64_t parse_begin_ns = mono_time_ns();
    pipeline_ = gst_parse_launch(pipeline_desc, &error);
    uint64_t parse_end_ns = mono_time_ns();
    rec_trace("start gst_parse_launch done in %.3f ms", static_cast<double>(parse_end_ns - parse_begin_ns) / 1000000.0);
    g_free(pipeline_desc);
    if (!pipeline_) {
        if (error) {
            g_printerr("[recorder] gstreamer pipeline create failed: %s\n", error->message);
            g_error_free(error);
        }
        last_file_.clear();
        return false;
    }

    appsrc_ = gst_bin_get_by_name(GST_BIN(pipeline_), "vsrc");
    if (!appsrc_) {
        g_printerr("[recorder] appsrc not found in pipeline\n");
        gst_object_unref(pipeline_);
        pipeline_ = nullptr;
        last_file_.clear();
        return false;
    }

    gst_app_src_set_stream_type(GST_APP_SRC(appsrc_), GST_APP_STREAM_TYPE_STREAM);
    g_object_set(G_OBJECT(appsrc_), "block", FALSE, nullptr);

    GstStateChangeReturn ret = gst_element_set_state(pipeline_, GST_STATE_PLAYING);
    uint64_t play_end_ns = mono_time_ns();
    rec_trace("start set PLAYING ret=%d total_ms=%.3f", ret,
              static_cast<double>(play_end_ns - start_begin_ns) / 1000000.0);
    if (ret == GST_STATE_CHANGE_FAILURE) {
        g_printerr("[recorder] failed to set pipeline PLAYING\n");
        gst_object_unref(appsrc_);
        appsrc_ = nullptr;
        gst_object_unref(pipeline_);
        pipeline_ = nullptr;
        last_file_.clear();
        return false;
    }

    bus_ = gst_element_get_bus(pipeline_);

    frame_size_ = frame_size;
    frame_duration_ns_ = GST_SECOND / fps;
    frame_index_ = 0;
    first_frame_ts_ns_ = 0;
    last_pts_ns_ = 0;
    has_first_frame_ts_ = false;

    if (enable_audio) {
        g_print("[recorder] started with audio\n");
    } else {
        g_print("[recorder] started without audio (set LVGL_CAMERA_RECORD_AUDIO=1 to enable)\n");
    }
    return true;
}

void Mp4Recorder::stop() {
    if (!pipeline_) return;

    uint64_t stop_begin_ns = mono_time_ns();
    rec_trace("stop enter frame_index=%llu", static_cast<unsigned long long>(frame_index_));

    if (appsrc_) {
        gst_app_src_end_of_stream(GST_APP_SRC(appsrc_));
        perf_logger_log("record stop appsrc_eos_sent=1\n");
    } else {
        perf_logger_log("record stop appsrc_eos_sent=0\n");
    }

    bool pipeline_eos_sent = false;
    if (pipeline_) {
        pipeline_eos_sent = gst_element_send_event(pipeline_, gst_event_new_eos());
    }
    perf_logger_log("record stop pipeline_eos_sent=%d\n", pipeline_eos_sent ? 1 : 0);

    bool eos_received = false;
    uint64_t eos_wait_begin_ns = mono_time_ns();
    bool bus_ok = process_bus_messages(true, &eos_received);
    uint64_t eos_wait_end_ns = mono_time_ns();
    double eos_wait_ms = static_cast<double>(eos_wait_end_ns - eos_wait_begin_ns) / 1000000.0;
    perf_logger_log("record stop eos_wait_ms=%.3f\n", eos_wait_ms);
    perf_logger_log("record stop eos_received=%d bus_ok=%d\n", eos_received ? 1 : 0, bus_ok ? 1 : 0);
    rec_trace("stop after process_bus_messages(wait_eos=true)");

    gst_element_set_state(pipeline_, GST_STATE_NULL);
    if (appsrc_) {
        gst_object_unref(appsrc_);
        appsrc_ = nullptr;
    }
    if (bus_) {
        gst_object_unref(bus_);
        bus_ = nullptr;
    }
    gst_object_unref(pipeline_);
    pipeline_ = nullptr;

    uint64_t stop_end_ns = mono_time_ns();
    rec_trace("stop done total_ms=%.3f", static_cast<double>(stop_end_ns - stop_begin_ns) / 1000000.0);

    frame_size_ = 0;
    frame_duration_ns_ = 0;
    frame_index_ = 0;
    first_frame_ts_ns_ = 0;
    last_pts_ns_ = 0;
    has_first_frame_ts_ = false;
}

bool Mp4Recorder::write_frame(const uint8_t *data, size_t size, uint64_t frame_ts_ns) {
    if (!pipeline_ || !appsrc_ || !data || size < frame_size_) return false;
    const uint64_t frame_idx = frame_index_;
    uint64_t wf_begin_ns = mono_time_ns();
    rec_trace("write enter idx=%llu size=%zu", static_cast<unsigned long long>(frame_idx), size);

    uint64_t bus_begin_ns = mono_time_ns();
    if (!process_bus_messages(false, nullptr)) {
        uint64_t bus_end_ns = mono_time_ns();
        rec_trace("write bus check failed idx=%llu bus_ms=%.3f",
                  static_cast<unsigned long long>(frame_idx),
                  static_cast<double>(bus_end_ns - bus_begin_ns) / 1000000.0);
        stop();
        return false;
    }
    uint64_t bus_end_ns = mono_time_ns();
    rec_trace("write bus check ok idx=%llu bus_ms=%.3f",
              static_cast<unsigned long long>(frame_idx),
              static_cast<double>(bus_end_ns - bus_begin_ns) / 1000000.0);

    uint64_t alloc_begin_ns = mono_time_ns();
    GstBuffer *buffer = gst_buffer_new_allocate(nullptr, frame_size_, nullptr);
    uint64_t alloc_end_ns = mono_time_ns();
    rec_trace("write buffer alloc idx=%llu alloc_ms=%.3f",
              static_cast<unsigned long long>(frame_idx),
              static_cast<double>(alloc_end_ns - alloc_begin_ns) / 1000000.0);
    if (!buffer) return false;

    GstMapInfo map;
    if (!gst_buffer_map(buffer, &map, GST_MAP_WRITE)) {
        gst_buffer_unref(buffer);
        return false;
    }
    std::memcpy(map.data, data, frame_size_);
    gst_buffer_unmap(buffer, &map);
    uint64_t copy_end_ns = mono_time_ns();
    rec_trace("write map+copy idx=%llu copy_ms=%.3f",
              static_cast<unsigned long long>(frame_idx),
              static_cast<double>(copy_end_ns - alloc_end_ns) / 1000000.0);

    uint64_t pts_ns = 0;
    if (frame_ts_ns != 0) {
        if (!has_first_frame_ts_) {
            first_frame_ts_ns_ = frame_ts_ns;
            has_first_frame_ts_ = true;
            pts_ns = 0;
        } else if (frame_ts_ns >= first_frame_ts_ns_) {
            pts_ns = frame_ts_ns - first_frame_ts_ns_;
        } else {
            pts_ns = last_pts_ns_ + frame_duration_ns_;
        }
        if (pts_ns < last_pts_ns_) {
            pts_ns = last_pts_ns_ + 1;
        }
    } else {
        pts_ns = frame_index_ * frame_duration_ns_;
    }

    uint64_t duration_ns = frame_duration_ns_;
    if (frame_index_ > 0 && pts_ns > last_pts_ns_) {
        duration_ns = pts_ns - last_pts_ns_;
    }

    GST_BUFFER_PTS(buffer) = static_cast<GstClockTime>(pts_ns);
    GST_BUFFER_DTS(buffer) = static_cast<GstClockTime>(pts_ns);
    GST_BUFFER_DURATION(buffer) = static_cast<GstClockTime>(duration_ns);
    last_pts_ns_ = pts_ns;
    frame_index_++;

    rec_trace("write before push idx=%llu", static_cast<unsigned long long>(frame_idx));
    uint64_t push_begin_ns = mono_time_ns();
    GstFlowReturn flow_ret = gst_app_src_push_buffer(GST_APP_SRC(appsrc_), buffer);
    uint64_t push_end_ns = mono_time_ns();
    rec_trace("write after push idx=%llu flow_ret=%d push_ms=%.3f total_ms=%.3f",
              static_cast<unsigned long long>(frame_idx),
              flow_ret,
              static_cast<double>(push_end_ns - push_begin_ns) / 1000000.0,
              static_cast<double>(push_end_ns - wf_begin_ns) / 1000000.0);
    return flow_ret == GST_FLOW_OK;
}

bool Mp4Recorder::process_bus_messages(bool wait_eos, bool *received_eos) {
    if (!bus_) return true;

    if (received_eos) {
        *received_eos = false;
    }

    bool eos_seen = false;
    constexpr GstClockTime kStopWaitEosTimeout = 2000 * GST_MSECOND;

    if (wait_eos) {
        GstMessage *msg = gst_bus_timed_pop_filtered(
            bus_,
            kStopWaitEosTimeout,
            static_cast<GstMessageType>(GST_MESSAGE_EOS | GST_MESSAGE_ERROR));
        if (msg) {
            if (GST_MESSAGE_TYPE(msg) == GST_MESSAGE_EOS) {
                eos_seen = true;
            } else if (GST_MESSAGE_TYPE(msg) == GST_MESSAGE_ERROR) {
                GError *err = nullptr;
                gchar *debug = nullptr;
                gst_message_parse_error(msg, &err, &debug);
                g_printerr("[recorder] pipeline error on stop: %s (%s)\n",
                           err ? err->message : "unknown",
                           debug ? debug : "no debug");
                if (err) g_error_free(err);
                if (debug) g_free(debug);
            }
            gst_message_unref(msg);
        } else {
            perf_logger_log("record stop eos_wait timeout_ms=%u eos_received=0\n",
                            static_cast<unsigned>(kStopWaitEosTimeout / GST_MSECOND));
        }
    }

    while (true) {
        GstMessage *msg = gst_bus_pop_filtered(
            bus_,
            static_cast<GstMessageType>(GST_MESSAGE_ERROR | GST_MESSAGE_EOS | GST_MESSAGE_WARNING));
        if (!msg) break;

        if (GST_MESSAGE_TYPE(msg) == GST_MESSAGE_ERROR) {
            GError *err = nullptr;
            gchar *debug = nullptr;
            gst_message_parse_error(msg, &err, &debug);
            g_printerr("[recorder] pipeline error: %s (%s)\n",
                       err ? err->message : "unknown",
                       debug ? debug : "no debug");
            if (err) g_error_free(err);
            if (debug) g_free(debug);
            gst_message_unref(msg);
            return false;
        }

        if (GST_MESSAGE_TYPE(msg) == GST_MESSAGE_EOS) {
            eos_seen = true;
        }

        if (GST_MESSAGE_TYPE(msg) == GST_MESSAGE_WARNING) {
            GError *warn = nullptr;
            gchar *debug = nullptr;
            gst_message_parse_warning(msg, &warn, &debug);
            g_printerr("[recorder] pipeline warning: %s (%s)\n",
                       warn ? warn->message : "unknown",
                       debug ? debug : "no debug");
            if (warn) g_error_free(warn);
            if (debug) g_free(debug);
        }

        gst_message_unref(msg);
    }

    if (received_eos) {
        *received_eos = eos_seen;
    }

    if (wait_eos) {
        perf_logger_log("record stop eos_wait timeout_ms=%u eos_received=%d\n",
                        static_cast<unsigned>(kStopWaitEosTimeout / GST_MSECOND),
                        eos_seen ? 1 : 0);
    }

    return true;
}
