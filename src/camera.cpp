#include "camera.h"

#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/select.h>
#if __has_include(<linux/dma-heap.h>)
#include <linux/dma-heap.h>
#else
#include "../../../kernel/include/uapi/linux/dma-heap.h"
#endif
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <chrono>
#include <algorithm>
#if !defined(DOORBELL_DISABLE_RGA)
#include <rga/im2d.h>
#endif

#include "utils/perf_logger.h"

namespace {
constexpr uint32_t kBufferCount = 4;
constexpr const char *kDmaHeapDefault = "/dev/dma_heap/system-dma32";

size_t packed_raw_size(uint32_t pixfmt, uint32_t width, uint32_t height) {
	size_t w = static_cast<size_t>(width);
	size_t h = static_cast<size_t>(height);
	switch (pixfmt) {
		case V4L2_PIX_FMT_UYVY:
		case V4L2_PIX_FMT_YUYV:
		case V4L2_PIX_FMT_YUV422P:
			return w * h * 2;
		case V4L2_PIX_FMT_NV12:
		case V4L2_PIX_FMT_NV21:
			return w * h * 3 / 2;
		default:
			return 0;
	}
}

bool rga_enabled() {
	static int enabled = [] {
		const char *env = std::getenv("DOORBELL_RGA");
		if (env && (*env == '0' || *env == 'n' || *env == 'N')) return 0;
		return 1;
	}();
	return enabled != 0;
}

const char *select_dma_heap_path() {
	static const char *chosen = [] {
		// Allow override. Example: DOORBELL_DMA_HEAP=/dev/dma_heap/system-dma32
		if (const char *env = std::getenv("DOORBELL_DMA_HEAP")) return env;
		// Prefer 32-bit addressable heaps to keep RGA within <4G when possible.
		static const char *candidates[] = {
			"/dev/dma_heap/system-dma32",
			"/dev/dma_heap/system",
			"/dev/dma_heap/cma",
		};
		for (const char *p : candidates) {
			if (::access(p, F_OK) == 0) return p;
		}
		return kDmaHeapDefault;
	}();
	return chosen;
}

constexpr int kRgaUnavailable = -10000;
constexpr int kRgaInvalidBuffer = -10001;

const char *pixel_mode_name(Camera::PixelMode mode) {
	switch (mode) {
		case Camera::PixelMode::UYVY: return "UYVY";
		case Camera::PixelMode::NV12: return "NV12";
		case Camera::PixelMode::NV21: return "NV21";
	}
	return "UNKNOWN";
}

const char *rga_error_text(int status) {
	if (status == kRgaUnavailable) return "RGA disabled or unavailable";
	if (status == kRgaInvalidBuffer) return "invalid DMA-BUF or stride";
#if defined(DOORBELL_DISABLE_RGA)
	(void)status;
	return "RGA unavailable in this build";
#else
	return imStrError(static_cast<IM_STATUS>(status));
#endif
}

void log_rga_failure(CameraRgaState &state,
			     const char *operation,
			     Camera::PixelMode mode,
			     int status,
			     uint32_t width,
			     uint32_t height,
			     uint32_t src_stride,
			     uint32_t src_hstride,
			     int src_fd,
			     int dst_fd,
			     int usage) {
	if (!state.record_failure()) return;
	const uint32_t failures = state.consecutive_failures();
	const char *error = rga_error_text(status);
	if (!error) error = "unknown RGA error";
	std::fprintf(stderr,
		"[rga] operation=%s failed count=%u status=%d error=%s src=%s %ux%u stride=%u hstride=%u src_fd=%d dst_fd=%d usage=0x%x\n",
		operation,
		failures,
		status,
		error,
		pixel_mode_name(mode),
		width,
		height,
		src_stride,
		src_hstride,
		src_fd,
		dst_fd,
		usage);
	perf_logger_log(
		"rga_failure operation=%s count=%u status=%d error=%s src=%s width=%u height=%u stride=%u hstride=%u src_fd=%d dst_fd=%d usage=0x%x\n",
		operation,
		failures,
		status,
		error,
		pixel_mode_name(mode),
		width,
		height,
		src_stride,
		src_hstride,
		src_fd,
		dst_fd,
		usage);
}

bool rga_convert_to_rgb(Camera::PixelMode mode,
			uint32_t width,
			uint32_t height,
			uint32_t src_stride_bytes,
			uint32_t src_hstride,
			int src_fd,
			int dst_fd,
			CameraRgaState &state) {
#if defined(DOORBELL_DISABLE_RGA)
	log_rga_failure(state, "nv12_to_rgb", mode, kRgaUnavailable, width, height,
			 src_stride_bytes, src_hstride, src_fd, dst_fd, 0);
	return false;
#else
	if (!rga_enabled() || src_fd < 0 || dst_fd < 0 || !src_stride_bytes || !src_hstride) {
		log_rga_failure(state, "nv12_to_rgb", mode,
				 !rga_enabled() ? kRgaUnavailable : kRgaInvalidBuffer,
				 width, height, src_stride_bytes, src_hstride, src_fd, dst_fd, 0);
		return false;
	}

	int src_fmt = 0;
	uint32_t src_wstride_px = 0;
	switch (mode) {
		case Camera::PixelMode::UYVY:
			src_fmt = RK_FORMAT_UYVY_422;
			src_wstride_px = src_stride_bytes / 2;
			break;
		case Camera::PixelMode::NV12:
			src_fmt = RK_FORMAT_YCbCr_420_SP;
			src_wstride_px = src_stride_bytes;
			break;
		case Camera::PixelMode::NV21:
			src_fmt = RK_FORMAT_YCrCb_420_SP;
			src_wstride_px = src_stride_bytes;
			break;
	}

	rga_buffer_t src_buf = wrapbuffer_fd(src_fd, width, height, src_fmt,
					 static_cast<int>(src_wstride_px), static_cast<int>(src_hstride));
	rga_buffer_t dst_buf = wrapbuffer_fd(dst_fd, width, height, RK_FORMAT_RGB_888,
					 static_cast<int>(width), static_cast<int>(height));
	IM_STATUS status = imcvtcolor(src_buf, dst_buf, src_fmt, RK_FORMAT_RGB_888,
				       IM_YUV_TO_RGB_BT709_LIMIT, 1, nullptr);
	if (status != IM_STATUS_SUCCESS) {
		log_rga_failure(state, "nv12_to_rgb", mode, static_cast<int>(status), width, height,
				 src_stride_bytes, src_hstride, src_fd, dst_fd, 0);
		return false;
	}
	return true;
#endif
}

bool rga_transform_to_nv12(Camera::PixelMode mode,
			   uint32_t width,
			   uint32_t height,
			   uint32_t src_stride_bytes,
			   uint32_t src_hstride,
			   int src_fd,
			   int dst_fd,
			   bool rotate180,
			   CameraRgaState &state) {
#if defined(DOORBELL_DISABLE_RGA)
	const int usage = rotate180 ? 2 : 0;
	log_rga_failure(state, "capture_to_nv12", mode, kRgaUnavailable, width, height,
			 src_stride_bytes, src_hstride, src_fd, dst_fd, usage);
	return false;
#else
	const int usage = rotate180 ? IM_HAL_TRANSFORM_ROT_180 : 0;
	if (!rga_enabled() || src_fd < 0 || dst_fd < 0 || !src_stride_bytes || !src_hstride) {
		log_rga_failure(state, "capture_to_nv12", mode,
				 !rga_enabled() ? kRgaUnavailable : kRgaInvalidBuffer,
				 width, height, src_stride_bytes, src_hstride, src_fd, dst_fd, usage);
		return false;
	}

	int src_fmt = 0;
	uint32_t src_wstride_px = 0;
	switch (mode) {
		case Camera::PixelMode::UYVY:
			src_fmt = RK_FORMAT_UYVY_422;
			src_wstride_px = src_stride_bytes / 2;
			break;
		case Camera::PixelMode::NV12:
			src_fmt = RK_FORMAT_YCbCr_420_SP;
			src_wstride_px = src_stride_bytes;
			break;
		case Camera::PixelMode::NV21:
			src_fmt = RK_FORMAT_YCrCb_420_SP;
			src_wstride_px = src_stride_bytes;
			break;
	}

	rga_buffer_t src_buf = wrapbuffer_fd(src_fd, width, height, src_fmt,
					 static_cast<int>(src_wstride_px), static_cast<int>(src_hstride));
	rga_buffer_t dst_buf = wrapbuffer_fd(dst_fd, width, height, RK_FORMAT_YCbCr_420_SP,
					 static_cast<int>(width), static_cast<int>(height));
	im_rect srect = {0, 0, static_cast<int>(width), static_cast<int>(height)};
	im_rect drect = {0, 0, static_cast<int>(width), static_cast<int>(height)};
	im_rect prect{};
	rga_buffer_t pat{};
	IM_STATUS status = improcess(src_buf, dst_buf, pat, srect, drect, prect, usage);
	if (status != IM_STATUS_SUCCESS) {
		log_rga_failure(state, "capture_to_nv12", mode, static_cast<int>(status), width, height,
				 src_stride_bytes, src_hstride, src_fd, dst_fd, usage);
		return false;
	}
	return true;
#endif
}
}

Camera::Camera(const Config &cfg) : cfg_(cfg) {
	rgb_size_ = static_cast<size_t>(cfg_.width) * static_cast<size_t>(cfg_.height) * 3;
	media_size_ = packed_raw_size(V4L2_PIX_FMT_NV12, cfg_.width, cfg_.height);
}

Camera::~Camera() {
	stop();
}

bool Camera::start() {
	if (running_.load()) return true;

	if (!open_device()) return false;
	if (!set_format()) return false;
	if (!alloc_output_buffers()) return false;
	if (!request_buffers()) return false;
	if (!verify_rga_pipeline()) return false;
	if (!start_stream()) return false;

	std::fprintf(stdout, "[camera] started fmt=%c%c%c%c media=%c%c%c%c planes=%u %ux%u\n",
	    cfg_.pixelformat & 0xFF,
	    (cfg_.pixelformat >> 8) & 0xFF,
	    (cfg_.pixelformat >> 16) & 0xFF,
	    (cfg_.pixelformat >> 24) & 0xFF,
	    V4L2_PIX_FMT_NV12 & 0xFF,
	    (V4L2_PIX_FMT_NV12 >> 8) & 0xFF,
	    (V4L2_PIX_FMT_NV12 >> 16) & 0xFF,
	    (V4L2_PIX_FMT_NV12 >> 24) & 0xFF,
	    num_planes_, cfg_.width, cfg_.height);

	running_.store(true);
	worker_ = std::thread(&Camera::capture_loop, this);
	return true;
}

void Camera::stop() {
	running_.store(false);
	if (worker_.joinable()) {
		worker_.join();
	}

	if (fd_ >= 0) {
		enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
		xioctl(fd_, VIDIOC_STREAMOFF, &type);
		close(fd_);
		fd_ = -1;
	}

	cleanup_buffers();
}

bool Camera::ready() const {
	return latest_.load(std::memory_order_acquire) >= 0;
}

bool Camera::set_rotate180(bool enabled) {
	rga_state_.set_rotate180(enabled);
	return true;
}

bool Camera::rotate180() const {
	return rga_state_.rotate180();
}

const uint8_t *Camera::frame_data() const {
	int idx = latest_.load(std::memory_order_acquire);
	if (idx < 0 || !rgb_[idx].start) return nullptr;
	return static_cast<const uint8_t *>(rgb_[idx].start);
}

bool Camera::copy_latest_frame(std::vector<uint8_t> &out_rgb, uint64_t &seq, uint64_t &ts_ns) const {
	if (out_rgb.size() != rgb_size_) {
		out_rgb.resize(rgb_size_);
	}

	for (int attempt = 0; attempt < 3; ++attempt) {
		int idx = latest_.load(std::memory_order_acquire);
		if (idx < 0 || !rgb_[idx].start) return false;

		::memcpy(out_rgb.data(), rgb_[idx].start, rgb_size_);

		int idx_after = latest_.load(std::memory_order_acquire);
		if (idx == idx_after) {
			seq = frame_seq_[idx].load(std::memory_order_acquire);
			ts_ns = frame_ts_ns_[idx].load(std::memory_order_acquire);
			return true;
		}
	}

	int idx = latest_.load(std::memory_order_acquire);
	if (idx < 0) return false;
	seq = frame_seq_[idx].load(std::memory_order_acquire);
	ts_ns = frame_ts_ns_[idx].load(std::memory_order_acquire);
	return true;
}

bool Camera::copy_latest_frames(std::vector<uint8_t> &out_rgb,
								std::vector<uint8_t> &out_raw,
								uint64_t &seq,
								uint64_t &ts_ns,
								uint32_t &pixfmt) const {
	for (int attempt = 0; attempt < 3; ++attempt) {
		int idx = latest_.load(std::memory_order_acquire);
		if (idx < 0 || !media_[idx].start || !rgb_[idx].start) return false;

		if (out_rgb.size() != rgb_size_) out_rgb.resize(rgb_size_);
		::memcpy(out_rgb.data(), rgb_[idx].start, rgb_size_);

		if (out_raw.size() != media_size_) out_raw.resize(media_size_);
		::memcpy(out_raw.data(), media_[idx].start, media_size_);

		int idx_after = latest_.load(std::memory_order_acquire);
		if (idx == idx_after) {
			seq = frame_seq_[idx].load(std::memory_order_acquire);
			ts_ns = frame_ts_ns_[idx].load(std::memory_order_acquire);
			pixfmt = V4L2_PIX_FMT_NV12;
			return true;
		}
	}
	return false;
}

bool Camera::copy_latest_media_frame(MediaFrame &out_frame) const {
	for (int attempt = 0; attempt < 3; ++attempt) {
		int idx = latest_.load(std::memory_order_acquire);
		if (idx < 0 || !media_[idx].start) return false;

		out_frame.data = static_cast<const uint8_t *>(media_[idx].start);
		out_frame.size = media_size_;
		out_frame.fd = media_[idx].fd;
		out_frame.width = cfg_.width;
		out_frame.height = cfg_.height;
		out_frame.pixfmt = V4L2_PIX_FMT_NV12;
		out_frame.stride_y = media_[idx].stride_y;
		out_frame.stride_uv = media_[idx].stride_uv;

		int idx_after = latest_.load(std::memory_order_acquire);
		if (idx == idx_after) {
			out_frame.seq = frame_seq_[idx].load(std::memory_order_acquire);
			out_frame.ts_ns = frame_ts_ns_[idx].load(std::memory_order_acquire);
			return true;
		}
	}
	return false;
}

void Camera::set_frame_ready_callback(std::function<void()> cb) {
	std::lock_guard<std::mutex> lock(frame_ready_cb_mtx_);
	frame_ready_cb_ = std::move(cb);
}

uint32_t Camera::frame_rate() const {
	return cfg_.fps;
}

uint32_t Camera::pixel_format() const {
	return cfg_.pixelformat;
}

uint32_t Camera::media_pixel_format() const {
	return V4L2_PIX_FMT_NV12;
}

size_t Camera::frame_size() const {
	return rgb_size_;
}

uint32_t Camera::width() const {
	return cfg_.width;
}

uint32_t Camera::height() const {
	return cfg_.height;
}

bool Camera::open_dma_heap() {	if (dma_heap_fd_ >= 0) return true;
	const char *heap = select_dma_heap_path();
	dma_heap_fd_ = open(heap, O_RDWR | O_CLOEXEC);
	if (dma_heap_fd_ < 0) {
		std::fprintf(stderr, "open dma_heap failed (%s): %s\n", heap, strerror(errno));
		return false;
	}
	std::fprintf(stdout, "[camera] using dma_heap %s\n", heap);
	return true;
}

int Camera::alloc_dma_buf(size_t length) {
	if (length == 0) return -1;
	if (!open_dma_heap()) return -1;

	struct dma_heap_allocation_data data;
	memset(&data, 0, sizeof(data));
	data.len = length;
	data.fd_flags = O_CLOEXEC | O_RDWR;

	if (ioctl(dma_heap_fd_, DMA_HEAP_IOCTL_ALLOC, &data) < 0) {
		std::perror("DMA_HEAP_IOCTL_ALLOC failed");
		return -1;
	}
	return static_cast<int>(data.fd);
}

bool Camera::alloc_output_buffers() {
	if (media_size_ == 0 || rgb_size_ == 0) {
		return false;
	}

	auto release_buffer = [](MediaBuffer &buffer) {
		if (buffer.start && buffer.length) {
			munmap(buffer.start, buffer.length);
			buffer.start = nullptr;
		}
		if (buffer.fd >= 0) {
			close(buffer.fd);
			buffer.fd = -1;
		}
		buffer.length = 0;
	};

	auto allocate_buffer = [this, &release_buffer](MediaBuffer &buffer,
							  size_t length,
							  const char *name) {
		release_buffer(buffer);
		buffer.fd = alloc_dma_buf(length);
		if (buffer.fd < 0) return false;
		buffer.length = length;
		buffer.start = mmap(NULL, buffer.length, PROT_READ | PROT_WRITE, MAP_SHARED, buffer.fd, 0);
		if (buffer.start == MAP_FAILED) {
			std::fprintf(stderr, "mmap %s dma-buf failed: %s\n", name, strerror(errno));
			close(buffer.fd);
			buffer.fd = -1;
			buffer.start = nullptr;
			buffer.length = 0;
			return false;
		}
		memset(buffer.start, 0, buffer.length);
		return true;
	};

	for (auto &buffer : media_) {
		if (!allocate_buffer(buffer, media_size_, "media")) return false;
		buffer.stride_y = cfg_.width;
		buffer.stride_uv = cfg_.width;
	}
	for (auto &buffer : rgb_) {
		if (!allocate_buffer(buffer, rgb_size_, "rgb")) return false;
		buffer.stride_y = cfg_.width * 3;
		buffer.stride_uv = 0;
	}
	return true;
}

bool Camera::verify_rga_pipeline() {
	if (media_[0].fd < 0 || media_[1].fd < 0 || rgb_[0].fd < 0) {
		log_rga_failure(rga_state_, "startup_self_test", PixelMode::NV12,
				 kRgaInvalidBuffer, cfg_.width, cfg_.height, cfg_.width,
				 cfg_.height, media_[0].fd, media_[1].fd, 0);
		return false;
	}

	if (!rga_transform_to_nv12(PixelMode::NV12, cfg_.width, cfg_.height,
				     cfg_.width, cfg_.height, media_[0].fd, media_[1].fd,
				     false, rga_state_) ||
		!rga_transform_to_nv12(PixelMode::NV12, cfg_.width, cfg_.height,
				     cfg_.width, cfg_.height, media_[0].fd, media_[1].fd,
				     true, rga_state_) ||
		!rga_convert_to_rgb(PixelMode::NV12, cfg_.width, cfg_.height,
				    cfg_.width, cfg_.height, media_[1].fd, rgb_[0].fd,
				    rga_state_)) {
		std::fprintf(stderr, "[rga] startup self-test failed; camera will not start\n");
		perf_logger_log("rga_startup_self_test result=failed\n");
		return false;
	}

	rga_state_.record_success();
	std::fprintf(stdout, "[rga] startup self-test passed: NV12 copy/rotate180 and NV12->RGB888\n");
	perf_logger_log("rga_startup_self_test result=passed\n");
	return true;
}

bool Camera::open_device() {
	fd_ = open(cfg_.device.c_str(), O_RDWR | O_NONBLOCK, 0);
	if (fd_ < 0) {
		std::perror("open video device failed");
		return false;
	}
	return true;
}

bool Camera::set_format() {
	struct v4l2_format fmt;
	memset(&fmt, 0, sizeof(fmt));
	fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
	fmt.fmt.pix_mp.width = cfg_.width;
	fmt.fmt.pix_mp.height = cfg_.height;
	fmt.fmt.pix_mp.pixelformat = cfg_.pixelformat;
	fmt.fmt.pix_mp.field = V4L2_FIELD_NONE;

	switch (cfg_.pixelformat) {
		case V4L2_PIX_FMT_UYVY:
		case V4L2_PIX_FMT_YUYV:
			fmt.fmt.pix_mp.num_planes = 1;
			break;
		case V4L2_PIX_FMT_YUV422P:
			fmt.fmt.pix_mp.num_planes = 3;
			break;
		default:
			fmt.fmt.pix_mp.num_planes = 2;
			break;
	}

	if (xioctl(fd_, VIDIOC_S_FMT, &fmt) == -1) {
		std::perror("VIDIOC_S_FMT failed");
		return false;
	}

	// Capture the actual format the driver accepted.
	cfg_.pixelformat = fmt.fmt.pix_mp.pixelformat;
	cfg_.width = fmt.fmt.pix_mp.width;
	cfg_.height = fmt.fmt.pix_mp.height;
	rgb_size_ = static_cast<size_t>(cfg_.width) * static_cast<size_t>(cfg_.height) * 3;
	media_size_ = packed_raw_size(V4L2_PIX_FMT_NV12, cfg_.width, cfg_.height);
	is_mplane_ = true;
	num_planes_ = fmt.fmt.pix_mp.num_planes ? fmt.fmt.pix_mp.num_planes : 1;
	for (uint32_t p = 0; p < num_planes_ && p < VIDEO_MAX_PLANES; ++p) {
		plane_stride_[p] = fmt.fmt.pix_mp.plane_fmt[p].bytesperline;
	}

	switch (cfg_.pixelformat) {
		case V4L2_PIX_FMT_UYVY: pix_mode_ = PixelMode::UYVY; break;
		case V4L2_PIX_FMT_NV21: pix_mode_ = PixelMode::NV21; break;
		case V4L2_PIX_FMT_NV12: pix_mode_ = PixelMode::NV12; break;
		default:
			std::fprintf(stderr, "[camera] RGA-only pipeline does not support capture format %c%c%c%c\n",
				cfg_.pixelformat & 0xFF,
				(cfg_.pixelformat >> 8) & 0xFF,
				(cfg_.pixelformat >> 16) & 0xFF,
				(cfg_.pixelformat >> 24) & 0xFF);
			return false;
	}
	if (num_planes_ != 1) {
		std::fprintf(stderr,
			"[camera] RGA-only pipeline requires one DMA-BUF plane, driver returned %u planes\n",
			num_planes_);
		return false;
	}

	struct v4l2_streamparm parm;
	memset(&parm, 0, sizeof(parm));
	parm.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
	parm.parm.capture.timeperframe.numerator = 1;
	parm.parm.capture.timeperframe.denominator = cfg_.fps;
	xioctl(fd_, VIDIOC_S_PARM, &parm);
	return true;
}

bool Camera::request_buffers() {
	struct v4l2_requestbuffers req;
	memset(&req, 0, sizeof(req));
	req.count = kBufferCount;
	req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
	req.memory = V4L2_MEMORY_DMABUF;

	if (xioctl(fd_, VIDIOC_REQBUFS, &req) == -1) {
		std::perror("VIDIOC_REQBUFS dmabuf failed, falling back to mmap");
		req.memory = V4L2_MEMORY_MMAP;
		if (xioctl(fd_, VIDIOC_REQBUFS, &req) == -1) {
			std::perror("VIDIOC_REQBUFS mmap failed");
			return false;
		}
		using_dmabuf_ = false;
	} else {
		using_dmabuf_ = true;
	}

	if (req.count < 2) {
		std::fprintf(stderr, "Insufficient buffer memory on %s\n", cfg_.device.c_str());
		return false;
	}

	buffers_.resize(req.count);
	for (uint32_t i = 0; i < req.count; ++i) {
		struct v4l2_buffer buf;
		memset(&buf, 0, sizeof(buf));
		buf.type = req.type;
		buf.memory = using_dmabuf_ ? V4L2_MEMORY_DMABUF : V4L2_MEMORY_MMAP;
		buf.index = i;

		v4l2_plane planes[VIDEO_MAX_PLANES];
		memset(planes, 0, sizeof(planes));
		buf.length = num_planes_;
		buf.m.planes = planes;

		if (xioctl(fd_, VIDIOC_QUERYBUF, &buf) == -1) {
			std::perror("VIDIOC_QUERYBUF failed");
			return false;
		}

		buffers_[i].planes.resize(num_planes_);
		for (uint32_t p = 0; p < num_planes_; ++p) {
			buffers_[i].planes[p].length = planes[p].length;
			if (planes[p].length == 0) {
				std::fprintf(stderr, "Plane %u length is 0 (fmt=%c%c%c%c)\n",
						 p,
						 cfg_.pixelformat & 0xFF,
						 (cfg_.pixelformat >> 8) & 0xFF,
						 (cfg_.pixelformat >> 16) & 0xFF,
						 (cfg_.pixelformat >> 24) & 0xFF);
				return false;
			}

			if (using_dmabuf_) {
				int fd = alloc_dma_buf(planes[p].length);
				if (fd < 0) return false;
				buffers_[i].planes[p].fd = fd;
				buffers_[i].planes[p].start = mmap(NULL, planes[p].length, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
				if (buffers_[i].planes[p].start == MAP_FAILED) {
					std::perror("mmap dma-buf failed");
					close(fd);
					buffers_[i].planes[p].fd = -1;
					return false;
				}
			} else {
				buffers_[i].planes[p].start = mmap(NULL, planes[p].length, PROT_READ | PROT_WRITE, MAP_SHARED, fd_, planes[p].m.mem_offset);
				if (buffers_[i].planes[p].start == MAP_FAILED) {
					std::perror("mmap failed");
					return false;
				}
				// Export DMABUF fd for this plane if supported.
				if (!dmabuf_export_supported_) {
					struct v4l2_exportbuffer exp_probe;
					memset(&exp_probe, 0, sizeof(exp_probe));
					exp_probe.type = req.type;
					exp_probe.index = i;
					exp_probe.plane = p;
					exp_probe.flags = O_CLOEXEC;
					if (xioctl(fd_, VIDIOC_EXPBUF, &exp_probe) == 0) {
						dmabuf_export_supported_ = true;
						buffers_[i].planes[p].fd = exp_probe.fd;
					} else {
						buffers_[i].planes[p].fd = -1;
					}
				} else {
					struct v4l2_exportbuffer exp;
					memset(&exp, 0, sizeof(exp));
					exp.type = req.type;
					exp.index = i;
					exp.plane = p;
					exp.flags = O_CLOEXEC;
					if (xioctl(fd_, VIDIOC_EXPBUF, &exp) == 0) {
						buffers_[i].planes[p].fd = exp.fd;
					} else {
						buffers_[i].planes[p].fd = -1;
					}
				}
			}
			if (buffers_[i].planes[p].fd < 0) {
				std::fprintf(stderr,
					"[camera] RGA-only pipeline requires an exported DMA-BUF fd for buffer=%u plane=%u\n",
					i,
					p);
				return false;
			}
		}
	}

	for (uint32_t i = 0; i < buffers_.size(); ++i) {
		struct v4l2_buffer buf;
		memset(&buf, 0, sizeof(buf));
		buf.type = req.type;
		buf.memory = using_dmabuf_ ? V4L2_MEMORY_DMABUF : V4L2_MEMORY_MMAP;
		buf.index = i;

		v4l2_plane planes[VIDEO_MAX_PLANES];
		memset(planes, 0, sizeof(planes));
		buf.length = num_planes_;
		buf.m.planes = planes;

		if (using_dmabuf_) {
			for (uint32_t p = 0; p < num_planes_; ++p) {
				planes[p].m.fd = buffers_[i].planes[p].fd;
				planes[p].length = buffers_[i].planes[p].length;
				planes[p].bytesused = buffers_[i].planes[p].length;
				planes[p].data_offset = 0;
			}
		}

		if (xioctl(fd_, VIDIOC_QBUF, &buf) == -1) {
			std::perror("VIDIOC_QBUF failed");
			return false;
		}
	}

	if (using_dmabuf_) {
		std::fprintf(stdout, "[camera] using V4L2_MEMORY_DMABUF via %s\n", select_dma_heap_path());
	} else {
		std::fprintf(stdout, "[camera] v4l2 dmabuf export support: %s\n", dmabuf_export_supported_ ? "yes" : "no");
	}
	return true;
}

bool Camera::start_stream() {
	enum v4l2_buf_type type = is_mplane_ ? V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE : V4L2_BUF_TYPE_VIDEO_CAPTURE;
	if (xioctl(fd_, VIDIOC_STREAMON, &type) == -1) {
		std::perror("VIDIOC_STREAMON failed");
		return false;
	}
	return true;
}

void Camera::capture_loop() {
	int write_index = 0;
	using clock = std::chrono::steady_clock;
	uint64_t last_dequeue_ns = 0;
	while (running_.load()) {
		fd_set fds;
		FD_ZERO(&fds);
		FD_SET(fd_, &fds);

		struct timeval tv;
		tv.tv_sec = 2;
		tv.tv_usec = 0;

		int r = select(fd_ + 1, &fds, NULL, NULL, &tv);
		if (r < 0) {
			if (errno == EINTR) continue;
			std::perror("select failed");
			break;
		}
		if (r == 0) continue;

		auto dq_begin = clock::now();
		if (dequeue_and_convert(write_index)) {
			auto dq_end = clock::now();
			auto now_ns = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
				dq_end.time_since_epoch()).count());
			uint64_t seq = frame_counter_.fetch_add(1, std::memory_order_relaxed) + 1;
			double dequeue_ms = static_cast<double>(
				std::chrono::duration_cast<std::chrono::nanoseconds>(dq_end - dq_begin).count()) / 1000000.0;
			double interval_ms = last_dequeue_ns
				? static_cast<double>(now_ns - last_dequeue_ns) / 1000000.0
				: 0.0;
			perf_logger_log("[camera_capture] dequeue_and_convert seq=%llu interval_ms=%.3f dequeue_ms=%.3f\n",
				static_cast<unsigned long long>(seq),
				interval_ms,
				dequeue_ms);
			last_dequeue_ns = now_ns;
			frame_ts_ns_[write_index].store(now_ns, std::memory_order_release);
			frame_seq_[write_index].store(seq, std::memory_order_release);
			latest_.store(write_index, std::memory_order_release);
			std::function<void()> frame_ready_cb;
			{
				std::lock_guard<std::mutex> lock(frame_ready_cb_mtx_);
				frame_ready_cb = frame_ready_cb_;
			}
			if (frame_ready_cb) {
				frame_ready_cb();
			}
			write_index ^= 1;
		}
	}
}

bool Camera::dequeue_and_convert(int write_index) {
	struct v4l2_buffer buf;
	memset(&buf, 0, sizeof(buf));
	buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
	buf.memory = using_dmabuf_ ? V4L2_MEMORY_DMABUF : V4L2_MEMORY_MMAP;

	v4l2_plane planes[VIDEO_MAX_PLANES];
	memset(planes, 0, sizeof(planes));
	buf.length = num_planes_;
	buf.m.planes = planes;

	if (xioctl(fd_, VIDIOC_DQBUF, &buf) == -1) {
		if (errno != EAGAIN && errno != EINTR) std::perror("VIDIOC_DQBUF failed");
		return false;
	}

	uint32_t plane_stride[VIDEO_MAX_PLANES] = {0};
	uint32_t plane_rows[VIDEO_MAX_PLANES] = {0};
	for (uint32_t p = 0; p < num_planes_ && p < VIDEO_MAX_PLANES; ++p) {
		uint32_t stride = plane_stride_[p];
		if (!stride && planes[p].bytesused) {
			uint32_t denom = cfg_.height;
			if ((pix_mode_ == PixelMode::NV12 || pix_mode_ == PixelMode::NV21) && p == 1 && cfg_.height) {
				denom = cfg_.height / 2;
			}
			if (denom) {
				stride = static_cast<uint32_t>(planes[p].bytesused / denom);
			}
		}
		if (!stride) {
			stride = (pix_mode_ == PixelMode::UYVY) ? (cfg_.width * 2) : cfg_.width;
		}
		plane_stride[p] = stride;

		uint32_t rows_cap = cfg_.height;
		if ((pix_mode_ == PixelMode::NV12 || pix_mode_ == PixelMode::NV21) && p == 1) {
			rows_cap = cfg_.height / 2;
		}
		uint32_t rows = rows_cap;
		if (stride && planes[p].bytesused) {
			uint32_t by_stride = static_cast<uint32_t>(planes[p].bytesused / stride);
			rows = std::min<uint32_t>(rows_cap, by_stride);
		}
		if (stride && buffers_[buf.index].planes[p].length >= stride) {
			uint32_t max_rows_by_len = static_cast<uint32_t>(buffers_[buf.index].planes[p].length / stride);
			rows = std::min<uint32_t>(rows, max_rows_by_len);
		}
		if ((pix_mode_ == PixelMode::NV12 || pix_mode_ == PixelMode::NV21) && p == 1) {
			rows &= ~1U;
			if (plane_rows[0]) {
				rows = std::min<uint32_t>(rows, plane_rows[0] / 2);
			}
		}
		plane_rows[p] = rows;
	}

	bool media_ok = false;
	switch (pix_mode_) {
		case PixelMode::UYVY: {
			uint32_t stride = plane_stride[0] ? plane_stride[0] : cfg_.width * 2;
			uint32_t rows = plane_rows[0] ? plane_rows[0] : cfg_.height;
			media_ok = rows == cfg_.height &&
				fill_media_buffer_with_rga(write_index,
							   PixelMode::UYVY,
							   stride,
							   cfg_.height,
							   buffers_[buf.index].planes[0].fd);
			break;
		}
		case PixelMode::NV12: {
			uint32_t stride = plane_stride[0] ? plane_stride[0] : cfg_.width;
			uint32_t rows = plane_rows[0] ? plane_rows[0] : cfg_.height;
			media_ok = rows == cfg_.height &&
				fill_media_buffer_with_rga(write_index,
							   PixelMode::NV12,
							   stride,
							   cfg_.height,
							   buffers_[buf.index].planes[0].fd);
			break;
		}
		case PixelMode::NV21: {
			uint32_t stride = plane_stride[0] ? plane_stride[0] : cfg_.width;
			uint32_t rows = plane_rows[0] ? plane_rows[0] : cfg_.height;
			media_ok = rows == cfg_.height &&
				fill_media_buffer_with_rga(write_index,
							   PixelMode::NV21,
							   stride,
							   cfg_.height,
							   buffers_[buf.index].planes[0].fd);
			break;
		}
	}

	if (media_ok) {
		media_ok = media_to_rgb(write_index);
	}
	if (media_ok) {
		rga_state_.record_success();
	}

	if (xioctl(fd_, VIDIOC_QBUF, &buf) == -1) {
		std::perror("VIDIOC_QBUF failed");
		std::fflush(stderr);
		return false;
	}
	return media_ok;
}

void Camera::cleanup_buffers() {
	for (auto &buffer : media_) {
		if (buffer.start && buffer.length) {
			munmap(buffer.start, buffer.length);
			buffer.start = nullptr;
		}
		if (buffer.fd >= 0) {
			close(buffer.fd);
			buffer.fd = -1;
		}
		buffer.length = 0;
	}
	for (auto &buffer : rgb_) {
		if (buffer.start && buffer.length) {
			munmap(buffer.start, buffer.length);
			buffer.start = nullptr;
		}
		if (buffer.fd >= 0) {
			close(buffer.fd);
			buffer.fd = -1;
		}
		buffer.length = 0;
	}

	for (auto &b : buffers_) {
		for (auto &p : b.planes) {
			if (p.start && p.length) {
				munmap(p.start, p.length);
			}
			if (p.fd >= 0) {
				close(p.fd);
				p.fd = -1;
			}
		}
	}
	buffers_.clear();
	if (dma_heap_fd_ >= 0) {
		close(dma_heap_fd_);
		dma_heap_fd_ = -1;
	}
}

int Camera::xioctl(int fd, unsigned long request, void *arg) {
	int r;
	do {
		r = ioctl(fd, request, arg);
	} while (r == -1 && errno == EINTR);
	return r;
}

bool Camera::fill_media_buffer_with_rga(int write_index,
					PixelMode mode,
					uint32_t src_stride,
					uint32_t src_hstride,
					int src_fd) {
	if (write_index < 0 || write_index >= 2 || media_[write_index].fd < 0) return false;
	return rga_transform_to_nv12(mode,
				     cfg_.width,
				     cfg_.height,
				     src_stride,
				     src_hstride,
				     src_fd,
				     media_[write_index].fd,
				     rga_state_.rotate180(),
				     rga_state_);
}

bool Camera::media_to_rgb(int write_index) {
	if (write_index < 0 || write_index >= 2 ||
		media_[write_index].fd < 0 || rgb_[write_index].fd < 0) {
		return false;
	}
	return rga_convert_to_rgb(PixelMode::NV12,
				  cfg_.width,
				  cfg_.height,
				  cfg_.width,
				  cfg_.height,
				  media_[write_index].fd,
				  rgb_[write_index].fd,
				  rga_state_);
}
