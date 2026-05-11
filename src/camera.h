#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include <linux/videodev2.h>

class Camera {
public:
    struct Config {
        std::string device = "/dev/video0";
        uint32_t width = 1280;
        uint32_t height = 800;
        uint32_t fps = 24;
        uint32_t pixelformat = V4L2_PIX_FMT_UYVY;
    };

    enum class PixelMode {
        UYVY,
        NV12,
        NV21,
    };

    explicit Camera(const Config &cfg);
    ~Camera();

    bool start();
    void stop();

    bool ready() const;
    const uint8_t *frame_data() const;
    bool copy_latest_frame(std::vector<uint8_t> &out_rgb, uint64_t &seq, uint64_t &ts_ns) const;
    bool copy_latest_frames(std::vector<uint8_t> &out_rgb,
                            std::vector<uint8_t> &out_raw,
                            uint64_t &seq,
                            uint64_t &ts_ns,
                            uint32_t &pixfmt) const;
    void set_frame_ready_callback(std::function<void()> cb);
    size_t frame_size() const;
    uint32_t width() const;
    uint32_t height() const;
	uint32_t pixel_format() const ;
	uint32_t frame_rate() const;
    uint64_t frame_counter() const { return frame_counter_.load(std::memory_order_acquire); }

private:
    struct Plane {
        void *start = nullptr;
        size_t length = 0;
        int fd = -1; // optional dmabuf fd (via EXPBUF)
    };

    struct Buffer {
        std::vector<Plane> planes;
    };


    bool open_device();
    bool set_format();
    bool request_buffers();
    bool start_stream();
    void capture_loop();
    bool dequeue_and_convert(int write_index);
    void cleanup_buffers();
    bool open_dma_heap();
    int alloc_dma_buf(size_t length);

    static int xioctl(int fd, unsigned long request, void *arg);
    static inline void yuv_to_rgb_pair(int y0, int y1, int u, int v, uint8_t *dst);
    static void yuyv_to_rgb888(const uint8_t *src, uint8_t *dst, uint32_t pixel_count);
    static void nv12_to_rgb888(const uint8_t *y, const uint8_t *uv, uint8_t *dst,
                                 uint32_t width, uint32_t height);

    int fd_ = -1;
    Config cfg_{};
    size_t rgb_size_ = 0;
    std::vector<Buffer> buffers_{};
    std::vector<uint8_t> rgb_[2];
    std::vector<uint8_t> raw_[2];
    uint32_t num_planes_ = 1;
    bool is_mplane_ = false;
    PixelMode pix_mode_ = PixelMode::UYVY;
    bool dmabuf_export_supported_ = false;
    std::atomic<int> latest_{-1};
    std::atomic<uint64_t> frame_counter_{0};
    std::atomic<uint64_t> frame_ts_ns_[2]{{0}, {0}};
    std::atomic<uint64_t> frame_seq_[2]{{0}, {0}};
    std::atomic<bool> running_{false};
    std::thread worker_{};
    bool using_dmabuf_ = false;
    int dma_heap_fd_ = -1;
    uint32_t plane_stride_[VIDEO_MAX_PLANES] = {0};
    std::function<void()> frame_ready_cb_;
    mutable std::mutex frame_ready_cb_mtx_;
};
