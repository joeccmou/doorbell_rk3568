#include "utils/doorbell_chime_player.h"

#include "utils/audio_pipeline_config.h"

#include <gst/app/gstappsink.h>

#include <cstdio>
#include <cstdint>
#include <filesystem>
#include <utility>

namespace {

constexpr unsigned kChimeRepeatCount = 2;

}

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
    // 新按铃只保留最新请求，播放线程会立即中断旧铃声并从头播放两次。
    {
        std::lock_guard<std::mutex> lock(mutex_);
        requested_play_sequence_.fetch_add(1);
    }
    cv_.notify_one();
}

void DoorbellChimePlayer::worker_loop() {
    uint64_t handled_play_sequence = 0;
    while (!stop_requested_.load()) {
        uint64_t play_sequence = 0;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            cv_.wait(lock, [this, &handled_play_sequence] {
                return stop_requested_.load() ||
                       requested_play_sequence_.load() != handled_play_sequence;
            });
            if (stop_requested_.load()) break;
            play_sequence = requested_play_sequence_.load();
            handled_play_sequence = play_sequence;
        }

        for (unsigned repeat_index = 1;
             repeat_index <= kChimeRepeatCount;
             ++repeat_index) {
            const PlaybackResult result =
                play_file_once(play_sequence, repeat_index);
            if (result == PlaybackResult::kFailed) {
                std::fprintf(stderr,
                             "[ring] play device chime failed path=%s "
                             "play_sequence=%llu repeat=%u/%u\n",
                             wav_path_.c_str(),
                             static_cast<unsigned long long>(play_sequence),
                             repeat_index,
                             kChimeRepeatCount);
                break;
            }
            if (result == PlaybackResult::kInterrupted) {
                std::fprintf(stdout,
                             "[ring] device chime interrupted "
                             "play_sequence=%llu repeat=%u/%u\n",
                             static_cast<unsigned long long>(play_sequence),
                             repeat_index,
                             kChimeRepeatCount);
                break;
            }
        }
    }
}

DoorbellChimePlayer::PlaybackResult DoorbellChimePlayer::play_file_once(
    uint64_t play_sequence,
    unsigned repeat_index) {
    const auto interrupted = [this, play_sequence] {
        return stop_requested_.load() ||
               requested_play_sequence_.load() != play_sequence;
    };
    if (interrupted()) {
        return PlaybackResult::kInterrupted;
    }
    if (!audio_ || !audio_->running() || !std::filesystem::is_regular_file(wav_path_)) {
        return PlaybackResult::kFailed;
    }

    gchar *description = g_strdup_printf(
        "filesrc location=\"%s\" ! wavparse ! audioconvert ! audioresample ! "
        "%s ! appsink name=chime_sink sync=true max-buffers=8 drop=false",
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
        return PlaybackResult::kFailed;
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
        return PlaybackResult::kFailed;
    }

    bool ok = true;
    bool finished = false;
    uint64_t sample_count = 0;
    uint64_t duration_ns = 0;
    while (!interrupted() && !finished) {
        GstSample *sample =
            gst_app_sink_try_pull_sample(GST_APP_SINK(sink), 50 * GST_MSECOND);
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
                sample_count += map.size / sizeof(int16_t);
                duration_ns += frame.duration_ns;
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

    if (interrupted()) {
        return PlaybackResult::kInterrupted;
    }
    if (!ok || !finished) {
        return PlaybackResult::kFailed;
    }

    std::fprintf(stdout,
                 "[ring] device chime completed play_sequence=%llu "
                 "repeat=%u/%u samples=%llu duration_ms=%llu\n",
                 static_cast<unsigned long long>(play_sequence),
                 repeat_index,
                 kChimeRepeatCount,
                 static_cast<unsigned long long>(sample_count),
                 static_cast<unsigned long long>(
                     duration_ns / (1000ULL * 1000ULL)));
    return PlaybackResult::kCompleted;
}
