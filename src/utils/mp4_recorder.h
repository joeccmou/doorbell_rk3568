#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <unordered_map>

#include "utils/audio_capture_manager.h"

typedef struct _GstElement GstElement;
typedef struct _GstBus GstBus;
typedef struct _GstMessage GstMessage;

class Mp4Recorder {
public:
    struct ClosedSegment {
        uint32_t fragment_index = 0;
        std::string temporary_file;
        uint64_t started_running_time_ns = 0;
        uint64_t ended_running_time_ns = 0;
    };

    using SegmentClosedCallback = std::function<void(const ClosedSegment &segment)>;

    Mp4Recorder() = default;
    ~Mp4Recorder();

    void set_audio_dispatcher(AudioFrameDispatcher *dispatcher) { audio_dispatcher_ = dispatcher; }

    bool start_segmented(const std::string &temporary_file_pattern,
                         uint32_t width,
                         uint32_t height,
                         uint32_t fps,
                         uint32_t pixfmt,
                         SegmentClosedCallback segment_closed_callback);
    void stop();
    bool running() const { return pipeline_ != nullptr; }

    bool write_frame(const uint8_t *data, size_t size, uint64_t frame_ts_ns);
    const std::string &last_file() const { return last_file_; }

private:
    bool process_bus_messages(bool wait_eos, bool *received_eos = nullptr);
    bool process_bus_message(GstMessage *message, bool *received_eos);
    void push_audio_frame(const AudioFrame &frame);
    void unregister_audio_consumer();

    GstElement *pipeline_ = nullptr;
    GstElement *appsrc_ = nullptr;
    GstElement *audio_appsrc_ = nullptr;
    GstElement *splitmuxsink_ = nullptr;
    GstBus *bus_ = nullptr;
    std::string last_file_;
    size_t frame_size_ = 0;
    uint64_t frame_index_ = 0;
    uint64_t frame_duration_ns_ = 0;
    uint64_t first_frame_ts_ns_ = 0;
    uint64_t last_pts_ns_ = 0;
    bool has_first_frame_ts_ = false;
    uint32_t next_fragment_index_ = 0;
    uint64_t last_fragment_end_running_time_ns_ = 0;
    std::unordered_map<std::string, uint64_t> fragment_started_running_time_ns_;
    std::unordered_map<std::string, uint32_t> fragment_indexes_;
    SegmentClosedCallback segment_closed_callback_;

    AudioFrameDispatcher *audio_dispatcher_ = nullptr;
    size_t audio_consumer_id_ = 0;
    std::mutex audio_mtx_;
    AudioTimestampRebaser audio_timestamp_rebaser_;
    uint64_t audio_frame_count_ = 0;
};
