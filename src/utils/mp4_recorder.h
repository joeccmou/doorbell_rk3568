#pragma once

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>

#include "utils/audio_capture_manager.h"

typedef struct _GstElement GstElement;
typedef struct _GstBus GstBus;

class Mp4Recorder {
public:
    Mp4Recorder() = default;
    ~Mp4Recorder();

    void set_audio_dispatcher(AudioFrameDispatcher *dispatcher) { audio_dispatcher_ = dispatcher; }

    bool start(const std::string &output_dir, uint32_t width, uint32_t height, uint32_t fps, uint32_t pixfmt);
    bool start_file(const std::string &output_file, uint32_t width, uint32_t height, uint32_t fps, uint32_t pixfmt);
    void stop();
    bool running() const { return pipeline_ != nullptr; }

    bool write_frame(const uint8_t *data, size_t size, uint64_t frame_ts_ns);
    const std::string &last_file() const { return last_file_; }

private:
    bool process_bus_messages(bool wait_eos, bool *received_eos = nullptr);
    void push_audio_frame(const AudioFrame &frame);
    void unregister_audio_consumer();

    GstElement *pipeline_ = nullptr;
    GstElement *appsrc_ = nullptr;
    GstElement *audio_appsrc_ = nullptr;
    GstBus *bus_ = nullptr;
    std::string last_file_;
    size_t frame_size_ = 0;
    uint64_t frame_index_ = 0;
    uint64_t frame_duration_ns_ = 0;
    uint64_t first_frame_ts_ns_ = 0;
    uint64_t last_pts_ns_ = 0;
    bool has_first_frame_ts_ = false;

    AudioFrameDispatcher *audio_dispatcher_ = nullptr;
    size_t audio_consumer_id_ = 0;
    std::mutex audio_mtx_;
    AudioTimestampRebaser audio_timestamp_rebaser_;
    uint64_t audio_frame_count_ = 0;
};
