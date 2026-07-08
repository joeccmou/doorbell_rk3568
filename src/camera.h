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
    struct MediaFrame {
        const uint8_t *data = nullptr;
        size_t size = 0;
        int fd = -1;
        uint32_t width = 0;
        uint32_t height = 0;
        uint32_t pixfmt = V4L2_PIX_FMT_NV12;
        uint32_t stride_y = 0;
        uint32_t stride_uv = 0;
        uint64_t seq = 0;
        uint64_t ts_ns = 0;
    };

    struct Config {
        std::string device = "/dev/video0";
        uint32_t width = 1280;
        uint32_t height = 720;
        uint32_t fps = 24;
        uint32_t pixelformat = V4L2_PIX_FMT_NV12;
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
    bool copy_latest_media_frame(MediaFrame &out_frame) const;
    void set_frame_ready_callback(std::function<void()> cb);
    size_t frame_size() const;
    uint32_t width() const;
    uint32_t height() const;
    uint32_t pixel_format() const;
    uint32_t media_pixel_format() const;
    uint32_t frame_rate() const;
    uint64_t frame_counter() const { return frame_counter_.load(std::memory_order_acquire); }

private:
    struct Plane {
        void *start = nullptr;
        size_t length = 0;
        int fd = -1;
    };

    struct Buffer {
        std::vector<Plane> planes;
    };

    struct MediaBuffer {
        void *start = nullptr;
        size_t length = 0;
        int fd = -1;
        uint32_t stride_y = 0;
        uint32_t stride_uv = 0;
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
    bool alloc_media_buffers();
    bool fill_media_buffer_from_uyvy(int write_index,
                                     const uint8_t *src,
                                     uint32_t src_stride,
                                     uint32_t src_rows,
                                     int src_fd);
    bool fill_media_buffer_from_nv12(int write_index,
                                     const uint8_t *y_base,
                                     const uint8_t *uv_base,
                                     uint32_t y_stride,
                                     uint32_t uv_stride,
                                     uint32_t rows_y,
                                     uint32_t rows_uv);
    bool fill_media_buffer_from_nv21(int write_index,
                                     const uint8_t *y_base,
                                     const uint8_t *vu_base,
                                     uint32_t y_stride,
                                     uint32_t vu_stride,
                                     uint32_t rows_y,
                                     uint32_t rows_vu);
    bool media_to_rgb(int write_index);

    static int xioctl(int fd, unsigned long request, void *arg);
    static inline void yuv_to_rgb_pair(int y0, int y1, int u, int v, uint8_t *dst);
    static void yuyv_to_rgb888(const uint8_t *src, uint8_t *dst, uint32_t pixel_count);
    static void nv12_to_rgb888(const uint8_t *y, const uint8_t *uv, uint8_t *dst,
                               uint32_t width, uint32_t height);
    static void uyvy_to_nv12(const uint8_t *src,
                             uint32_t src_stride,
                             uint8_t *dst_y,
                             uint8_t *dst_uv,
                             uint32_t width,
                             uint32_t height);
    static void nv21_to_nv12(uint8_t *dst_uv, const uint8_t *src_vu, uint32_t width, uint32_t chroma_rows);

    int fd_ = -1;
    Config cfg_{};
    size_t rgb_size_ = 0;
    size_t media_size_ = 0;
    std::vector<Buffer> buffers_{};
    std::vector<uint8_t> rgb_[2];
    MediaBuffer media_[2];
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