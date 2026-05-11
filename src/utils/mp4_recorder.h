#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

typedef struct _GstElement GstElement;
typedef struct _GstBus GstBus;

class Mp4Recorder {
public:
    Mp4Recorder() = default;
    ~Mp4Recorder();

    bool start(const std::string &output_dir, uint32_t width, uint32_t height, uint32_t fps, uint32_t pixfmt);
    void stop();
    bool running() const { return pipeline_ != nullptr; }

    bool write_frame(const uint8_t *data, size_t size, uint64_t frame_ts_ns);
    const std::string &last_file() const { return last_file_; }

private:
    bool process_bus_messages(bool wait_eos, bool *received_eos = nullptr);

    GstElement *pipeline_ = nullptr;
    GstElement *appsrc_ = nullptr;
    GstBus *bus_ = nullptr;
    std::string last_file_;
    size_t frame_size_ = 0;
    uint64_t frame_index_ = 0;
    uint64_t frame_duration_ns_ = 0;
    uint64_t first_frame_ts_ns_ = 0;
    uint64_t last_pts_ns_ = 0;
    bool has_first_frame_ts_ = false;
};
