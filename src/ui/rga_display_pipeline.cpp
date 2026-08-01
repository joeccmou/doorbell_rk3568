#include "rga_display_pipeline.h"

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <iterator>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#if __has_include(<linux/dma-heap.h>)
#include <linux/dma-heap.h>
#else
#include "../../../kernel/include/uapi/linux/dma-heap.h"
#endif
#if __has_include(<linux/dma-buf.h>)
#include <linux/dma-buf.h>
#else
#include "../../../kernel/include/uapi/linux/dma-buf.h"
#endif

#if !defined(DOORBELL_DISABLE_RGA)
#include <rga/im2d.h>
#endif

#include "utils/perf_logger.h"

namespace {
constexpr const char *kDmaHeapDefault = "/dev/dma_heap/system-dma32";
constexpr int kPipelineUnavailable = -10000;
constexpr int kBufferAllocationFailed = -10001;
constexpr int kHandleImportFailed = -10002;
constexpr int kSelfTestVerificationFailed = -10003;

const char *select_dma_heap_path() {
    static const char *chosen = [] {
        if (const char *env = std::getenv("DOORBELL_DMA_HEAP")) return env;
        static const char *candidates[] = {
            "/dev/dma_heap/system-dma32",
            "/dev/dma_heap/system",
            "/dev/dma_heap/cma",
        };
        for (const char *path : candidates) {
            if (::access(path, F_OK) == 0) return path;
        }
        return kDmaHeapDefault;
    }();
    return chosen;
}

const char *rga_error_text(int status) {
    switch (status) {
        case kPipelineUnavailable: return "RGA unavailable in this build";
        case kBufferAllocationFailed: return "DMA-BUF allocation failed";
        case kHandleImportFailed: return "RGA handle import failed";
        case kSelfTestVerificationFailed: return "self-test output verification failed";
        default: break;
    }
#if defined(DOORBELL_DISABLE_RGA)
    (void)status;
    return "RGA unavailable in this build";
#else
    const char *text = imStrError(static_cast<IM_STATUS>(status));
    return text ? text : "unknown RGA error";
#endif
}

bool allocate_mapped_buffer(int heap_fd,
                            size_t size,
                            const char *name,
                            int &buffer_fd,
                            void *&mapping) {
    dma_heap_allocation_data allocation{};
    allocation.len = size;
    allocation.fd_flags = O_RDWR | O_CLOEXEC;
    allocation.heap_flags = 0;
    if (::ioctl(heap_fd, DMA_HEAP_IOCTL_ALLOC, &allocation) < 0) {
        std::fprintf(stderr,
                     "[rga-display] allocate %s DMA-BUF failed size=%zu errno=%d error=%s\n",
                     name, size, errno, std::strerror(errno));
        return false;
    }

    buffer_fd = static_cast<int>(allocation.fd);
    mapping = ::mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, buffer_fd, 0);
    if (mapping == MAP_FAILED) {
        std::fprintf(stderr,
                     "[rga-display] mmap %s DMA-BUF failed size=%zu fd=%d errno=%d error=%s\n",
                     name, size, buffer_fd, errno, std::strerror(errno));
        mapping = nullptr;
        ::close(buffer_fd);
        buffer_fd = -1;
        return false;
    }

    std::memset(mapping, 0, size);
    return true;
}

void sync_dma_buffer(int fd, uint64_t flags) {
    if (fd < 0) return;
    dma_buf_sync sync{};
    sync.flags = flags;
    (void)::ioctl(fd, DMA_BUF_IOCTL_SYNC, &sync);
}
}  // namespace

RgaDisplayPipeline::RgaDisplayPipeline(uint32_t source_width,
                                       uint32_t source_height,
                                       uint32_t display_width,
                                       uint32_t display_height)
    : source_width_(source_width),
      source_height_(source_height),
      display_width_(display_width),
      display_height_(display_height),
      source_size_(static_cast<size_t>(source_width) *
                   static_cast<size_t>(source_height) * 3),
      output_size_(static_cast<size_t>(display_width) *
                   static_cast<size_t>(display_height) * 4) {}

RgaDisplayPipeline::~RgaDisplayPipeline() {
    release_handles();
    release_buffers();
}

bool RgaDisplayPipeline::initialize() {
    return attempt_recovery(std::chrono::steady_clock::now());
}

RgaDisplayPipeline::RenderResult RgaDisplayPipeline::render(const uint8_t *rgb,
                                                            size_t size) {
    if (!rgb || size < source_size_) return RenderResult::Failed;

    const auto now = std::chrono::steady_clock::now();
    if (!ready_) {
        if (now < next_retry_at_) return RenderResult::Backoff;
        if (!attempt_recovery(now)) return RenderResult::Failed;
    }

    sync_dma_buffer(source_fd_, DMA_BUF_SYNC_START | DMA_BUF_SYNC_WRITE);
    std::memcpy(source_map_, rgb, source_size_);
    sync_dma_buffer(source_fd_, DMA_BUF_SYNC_END | DMA_BUF_SYNC_WRITE);
    const int status = run_conversion();
    if (status <= 0) {
        ready_ = false;
        release_handles();
        record_failure("runtime_convert", status, now);
        return RenderResult::Failed;
    }
    sync_dma_buffer(output_fd_, DMA_BUF_SYNC_START | DMA_BUF_SYNC_READ);
    sync_dma_buffer(output_fd_, DMA_BUF_SYNC_END | DMA_BUF_SYNC_READ);
    return RenderResult::Rendered;
}

const uint8_t *RgaDisplayPipeline::output_data() const {
    return static_cast<const uint8_t *>(output_map_);
}

size_t RgaDisplayPipeline::output_size() const {
    return output_size_;
}

bool RgaDisplayPipeline::ready() const {
    return ready_;
}

bool RgaDisplayPipeline::attempt_recovery(std::chrono::steady_clock::time_point now) {
#if defined(DOORBELL_DISABLE_RGA)
    record_failure("initialize", kPipelineUnavailable, now);
    return false;
#else
    if (!ensure_buffers()) {
        record_failure("allocate_dmabuf", kBufferAllocationFailed, now);
        return false;
    }
    if (!import_handles()) {
        release_handles();
        record_failure("import_handle", kHandleImportFailed, now);
        return false;
    }
    const int self_test_status = run_self_test();
    if (self_test_status <= 0) {
        release_handles();
        record_failure("startup_self_test", self_test_status, now);
        return false;
    }

    ready_ = true;
    record_recovery();
    return true;
#endif
}

bool RgaDisplayPipeline::ensure_buffers() {
    if (source_map_ && output_map_ && source_fd_ >= 0 && output_fd_ >= 0) {
        return true;
    }

    if (heap_fd_ < 0) {
        heap_fd_ = ::open(select_dma_heap_path(), O_RDWR | O_CLOEXEC);
        if (heap_fd_ < 0) {
            std::fprintf(stderr,
                         "[rga-display] open DMA heap failed path=%s errno=%d error=%s\n",
                         select_dma_heap_path(), errno, std::strerror(errno));
            return false;
        }
    }

    if (source_fd_ < 0 &&
        !allocate_mapped_buffer(heap_fd_, source_size_, "source",
                                source_fd_, source_map_)) {
        return false;
    }
    if (output_fd_ < 0 &&
        !allocate_mapped_buffer(heap_fd_, output_size_, "output",
                                output_fd_, output_map_)) {
        return false;
    }
    return true;
}

bool RgaDisplayPipeline::import_handles() {
#if defined(DOORBELL_DISABLE_RGA)
    return false;
#else
    if (source_handle_ <= 0) {
        im_handle_param_t source_param{};
        source_param.width = static_cast<int>(source_width_);
        source_param.height = static_cast<int>(source_height_);
        source_param.format = RK_FORMAT_RGB_888;
        source_handle_ = importbuffer_fd(source_fd_, &source_param);
    }
    if (source_handle_ <= 0) return false;

    if (output_handle_ <= 0) {
        im_handle_param_t output_param{};
        output_param.width = static_cast<int>(display_width_);
        output_param.height = static_cast<int>(display_height_);
        output_param.format = RK_FORMAT_BGRA_8888;
        output_handle_ = importbuffer_fd(output_fd_, &output_param);
    }
    return output_handle_ > 0;
#endif
}

int RgaDisplayPipeline::run_self_test() {
#if defined(DOORBELL_DISABLE_RGA)
    return kPipelineUnavailable;
#else
    auto *source = static_cast<uint8_t *>(source_map_);
    sync_dma_buffer(source_fd_, DMA_BUF_SYNC_START | DMA_BUF_SYNC_WRITE);
    for (size_t offset = 0; offset + 2 < source_size_; offset += 3) {
        source[offset] = 0x24;
        source[offset + 1] = 0x79;
        source[offset + 2] = 0xD2;
    }
    sync_dma_buffer(source_fd_, DMA_BUF_SYNC_END | DMA_BUF_SYNC_WRITE);
    sync_dma_buffer(output_fd_, DMA_BUF_SYNC_START | DMA_BUF_SYNC_WRITE);
    std::memset(output_map_, 0xA5, output_size_);
    sync_dma_buffer(output_fd_, DMA_BUF_SYNC_END | DMA_BUF_SYNC_WRITE);

    const int status = run_conversion();
    if (status <= 0) return status;

    sync_dma_buffer(output_fd_, DMA_BUF_SYNC_START |
                                    DMA_BUF_SYNC_READ |
                                    DMA_BUF_SYNC_WRITE);
    const auto *output = static_cast<const uint8_t *>(output_map_);
    constexpr size_t kSampleCount = 32;
    size_t changed = 0;
    for (size_t i = 0; i < kSampleCount; ++i) {
        const size_t pixel =
            (i * (static_cast<size_t>(display_width_) *
                  static_cast<size_t>(display_height_) - 1)) /
            (kSampleCount - 1);
        const size_t offset = pixel * 4;
        if (output[offset] != 0xA5 || output[offset + 1] != 0xA5 ||
            output[offset + 2] != 0xA5 || output[offset + 3] != 0xA5) {
            ++changed;
        }
    }
    std::memset(output_map_, 0, output_size_);
    sync_dma_buffer(output_fd_, DMA_BUF_SYNC_END |
                                    DMA_BUF_SYNC_READ |
                                    DMA_BUF_SYNC_WRITE);
    if (changed != kSampleCount) return kSelfTestVerificationFailed;

    std::fprintf(stdout,
                 "[rga-display] self-test passed: RGB888 %ux%u -> BGRA8888 %ux%u "
                 "source_fd=%d output_fd=%d source_handle=%llu output_handle=%llu\n",
                 source_width_, source_height_, display_width_, display_height_,
                 source_fd_,
                 output_fd_,
                 static_cast<unsigned long long>(source_handle_),
                 static_cast<unsigned long long>(output_handle_));
    perf_logger_log(
        "rga_display_self_test result=passed src=%ux%u dst=%ux%u source_fd=%d output_fd=%d\n",
        source_width_, source_height_, display_width_, display_height_,
        source_fd_, output_fd_);
    return status;
#endif
}

int RgaDisplayPipeline::run_conversion() {
#if defined(DOORBELL_DISABLE_RGA)
    return kPipelineUnavailable;
#else
    if (source_handle_ <= 0 || output_handle_ <= 0) return kHandleImportFailed;

    rga_buffer_t source = wrapbuffer_handle(
        source_handle_,
        static_cast<int>(source_width_),
        static_cast<int>(source_height_),
        RK_FORMAT_RGB_888,
        static_cast<int>(source_width_),
        static_cast<int>(source_height_));
    rga_buffer_t output = wrapbuffer_handle(
        output_handle_,
        static_cast<int>(display_width_),
        static_cast<int>(display_height_),
        RK_FORMAT_BGRA_8888,
        static_cast<int>(display_width_),
        static_cast<int>(display_height_));
    rga_buffer_t pattern{};
    im_rect source_rect{
        0, 0, static_cast<int>(source_width_), static_cast<int>(source_height_)};
    im_rect output_rect{
        0, 0, static_cast<int>(display_width_), static_cast<int>(display_height_)};
    im_rect pattern_rect{};
    const IM_STATUS status = improcess(
        source, output, pattern, source_rect, output_rect, pattern_rect, 0);
    return static_cast<int>(status);
#endif
}

void RgaDisplayPipeline::release_handles() {
#if !defined(DOORBELL_DISABLE_RGA)
    if (source_handle_ > 0) {
        releasebuffer_handle(source_handle_);
        source_handle_ = 0;
    }
    if (output_handle_ > 0) {
        releasebuffer_handle(output_handle_);
        output_handle_ = 0;
    }
#else
    source_handle_ = 0;
    output_handle_ = 0;
#endif
}

void RgaDisplayPipeline::release_buffers() {
    if (source_map_) {
        ::munmap(source_map_, source_size_);
        source_map_ = nullptr;
    }
    if (output_map_) {
        ::munmap(output_map_, output_size_);
        output_map_ = nullptr;
    }
    if (source_fd_ >= 0) {
        ::close(source_fd_);
        source_fd_ = -1;
    }
    if (output_fd_ >= 0) {
        ::close(output_fd_);
        output_fd_ = -1;
    }
    if (heap_fd_ >= 0) {
        ::close(heap_fd_);
        heap_fd_ = -1;
    }
}

void RgaDisplayPipeline::record_failure(
    const char *stage,
    int status,
    std::chrono::steady_clock::time_point now) {
    has_failed_ = true;
    ready_ = false;
    ++consecutive_failures_;
    const auto delay = retry_delay();
    next_retry_at_ = now + delay;
    std::fprintf(
        stderr,
        "[rga-display] local preview disabled stage=%s count=%u status=%d error=%s "
        "retry_ms=%lld; MQTT/AI/snapshot/recording/WebRTC continue\n",
        stage,
        consecutive_failures_,
        status,
        rga_error_text(status),
        static_cast<long long>(delay.count()));
    perf_logger_log(
        "rga_display result=failed stage=%s count=%u status=%d retry_ms=%lld\n",
        stage,
        consecutive_failures_,
        status,
        static_cast<long long>(delay.count()));
}

void RgaDisplayPipeline::record_recovery() {
    if (has_failed_) {
        std::fprintf(stdout,
                     "[rga-display] local preview recovered after %u failed attempt(s)\n",
                     consecutive_failures_);
        perf_logger_log("rga_display result=recovered failed_attempts=%u\n",
                        consecutive_failures_);
    }
    consecutive_failures_ = 0;
    next_retry_at_ = {};
}

std::chrono::milliseconds RgaDisplayPipeline::retry_delay() const {
    static constexpr uint32_t kRetrySeconds[] = {1, 2, 4, 8, 16, 30};
    const size_t index = std::min<size_t>(
        consecutive_failures_ > 0 ? consecutive_failures_ - 1 : 0,
        std::size(kRetrySeconds) - 1);
    return std::chrono::seconds(kRetrySeconds[index]);
}
