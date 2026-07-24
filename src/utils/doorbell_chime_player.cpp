#include "utils/doorbell_chime_player.h"

#include "utils/audio_pipeline_config.h"

#include <gst/app/gstappsink.h>

#include <cstdio>
#include <filesystem>
#include <utility>

DoorbellChimePlayer::DoorbellChimePlayer(
    AudioCaptureManager *audio,
    std::string wav_path)
    : audio_(audio), wav_path_(std::move(wav_path)) {}

DoorbellChimePlayer::~DoorbellChimePlayer() {
    stop();
}

void DoorbellChimePlayer::start() {
    if (worker_.joinable()) return;
    stop_requested_.store(false);
    worker_ = std::thread(&DoorbellChimePlayer::worker_loop, this);
}

void DoorbellChimePlayer::stop() {
    stop_requested_.store(true);
    cv_.notify_all();
    if (worker_.joinable()) worker_.join();
}

void DoorbellChimePlayer::play_once() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        ++pending_plays_;
    }
    cv_.notify_one();
}

void DoorbellChimePlayer::worker_loop() {
    while (!stop_requested_.load()) {
        {
            std::unique_lock<std::mutex> lock(mutex_);
            cv_.wait(lock, [this] {
                return stop_requested_.load() || pending_plays_ > 0;
            });
            if (stop_requested_.load()) break;
            --pending_plays_;
        }
        if (!play_file_once()) {
            std::fprintf(stderr,
                         "[ring] play device chime failed path=%s\n",
                         wav_path_.c_str());
        }
    }
}

bool DoorbellChimePlayer::play_file_once() {
    if (!audio_ || !audio_->running() || !std::filesystem::is_regular_file(wav_path_)) {
        return false;
    }

    gchar *description = g_strdup_printf(
        "filesrc location=\"%s\" ! wavparse ! audioconvert ! audioresample ! "
        "%s ! appsink name=chime_sink sync=false max-buffers=8 drop=false",
        wav_path_.c_str(),
        audio_webrtc_raw_caps());
    GError *error = nullptr;
    GstElement *pipeline = gst_parse_launch(description, &error);
    g_free(description);
    if (!pipeline) {
        if (error) {
            std::fprintf(stderr, "[ring] create chime pipeline failed error=%s\n", error->message);
            g_error_free(error);
        }
        return false;
    }
    if (error) {
        std::fprintf(stderr, "[ring] chime pipeline warning=%s\n", error->message);
        g_error_free(error);
    }

    GstElement *sink = gst_bin_get_by_name(GST_BIN(pipeline), "chime_sink");
    GstBus *bus = gst_element_get_bus(pipeline);
    if (!sink || !bus ||
        gst_element_set_state(pipeline, GST_STATE_PLAYING) == GST_STATE_CHANGE_FAILURE) {
        if (sink) gst_object_unref(sink);
        if (bus) gst_object_unref(bus);
        gst_element_set_state(pipeline, GST_STATE_NULL);
        gst_object_unref(pipeline);
        return false;
    }

    bool ok = true;
    bool finished = false;
    while (!stop_requested_.load() && !finished) {
        GstSample *sample =
            gst_app_sink_try_pull_sample(GST_APP_SINK(sink), 100 * GST_MSECOND);
        if (sample) {
            GstBuffer *buffer = gst_sample_get_buffer(sample);
            GstMapInfo map{};
            if (buffer && gst_buffer_map(buffer, &map, GST_MAP_READ)) {
                AudioFrame frame;
                frame.data = map.data;
                frame.size = map.size;
                frame.duration_ns = GST_CLOCK_TIME_IS_VALID(
                                        GST_BUFFER_DURATION(buffer))
                    ? GST_BUFFER_DURATION(buffer)
                    : 20ULL * 1000ULL * 1000ULL;
                ok = audio_->push_playback_frame(frame) && ok;
                gst_buffer_unmap(buffer, &map);
            }
            gst_sample_unref(sample);
        }

        GstMessage *message = gst_bus_pop_filtered(
            bus,
            static_cast<GstMessageType>(GST_MESSAGE_EOS | GST_MESSAGE_ERROR));
        if (!message) continue;
        if (GST_MESSAGE_TYPE(message) == GST_MESSAGE_ERROR) {
            GError *message_error = nullptr;
            gchar *debug = nullptr;
            gst_message_parse_error(message, &message_error, &debug);
            std::fprintf(stderr,
                         "[ring] chime decode failed error=%s debug=%s\n",
                         message_error ? message_error->message : "unknown",
                         debug ? debug : "");
            if (message_error) g_error_free(message_error);
            g_free(debug);
            ok = false;
        }
        finished = true;
        gst_message_unref(message);
    }

    gst_element_set_state(pipeline, GST_STATE_NULL);
    gst_object_unref(bus);
    gst_object_unref(sink);
    gst_object_unref(pipeline);
    return ok && finished;
}
