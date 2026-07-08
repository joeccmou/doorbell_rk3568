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

int clamp8(int v) {
	if (v < 0) return 0;
	if (v > 255) return 255;
	return v;
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

bool try_rga_convert(Camera::PixelMode mode,
			uint32_t width,
			uint32_t height,
			uint32_t src_stride_bytes,
			uint32_t src_hstride,
			int src_fd,
			const uint8_t *src_va,
			uint8_t *dst_va) {
#if defined(DOORBELL_DISABLE_RGA)
	(void)mode;
	(void)width;
	(void)height;
	(void)src_stride_bytes;
	(void)src_hstride;
	(void)src_fd;
	(void)src_va;
	(void)dst_va;
	return false;
#else
	static bool rga_suppressed = false;
	if (!rga_enabled() || !dst_va || rga_suppressed) 
	{
		std::fprintf(stdout, "[rga] RGA disabled or not usable, fallback to CPU, rga_enabled()=%d, dst_va=%p, rga_suppressed=%d\n",
			rga_enabled(), dst_va, rga_suppressed);
		return false;
	}

	int rga_fmt = 0;
	uint32_t src_wstride_px = 0;
	switch (mode) {
		case Camera::PixelMode::UYVY:
			rga_fmt = RK_FORMAT_UYVY_422;
			src_wstride_px = src_stride_bytes / 2;
			break;
		case Camera::PixelMode::NV12:
			rga_fmt = RK_FORMAT_YCbCr_420_SP;
			src_wstride_px = src_stride_bytes;
			break;
		case Camera::PixelMode::NV21:
			rga_fmt = RK_FORMAT_YCrCb_420_SP;
			src_wstride_px = src_stride_bytes;
			break;
		default:
			return false;
	}

	if (!src_wstride_px || !src_hstride) return false;

	rga_buffer_t src_buf;
	if (src_fd >= 0) {
		src_buf = wrapbuffer_fd(src_fd, width, height, rga_fmt,
				 static_cast<int>(src_wstride_px), static_cast<int>(src_hstride));
	} else {
		src_buf = wrapbuffer_virtualaddr(const_cast<uint8_t *>(src_va), width, height, rga_fmt,
				 static_cast<int>(src_wstride_px), static_cast<int>(src_hstride));
	}
	rga_buffer_t dst_buf = wrapbuffer_virtualaddr(dst_va, width, height, RK_FORMAT_RGB_888,
				static_cast<int>(width), static_cast<int>(height));

	IM_STATUS st = imcvtcolor(src_buf, dst_buf, rga_fmt, RK_FORMAT_RGB_888, IM_YUV_TO_RGB_BT709_LIMIT, 1, nullptr);
	static bool logged = false;
	if (st != IM_STATUS_SUCCESS) {
		if (!logged) {
			std::fprintf(stderr, "[rga] imcvtcolor failed status=%d (fmt=%d), fallback to CPU\n", st, rga_fmt);
			logged = true;
		}
		rga_suppressed = true;
		return false;
	}
	return true;
#endif
}

bool try_rga_convert_to_nv12(Camera::PixelMode mode,
				 uint32_t width,
				 uint32_t height,
				 uint32_t src_stride_bytes,
				 uint32_t src_hstride,
				 int src_fd,
				 const uint8_t *src_va,
				 int dst_fd,
				 uint8_t *dst_va) {
#if defined(DOORBELL_DISABLE_RGA)
	(void)mode;
	(void)width;
	(void)height;
	(void)src_stride_bytes;
	(void)src_hstride;
	(void)src_fd;
	(void)src_va;
	(void)dst_fd;
	(void)dst_va;
	return false;
#else
	if (!rga_enabled() || (dst_fd < 0 && !dst_va)) {
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
		default:
			return false;
	}
	if (!src_wstride_px || !src_hstride) return false;

	rga_buffer_t src_buf;
	if (src_fd >= 0) {
		src_buf = wrapbuffer_fd(src_fd, width, height, src_fmt,
				 static_cast<int>(src_wstride_px), static_cast<int>(src_hstride));
	} else {
		src_buf = wrapbuffer_virtualaddr(const_cast<uint8_t *>(src_va), width, height, src_fmt,
				 static_cast<int>(src_wstride_px), static_cast<int>(src_hstride));
	}

	rga_buffer_t dst_buf;
	if (dst_fd >= 0) {
		dst_buf = wrapbuffer_fd(dst_fd, width, height, RK_FORMAT_YCbCr_420_SP,
				 static_cast<int>(width), static_cast<int>(height));
	} else {
		dst_buf = wrapbuffer_virtualaddr(dst_va, width, height, RK_FORMAT_YCbCr_420_SP,
				 static_cast<int>(width), static_cast<int>(height));
	}

	im_rect srect = {0, 0, static_cast<int>(width), static_cast<int>(height)};
	im_rect drect = {0, 0, static_cast<int>(width), static_cast<int>(height)};
	im_rect prect;
	memset(&prect, 0, sizeof(prect));
	rga_buffer_t pat;
	memset(&pat, 0, sizeof(pat));

	IM_STATUS st = improcess(src_buf, dst_buf, pat, srect, drect, prect, 0);
	if (st != IM_STATUS_SUCCESS) {
		static bool logged = false;
		if (!logged) {
			std::fprintf(stderr, "[rga] improcess to NV12 failed status=%d, fallback to CPU\n", st);
			logged = true;
		}
		return false;
	}
	return true;
#endif
}
}

Camera::Camera(const Config &cfg) : cfg_(cfg) {
	rgb_size_ = static_cast<size_t>(cfg_.width) * static_cast<size_t>(cfg_.height) * 3;
	media_size_ = packed_raw_size(V4L2_PIX_FMT_NV12, cfg_.width, cfg_.height);
	rgb_[0].resize(rgb_size_);
	rgb_[1].resize(rgb_size_);
}

Camera::~Camera() {
	stop();
}

bool Camera::start() {
	if (running_.load()) return true;

	if (!open_device()) return false;
	if (!set_format()) return false;
	if (!alloc_media_buffers()) return false;
	if (!request_buffers()) return false;
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

const uint8_t *Camera::frame_data() const {
	int idx = latest_.load(std::memory_order_acquire);
	if (idx < 0) return nullptr;
	return rgb_[idx].data();
}

bool Camera::copy_latest_frame(std::vector<uint8_t> &out_rgb, uint64_t &seq, uint64_t &ts_ns) const {
	if (out_rgb.size() != rgb_size_) {
		out_rgb.resize(rgb_size_);
	}

	for (int attempt = 0; attempt < 3; ++attempt) {
		int idx = latest_.load(std::memory_order_acquire);
		if (idx < 0) return false;

		::memcpy(out_rgb.data(), rgb_[idx].data(), rgb_size_);

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
		if (idx < 0 || !media_[idx].start) return false;

		if (out_rgb.size() != rgb_size_) out_rgb.resize(rgb_size_);
		::memcpy(out_rgb.data(), rgb_[idx].data(), rgb_size_);

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

bool Camera::alloc_media_buffers() {
	if (media_size_ == 0) {
		return false;
	}

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
		buffer.stride_y = cfg_.width;
		buffer.stride_uv = cfg_.width;

		buffer.fd = alloc_dma_buf(media_size_);
		if (buffer.fd < 0) {
			return false;
		}
		buffer.length = media_size_;
		buffer.start = mmap(NULL, buffer.length, PROT_READ | PROT_WRITE, MAP_SHARED, buffer.fd, 0);
		if (buffer.start == MAP_FAILED) {
			std::perror("mmap media dma-buf failed");
			close(buffer.fd);
			buffer.fd = -1;
			buffer.start = nullptr;
			return false;
		}
		memset(buffer.start, 0, buffer.length);
	}
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
	rgb_[0].assign(rgb_size_, 0);
	rgb_[1].assign(rgb_size_, 0);
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
		default: pix_mode_ = PixelMode::UYVY; break;
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

	uint8_t *plane_base[VIDEO_MAX_PLANES] = {nullptr};
	uint32_t plane_stride[VIDEO_MAX_PLANES] = {0};
	uint32_t plane_rows[VIDEO_MAX_PLANES] = {0};
	for (uint32_t p = 0; p < num_planes_ && p < VIDEO_MAX_PLANES; ++p) {
		plane_base[p] = static_cast<uint8_t *>(buffers_[buf.index].planes[p].start) + planes[p].data_offset;
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
			const uint8_t *base = plane_base[0];
			uint32_t stride = plane_stride[0] ? plane_stride[0] : cfg_.width * 2;
			uint32_t rows = plane_rows[0] ? plane_rows[0] : cfg_.height;
			media_ok = fill_media_buffer_from_uyvy(write_index,
			                                      base,
			                                      stride,
			                                      rows,
			                                      buffers_[buf.index].planes[0].fd);
			break;
		}
		case PixelMode::NV12: {
			const uint8_t *y_base = plane_base[0];
			const uint8_t *uv_base = nullptr;
			uint32_t y_stride = plane_stride[0] ? plane_stride[0] : cfg_.width;
			uint32_t uv_stride = 0;
			size_t len0 = buffers_[buf.index].planes[0].length;
			size_t used0 = planes[0].bytesused ? planes[0].bytesused : len0;
			size_t len_uv_avail = 0;
			size_t uv_offset = static_cast<size_t>(y_stride) * cfg_.height;
			size_t uv_span = static_cast<size_t>(y_stride) * (cfg_.height / 2);
			size_t padded_h = 0;
			if (y_stride) {
				padded_h = static_cast<size_t>((len0 * 2) / (3 * y_stride));
			}
			size_t uv_offset_padded = padded_h ? padded_h * y_stride : uv_offset;
			if (buffers_[buf.index].planes.size() > 1) {
				uv_base = plane_base[1];
				uv_stride = plane_stride[1] ? plane_stride[1] : cfg_.width;
				len_uv_avail = planes[1].bytesused ? planes[1].bytesused : buffers_[buf.index].planes[1].length;
			} else {
				if (uv_offset_padded >= uv_offset && uv_offset_padded + uv_span <= len0) {
					uv_offset = uv_offset_padded;
				} else if (used0 > uv_span) {
					uv_offset = used0 - uv_span;
				}
				if (uv_offset > len0) uv_offset = len0;
				uv_base = y_base + uv_offset;
				uv_stride = y_stride;
				len_uv_avail = (uv_offset < len0) ? (len0 - uv_offset) : 0;
			}
			uint32_t rows_y = plane_rows[0] ? plane_rows[0] : std::min<uint32_t>(cfg_.height, static_cast<uint32_t>(used0 / y_stride));
			uint32_t rows_uv = plane_rows[1] ? plane_rows[1] : (cfg_.height / 2);
			if (uv_stride) {
				rows_uv = std::min<uint32_t>(rows_uv, static_cast<uint32_t>(len_uv_avail / uv_stride));
			}
			rows_uv = std::min<uint32_t>(rows_uv, rows_y / 2);
			media_ok = fill_media_buffer_from_nv12(write_index, y_base, uv_base, y_stride, uv_stride, rows_y, rows_uv);
			break;
		}
		case PixelMode::NV21: {
			const uint8_t *y_base = plane_base[0];
			const uint8_t *vu_base = nullptr;
			uint32_t y_stride = plane_stride[0] ? plane_stride[0] : cfg_.width;
			uint32_t vu_stride = 0;
			size_t len0 = buffers_[buf.index].planes[0].length;
			size_t used0 = planes[0].bytesused ? planes[0].bytesused : len0;
			size_t len_vu_avail = 0;
			size_t vu_offset = static_cast<size_t>(y_stride) * cfg_.height;
			size_t vu_span = static_cast<size_t>(y_stride) * (cfg_.height / 2);
			size_t padded_h = 0;
			if (y_stride) {
				padded_h = static_cast<size_t>((len0 * 2) / (3 * y_stride));
			}
			size_t vu_offset_padded = padded_h ? padded_h * y_stride : vu_offset;
			if (buffers_[buf.index].planes.size() > 1) {
				vu_base = plane_base[1];
				vu_stride = plane_stride[1] ? plane_stride[1] : cfg_.width;
				len_vu_avail = planes[1].bytesused ? planes[1].bytesused : buffers_[buf.index].planes[1].length;
			} else {
				if (vu_offset_padded >= vu_offset && vu_offset_padded + vu_span <= len0) {
					vu_offset = vu_offset_padded;
				} else if (used0 > vu_span) {
					vu_offset = used0 - vu_span;
				}
				if (vu_offset > len0) vu_offset = len0;
				vu_base = y_base + vu_offset;
				vu_stride = y_stride;
				len_vu_avail = (vu_offset < len0) ? (len0 - vu_offset) : 0;
			}
			uint32_t rows_y = plane_rows[0] ? plane_rows[0] : std::min<uint32_t>(cfg_.height, static_cast<uint32_t>(used0 / y_stride));
			uint32_t rows_vu = plane_rows[1] ? plane_rows[1] : (cfg_.height / 2);
			if (vu_stride) {
				rows_vu = std::min<uint32_t>(rows_vu, static_cast<uint32_t>(len_vu_avail / vu_stride));
			}
			rows_vu = std::min<uint32_t>(rows_vu, rows_y / 2);
			media_ok = fill_media_buffer_from_nv21(write_index, y_base, vu_base, y_stride, vu_stride, rows_y, rows_vu);
			break;
		}
	}

	if (media_ok) {
		media_ok = media_to_rgb(write_index);
	}
	if (!media_ok) {
		memset(rgb_[write_index].data(), 0, rgb_size_);
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

// Fixed-point fast YUV->RGB (full-range). Coeffs are Q14 approximations of the float path.
inline void Camera::yuv_to_rgb_pair(int y0, int y1, int u, int v, uint8_t *dst) {
	// Convert limited-range YUV to RGB with BT.601 coefficients (full-range tends閺?閸嬪繗澹?.
	// Y in [16,235], U/V in [16,240]; clamp after applying offsets.
	y0 = std::max(16, std::min(235, y0));
	y1 = std::max(16, std::min(235, y1));

	int r_add = (v * 22970) >> 14;              // 1.402 * 16384
	int g_add = (-v * 11698 - u * 5638) >> 14;  // -0.713 * V -0.344 * U
	int b_add = (u * 29032) >> 14;              // 1.772 * U

	int r0 = clamp8((y0 - 16) + r_add);
	int g0 = clamp8((y0 - 16) + g_add);
	int b0 = clamp8((y0 - 16) + b_add);

	int r1 = clamp8((y1 - 16) + r_add);
	int g1 = clamp8((y1 - 16) + g_add);
	int b1 = clamp8((y1 - 16) + b_add);

	dst[0] = static_cast<uint8_t>(r0);
	dst[1] = static_cast<uint8_t>(g0);
	dst[2] = static_cast<uint8_t>(b0);
	dst[3] = static_cast<uint8_t>(r1);
	dst[4] = static_cast<uint8_t>(g1);
	dst[5] = static_cast<uint8_t>(b1);
}

void Camera::yuyv_to_rgb888(const uint8_t *src, uint8_t *dst, uint32_t pixel_count) {
	for (uint32_t i = 0; i + 1 < pixel_count; i += 2) {
		int y0 = src[0];
		int u = src[1] - 128;
		int y1 = src[2];
		int v = src[3] - 128;
		src += 4;
		yuv_to_rgb_pair(y0, y1, u, v, dst);
		dst += 6;
	}
}

void Camera::nv12_to_rgb888(const uint8_t *y, const uint8_t *uv, uint8_t *dst,
					uint32_t width, uint32_t height) {
	for (uint32_t j = 0; j < height; ++j) {
		const uint8_t *y_row = y + j * width;
		const uint8_t *uv_row = uv + (j / 2) * width;
		uint8_t *out = dst + j * width * 3;
		for (uint32_t i = 0; i < width; i += 2) {
			int Y0 = y_row[i];
			int Y1 = y_row[i + 1];
			int U = uv_row[i & ~1] - 128;
			int V = uv_row[(i & ~1) + 1] - 128;
			yuv_to_rgb_pair(Y0, Y1, U, V, out + i * 3);
		}
	}
}

void Camera::uyvy_to_nv12(const uint8_t *src,
				 uint32_t src_stride,
				 uint8_t *dst_y,
				 uint8_t *dst_uv,
				 uint32_t width,
				 uint32_t height) {
	for (uint32_t row = 0; row < height; ++row) {
		const uint8_t *src_row = src + static_cast<size_t>(row) * src_stride;
		uint8_t *dst_row = dst_y + static_cast<size_t>(row) * width;
		for (uint32_t col = 0; col + 1 < width; col += 2) {
			dst_row[col] = src_row[1];
			dst_row[col + 1] = src_row[3];
			src_row += 4;
		}
	}

	for (uint32_t row = 0; row < height; row += 2) {
		const uint8_t *src_row0 = src + static_cast<size_t>(row) * src_stride;
		const uint8_t *src_row1 = src + static_cast<size_t>(std::min<uint32_t>(row + 1, height - 1)) * src_stride;
		uint8_t *dst_row = dst_uv + static_cast<size_t>(row / 2) * width;
		for (uint32_t col = 0; col + 1 < width; col += 2) {
			const size_t off = static_cast<size_t>(col) * 2;
			dst_row[col] = static_cast<uint8_t>((static_cast<uint32_t>(src_row0[off + 0]) + static_cast<uint32_t>(src_row1[off + 0]) + 1U) / 2U);
			dst_row[col + 1] = static_cast<uint8_t>((static_cast<uint32_t>(src_row0[off + 2]) + static_cast<uint32_t>(src_row1[off + 2]) + 1U) / 2U);
		}
	}
}

void Camera::nv21_to_nv12(uint8_t *dst_uv, const uint8_t *src_vu, uint32_t width, uint32_t chroma_rows) {
	for (uint32_t row = 0; row < chroma_rows; ++row) {
		const uint8_t *src_row = src_vu + static_cast<size_t>(row) * width;
		uint8_t *dst_row = dst_uv + static_cast<size_t>(row) * width;
		for (uint32_t col = 0; col + 1 < width; col += 2) {
			dst_row[col] = src_row[col + 1];
			dst_row[col + 1] = src_row[col];
		}
	}
}

bool Camera::fill_media_buffer_from_uyvy(int write_index,
					     const uint8_t *src,
					     uint32_t src_stride,
					     uint32_t src_rows,
					     int src_fd) {
	if (!media_[write_index].start) return false;
	uint8_t *dst = static_cast<uint8_t *>(media_[write_index].start);
	memset(dst, 0, media_size_);
	const uint32_t rows = std::min(src_rows, cfg_.height);
	if (rows == cfg_.height &&
		try_rga_convert_to_nv12(PixelMode::UYVY,
				       cfg_.width,
				       cfg_.height,
				       src_stride,
				       rows,
				       src_fd,
				       src,
				       media_[write_index].fd,
				       dst)) {
		return true;
	}
	uyvy_to_nv12(src, src_stride, dst, dst + static_cast<size_t>(cfg_.width) * cfg_.height, cfg_.width, rows);
	return rows == cfg_.height;
}

bool Camera::fill_media_buffer_from_nv12(int write_index,
					     const uint8_t *y_base,
					     const uint8_t *uv_base,
					     uint32_t y_stride,
					     uint32_t uv_stride,
					     uint32_t rows_y,
					     uint32_t rows_uv) {
	if (!media_[write_index].start || !y_base || !uv_base) return false;
	uint8_t *dst = static_cast<uint8_t *>(media_[write_index].start);
	uint8_t *dst_uv = dst + static_cast<size_t>(cfg_.width) * cfg_.height;
	memset(dst, 0, media_size_);
	rows_y = std::min(rows_y, cfg_.height);
	rows_uv = std::min(rows_uv, cfg_.height / 2);
	for (uint32_t row = 0; row < rows_y; ++row) {
		::memcpy(dst + static_cast<size_t>(row) * cfg_.width,
				 y_base + static_cast<size_t>(row) * y_stride,
				 cfg_.width);
	}
	for (uint32_t row = 0; row < rows_uv; ++row) {
		::memcpy(dst_uv + static_cast<size_t>(row) * cfg_.width,
				 uv_base + static_cast<size_t>(row) * uv_stride,
				 cfg_.width);
	}
	return rows_y == cfg_.height && rows_uv == (cfg_.height / 2);
}

bool Camera::fill_media_buffer_from_nv21(int write_index,
					     const uint8_t *y_base,
					     const uint8_t *vu_base,
					     uint32_t y_stride,
					     uint32_t vu_stride,
					     uint32_t rows_y,
					     uint32_t rows_vu) {
	if (!media_[write_index].start || !y_base || !vu_base) return false;
	uint8_t *dst = static_cast<uint8_t *>(media_[write_index].start);
	uint8_t *dst_uv = dst + static_cast<size_t>(cfg_.width) * cfg_.height;
	memset(dst, 0, media_size_);
	rows_y = std::min(rows_y, cfg_.height);
	rows_vu = std::min(rows_vu, cfg_.height / 2);
	for (uint32_t row = 0; row < rows_y; ++row) {
		::memcpy(dst + static_cast<size_t>(row) * cfg_.width,
				 y_base + static_cast<size_t>(row) * y_stride,
				 cfg_.width);
	}
	for (uint32_t row = 0; row < rows_vu; ++row) {
		nv21_to_nv12(dst_uv + static_cast<size_t>(row) * cfg_.width,
				     vu_base + static_cast<size_t>(row) * vu_stride,
				     cfg_.width,
				     1);
	}
	return rows_y == cfg_.height && rows_vu == (cfg_.height / 2);
}

bool Camera::media_to_rgb(int write_index) {
	if (!media_[write_index].start) return false;
	const uint8_t *y = static_cast<const uint8_t *>(media_[write_index].start);
	const uint8_t *uv = y + static_cast<size_t>(cfg_.width) * cfg_.height;
	if (try_rga_convert(PixelMode::NV12,
				    cfg_.width,
				    cfg_.height,
				    cfg_.width,
				    cfg_.height,
				    media_[write_index].fd,
				    y,
				    rgb_[write_index].data())) {
		return true;
	}
	nv12_to_rgb888(y, uv, rgb_[write_index].data(), cfg_.width, cfg_.height);
	return true;
}
