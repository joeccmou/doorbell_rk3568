#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>

class RgaDisplayPipeline final {
public:
    enum class RenderResult {
        Rendered,
        Backoff,
        Failed,
    };

    RgaDisplayPipeline(uint32_t source_width,
                       uint32_t source_height,
                       uint32_t display_width,
                       uint32_t display_height);
    ~RgaDisplayPipeline();

    RgaDisplayPipeline(const RgaDisplayPipeline &) = delete;
    RgaDisplayPipeline &operator=(const RgaDisplayPipeline &) = delete;

    bool initialize();
    RenderResult render(const uint8_t *rgb, size_t size);

    const uint8_t *output_data() const;
    size_t output_size() const;
    bool ready() const;

private:
    bool attempt_recovery(std::chrono::steady_clock::time_point now);
    bool ensure_buffers();
    bool import_handles();
    int run_self_test();
    int run_conversion();
    void release_handles();
    void release_buffers();
    void record_failure(const char *stage, int status,
                        std::chrono::steady_clock::time_point now);
    void record_recovery();
    std::chrono::milliseconds retry_delay() const;

    uint32_t source_width_ = 0;
    uint32_t source_height_ = 0;
    uint32_t display_width_ = 0;
    uint32_t display_height_ = 0;
    size_t source_size_ = 0;
    size_t output_size_ = 0;

    int heap_fd_ = -1;
    int source_fd_ = -1;
    int output_fd_ = -1;
    void *source_map_ = nullptr;
    void *output_map_ = nullptr;
    uint64_t source_handle_ = 0;
    uint64_t output_handle_ = 0;

    bool ready_ = false;
    bool has_failed_ = false;
    uint32_t consecutive_failures_ = 0;
    std::chrono::steady_clock::time_point next_retry_at_{};
};
