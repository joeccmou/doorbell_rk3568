#include "mp4_recorder.h"
#include "perf_logger.h"

#include <algorithm>
#include <chrono>
#include <cstdarg>
#include <cstring>
#include <cstdlib>
#include <linux/videodev2.h>
#include <unistd.h>
#include <filesystem>
#include <system_error>
#include <utility>

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
        const char *env = std::getenv("DOORBELL_REC_TRACE");
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

bool Mp4Recorder::start_segmented(const std::string &temporary_file_pattern,
                                  uint32_t width,
                                  uint32_t height,
                                  uint32_t fps,
                                  uint32_t pixfmt,
                                  SegmentClosedCallback segment_closed_callback) {
    uint64_t start_begin_ns = mono_time_ns();
    rec_trace("start enter pattern=%s w=%u h=%u fps=%u pixfmt=%c%c%c%c",
              temporary_file_pattern.c_str(),
              width,
              height,
              fps,
              pixfmt & 0xFF,
              (pixfmt >> 8) & 0xFF,
              (pixfmt >> 16) & 0xFF,
              (pixfmt >> 24) & 0xFF);
    if (running()) return true;
    if (temporary_file_pattern.empty() || width == 0 || height == 0 || fps == 0) return false;

    const std::string output_dir = std::filesystem::path(temporary_file_pattern).parent_path().string();
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

    last_file_ = temporary_file_pattern;
    segment_closed_callback_ = std::move(segment_closed_callback);

    ensure_gstreamer_initialized();

        const char *gst_format = nullptr;
    size_t frame_size = 0;
    switch (pixfmt) {
        case V4L2_PIX_FMT_NV12:
            gst_format = "NV12";
            frame_size = static_cast<size_t>(width) * static_cast<size_t>(height) * 3 / 2;
            break;
        default:
            g_printerr("[recorder] unsupported media pixfmt: %c%c%c%c\n",
                       pixfmt & 0xFF,
                       (pixfmt >> 8) & 0xFF,
                       (pixfmt >> 16) & 0xFF,
                       (pixfmt >> 24) & 0xFF);
            last_file_.clear();
            segment_closed_callback_ = {};
            return false;
    }

    const char *record_audio_env = std::getenv("DOORBELL_RECORD_AUDIO");
    const bool enable_audio = (record_audio_env == nullptr || record_audio_env[0] != '0') &&
                              audio_dispatcher_ != nullptr;

    gchar *pipeline_desc = nullptr;
    if (enable_audio) {
        pipeline_desc = g_strdup_printf(
            "appsrc name=vsrc is-live=true block=false format=time do-timestamp=true "
            "caps=video/x-raw,format=%s,width=%u,height=%u,framerate=%u/1 "            "! queue leaky=downstream max-size-buffers=2 "
            "! mpph264enc ! h264parse config-interval=-1 ! queue ! mux.video "
            "appsrc name=asrc is-live=true block=false format=time do-timestamp=false "
            "caps=audio/x-raw,format=S16LE,layout=interleaved,rate=48000,channels=1 "
            "! queue leaky=downstream max-size-buffers=16 ! audioconvert ! audioresample "
            "! voaacenc bitrate=128000 ! aacparse ! queue ! mux.audio_0 "
            "splitmuxsink name=mux",
            gst_format, width, height, fps);
    } else {
        pipeline_desc = g_strdup_printf(
            "appsrc name=vsrc is-live=true block=false format=time do-timestamp=true "
            "caps=video/x-raw,format=%s,width=%u,height=%u,framerate=%u/1 "            "! queue leaky=downstream max-size-buffers=2 "
            "! mpph264enc ! h264parse config-interval=-1 ! queue ! mux.video "
            "splitmuxsink name=mux",
            gst_format, width, height, fps);
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
        segment_closed_callback_ = {};
        return false;
    }

    appsrc_ = gst_bin_get_by_name(GST_BIN(pipeline_), "vsrc");
    if (!appsrc_) {
        g_printerr("[recorder] appsrc not found in pipeline\n");
        gst_object_unref(pipeline_);
        pipeline_ = nullptr;
        last_file_.clear();
        segment_closed_callback_ = {};
        return false;
    }

    splitmuxsink_ = gst_bin_get_by_name(GST_BIN(pipeline_), "mux");
    if (!splitmuxsink_) {
        g_printerr("[recorder] splitmuxsink not found in pipeline\n");
        gst_object_unref(appsrc_);
        appsrc_ = nullptr;
        gst_object_unref(pipeline_);
        pipeline_ = nullptr;
        last_file_.clear();
        segment_closed_callback_ = {};
        return false;
    }

    GstStructure *muxer_properties = gst_structure_new(
        "properties",
        "faststart", G_TYPE_BOOLEAN, TRUE,
        nullptr);
    g_object_set(
        G_OBJECT(splitmuxsink_),
        "location", last_file_.c_str(),
        "max-size-time", static_cast<guint64>(60 * GST_SECOND),
        "max-size-bytes", static_cast<guint64>(0),
        "send-keyframe-requests", TRUE,
        "async-finalize", TRUE,
        "muxer-factory", "mp4mux",
        "muxer-properties", muxer_properties,
        nullptr);
    gst_structure_free(muxer_properties);

    if (enable_audio) {
        audio_appsrc_ = gst_bin_get_by_name(GST_BIN(pipeline_), "asrc");
        if (!audio_appsrc_) {
            g_printerr("[recorder] audio appsrc not found in pipeline\n");
            gst_object_unref(appsrc_);
            appsrc_ = nullptr;
            gst_object_unref(splitmuxsink_);
            splitmuxsink_ = nullptr;
            gst_object_unref(pipeline_);
            pipeline_ = nullptr;
            last_file_.clear();
            segment_closed_callback_ = {};
            return false;
        }
        gst_app_src_set_stream_type(GST_APP_SRC(audio_appsrc_), GST_APP_STREAM_TYPE_STREAM);
        g_object_set(G_OBJECT(audio_appsrc_), "block", FALSE, nullptr);
    }
    gst_app_src_set_stream_type(GST_APP_SRC(appsrc_), GST_APP_STREAM_TYPE_STREAM);
    g_object_set(G_OBJECT(appsrc_), "block", FALSE, nullptr);

    GstStateChangeReturn ret = gst_element_set_state(pipeline_, GST_STATE_PLAYING);
    uint64_t play_end_ns = mono_time_ns();
    rec_trace("start set PLAYING ret=%d total_ms=%.3f", ret,
              static_cast<double>(play_end_ns - start_begin_ns) / 1000000.0);
    if (ret == GST_STATE_CHANGE_FAILURE) {
        g_printerr("[recorder] failed to set pipeline PLAYING\n");
        if (audio_appsrc_) {
            gst_object_unref(audio_appsrc_);
            audio_appsrc_ = nullptr;
        }
        gst_object_unref(appsrc_);
        appsrc_ = nullptr;
        gst_object_unref(splitmuxsink_);
        splitmuxsink_ = nullptr;
        gst_object_unref(pipeline_);
        pipeline_ = nullptr;
        last_file_.clear();
        segment_closed_callback_ = {};
        return false;
    }

    bus_ = gst_element_get_bus(pipeline_);

    frame_size_ = frame_size;
    frame_duration_ns_ = GST_SECOND / fps;
    frame_index_ = 0;
    first_frame_ts_ns_ = 0;
    last_pts_ns_ = 0;
    has_first_frame_ts_ = false;
    next_fragment_index_ = 0;
    last_fragment_end_running_time_ns_ = 0;
    fragment_started_running_time_ns_.clear();
    fragment_indexes_.clear();

    if (enable_audio) {
        {
            std::lock_guard<std::mutex> lock(audio_mtx_);
            audio_timestamp_rebaser_.reset();
            audio_frame_count_ = 0;
        }
        audio_consumer_id_ = audio_dispatcher_->register_consumer(
            [this](const AudioFrame &frame) {
                push_audio_frame(frame);
            });
        g_print("[recorder] started with audio\n");
    } else {
        g_print("[recorder] started without audio (set DOORBELL_RECORD_AUDIO=1 to enable)\n");
    }
    return true;
}

void Mp4Recorder::stop() {
    if (!pipeline_) return;

    uint64_t stop_begin_ns = mono_time_ns();
    rec_trace("stop enter frame_index=%llu", static_cast<unsigned long long>(frame_index_));

    unregister_audio_consumer();
    GstElement *audio_appsrc = nullptr;
    {
        std::lock_guard<std::mutex> lock(audio_mtx_);
        audio_appsrc = audio_appsrc_;
        audio_appsrc_ = nullptr;
    }
    if (audio_appsrc) {
        gst_app_src_end_of_stream(GST_APP_SRC(audio_appsrc));
    }

    if (appsrc_) {
        gst_app_src_end_of_stream(GST_APP_SRC(appsrc_));
        perf_logger_log("record stop appsrc_eos_sent=1\n");
    } else {
        perf_logger_log("record stop appsrc_eos_sent=0\n");
    }

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
    if (splitmuxsink_) {
        gst_object_unref(splitmuxsink_);
        splitmuxsink_ = nullptr;
    }
    if (audio_appsrc) {
        gst_object_unref(audio_appsrc);
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
    next_fragment_index_ = 0;
    last_fragment_end_running_time_ns_ = 0;
    fragment_started_running_time_ns_.clear();
    fragment_indexes_.clear();
    segment_closed_callback_ = {};
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

    constexpr GstClockTime kStopWaitEosTimeout = 30 * GST_SECOND;
    const GstMessageType message_types = static_cast<GstMessageType>(
        GST_MESSAGE_ERROR | GST_MESSAGE_EOS | GST_MESSAGE_WARNING | GST_MESSAGE_ELEMENT);

    if (wait_eos) {
        const GstClockTime deadline = gst_util_get_timestamp() + kStopWaitEosTimeout;
        while (!received_eos || !*received_eos) {
            const GstClockTime now = gst_util_get_timestamp();
            if (now >= deadline) break;
            GstMessage *message = gst_bus_timed_pop_filtered(bus_, deadline - now, message_types);
            if (!message) break;
            const bool ok = process_bus_message(message, received_eos);
            gst_message_unref(message);
            if (!ok) return false;
        }
        if (!received_eos || !*received_eos) {
            perf_logger_log("record stop eos_wait timeout_ms=%u eos_received=0\n",
                            static_cast<unsigned>(kStopWaitEosTimeout / GST_MSECOND));
        }
    }

    while (true) {
        GstMessage *message = gst_bus_pop_filtered(bus_, message_types);
        if (!message) break;
        const bool ok = process_bus_message(message, received_eos);
        gst_message_unref(message);
        if (!ok) return false;
    }

    if (wait_eos) {
        perf_logger_log("record stop eos_wait timeout_ms=%u eos_received=%d\n",
                        static_cast<unsigned>(kStopWaitEosTimeout / GST_MSECOND),
                        received_eos && *received_eos ? 1 : 0);
    }

    return true;
}

bool Mp4Recorder::process_bus_message(GstMessage *message, bool *received_eos) {
    if (!message) return true;

    if (GST_MESSAGE_TYPE(message) == GST_MESSAGE_ERROR) {
        GError *error = nullptr;
        gchar *debug = nullptr;
        gst_message_parse_error(message, &error, &debug);
        g_printerr("[recorder] pipeline error: %s (%s)\n",
                   error ? error->message : "unknown",
                   debug ? debug : "no debug");
        if (error) g_error_free(error);
        if (debug) g_free(debug);
        return false;
    }

    if (GST_MESSAGE_TYPE(message) == GST_MESSAGE_WARNING) {
        GError *warning = nullptr;
        gchar *debug = nullptr;
        gst_message_parse_warning(message, &warning, &debug);
        g_printerr("[recorder] pipeline warning: %s (%s)\n",
                   warning ? warning->message : "unknown",
                   debug ? debug : "no debug");
        if (warning) g_error_free(warning);
        if (debug) g_free(debug);
        return true;
    }

    if (GST_MESSAGE_TYPE(message) == GST_MESSAGE_EOS) {
        if (received_eos) *received_eos = true;
        return true;
    }

    if (GST_MESSAGE_TYPE(message) != GST_MESSAGE_ELEMENT) return true;
    const GstStructure *structure = gst_message_get_structure(message);
    if (!structure) return true;

    const bool fragment_opened =
        gst_structure_has_name(structure, "splitmuxsink-fragment-opened");
    const bool fragment_closed =
        gst_structure_has_name(structure, "splitmuxsink-fragment-closed");
    if (!fragment_opened && !fragment_closed) return true;

    const gchar *location_value = gst_structure_get_string(structure, "location");
    if (!location_value || location_value[0] == '\0') return true;
    const std::string location(location_value);
    guint64 running_time_ns = 0;
    if (!gst_structure_get_clock_time(structure, "running-time", &running_time_ns)) {
        running_time_ns = fragment_closed
            ? last_fragment_end_running_time_ns_
            : 0;
    }

    if (fragment_opened) {
        fragment_started_running_time_ns_[location] = running_time_ns;
        fragment_indexes_[location] = next_fragment_index_++;
        rec_trace("fragment opened index=%u running_time_ns=%llu file=%s",
                  fragment_indexes_[location],
                  static_cast<unsigned long long>(running_time_ns),
                  location.c_str());
        return true;
    }

    const auto started = fragment_started_running_time_ns_.find(location);
    const auto index = fragment_indexes_.find(location);
    ClosedSegment segment;
    segment.fragment_index = index != fragment_indexes_.end()
        ? index->second
        : next_fragment_index_++;
    segment.temporary_file = location;
    const uint64_t reported_started_running_time_ns = started != fragment_started_running_time_ns_.end()
        ? started->second
        : last_fragment_end_running_time_ns_;
    segment.started_running_time_ns = segment.fragment_index == 0
        ? reported_started_running_time_ns
        : last_fragment_end_running_time_ns_;
    segment.ended_running_time_ns = std::max<uint64_t>(
        segment.started_running_time_ns,
        running_time_ns);
    last_fragment_end_running_time_ns_ = segment.ended_running_time_ns;
    fragment_started_running_time_ns_.erase(location);
    fragment_indexes_.erase(location);

    rec_trace("fragment closed index=%u start_ns=%llu end_ns=%llu file=%s",
              segment.fragment_index,
              static_cast<unsigned long long>(segment.started_running_time_ns),
              static_cast<unsigned long long>(segment.ended_running_time_ns),
              location.c_str());
    if (segment_closed_callback_) {
        try {
            segment_closed_callback_(segment);
        } catch (const std::exception &exception) {
            g_printerr("[recorder] segment callback failed: %s\n", exception.what());
            return false;
        } catch (...) {
            g_printerr("[recorder] segment callback failed: unknown error\n");
            return false;
        }
    }
    return true;
}

void Mp4Recorder::unregister_audio_consumer() {
    if (audio_dispatcher_ && audio_consumer_id_ != 0) {
        audio_dispatcher_->unregister_consumer(audio_consumer_id_);
        audio_consumer_id_ = 0;
    }
}

void Mp4Recorder::push_audio_frame(const AudioFrame &frame) {
    std::lock_guard<std::mutex> lock(audio_mtx_);
    if (!audio_appsrc_ || !frame.data || frame.size == 0) return;

    GstBuffer *buffer = gst_buffer_new_allocate(nullptr, frame.size, nullptr);
    if (!buffer) return;

    GstMapInfo map;
    if (!gst_buffer_map(buffer, &map, GST_MAP_WRITE)) {
        gst_buffer_unref(buffer);
        return;
    }
    std::memcpy(map.data, frame.data, frame.size);
    gst_buffer_unmap(buffer, &map);

    const uint64_t rebased_pts_ns = audio_timestamp_rebaser_.rebase(frame.pts_ns, frame.duration_ns);
    GST_BUFFER_PTS(buffer) = static_cast<GstClockTime>(rebased_pts_ns);
    GST_BUFFER_DTS(buffer) = static_cast<GstClockTime>(rebased_pts_ns);
    GST_BUFFER_DURATION(buffer) = static_cast<GstClockTime>(frame.duration_ns);

    ++audio_frame_count_;
    if (audio_frame_count_ == 1 || audio_frame_count_ % 500 == 0) {
        g_print("[recorder] audio input count=%llu source_pts_ns=%llu rebased_pts_ns=%llu\n",
                static_cast<unsigned long long>(audio_frame_count_),
                static_cast<unsigned long long>(frame.pts_ns),
                static_cast<unsigned long long>(rebased_pts_ns));
    }

    GstFlowReturn flow_ret = gst_app_src_push_buffer(GST_APP_SRC(audio_appsrc_), buffer);
    if (flow_ret != GST_FLOW_OK && flow_ret != GST_FLOW_FLUSHING) {
        g_printerr("[recorder] audio appsrc push failed: %d\n", flow_ret);
    }
}
