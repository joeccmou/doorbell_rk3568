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
#if !defined(LVGL_CAMERA_DISABLE_RGA)
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
		const char *env = std::getenv("LVGL_CAMERA_RGA");
		if (env && (*env == '0' || *env == 'n' || *env == 'N')) return 0;
		return 1;
	}();
	return enabled != 0;
}

const char *select_dma_heap_path() {
	static const char *chosen = [] {
		// Allow override. Example: LVGL_CAMERA_DMA_HEAP=/dev/dma_heap/system-dma32
		if (const char *env = std::getenv("LVGL_CAMERA_DMA_HEAP")) return env;
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
#if defined(LVGL_CAMERA_DISABLE_RGA)
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
			src_wstride_px = src_stride_bytes / 2; // 2 bytes per pixel
			break;
		case Camera::PixelMode::NV12:
			rga_fmt = RK_FORMAT_YCbCr_420_SP;
			src_wstride_px = src_stride_bytes; // 1 byte per Y pixel
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

	// Force limited-range BT.601, matching most USB/MIPI camera outputs to avoid green/dark cast.
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
	// std::fprintf(stdout, "[rga] imcvtcolor success fmt=%d %ux%u\n", rga_fmt, width, height);
	return true;
#endif
}
}

Camera::Camera(const Config &cfg) : cfg_(cfg) {
	rgb_size_ = static_cast<size_t>(cfg_.width) * static_cast<size_t>(cfg_.height) * 3;
	rgb_[0].resize(rgb_size_);
	rgb_[1].resize(rgb_size_);
	raw_[0].resize(packed_raw_size(cfg_.pixelformat, cfg_.width, cfg_.height));
	raw_[1].resize(packed_raw_size(cfg_.pixelformat, cfg_.width, cfg_.height));
}

Camera::~Camera() {
	stop();
}

bool Camera::start() {
	if (running_.load()) return true;

	if (!open_device()) return false;
	if (!set_format()) return false;
	if (!request_buffers()) return false;
	if (!start_stream()) return false;
		

	std::fprintf(stdout, "[camera] started fmt=%c%c%c%c planes=%u %ux%u\n",
		    cfg_.pixelformat & 0xFF,
		    (cfg_.pixelformat >> 8) & 0xFF,
		    (cfg_.pixelformat >> 16) & 0xFF,
		    (cfg_.pixelformat >> 24) & 0xFF,
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
		if (idx < 0) return false;

		if (out_rgb.size() != rgb_size_) out_rgb.resize(rgb_size_);
		::memcpy(out_rgb.data(), rgb_[idx].data(), rgb_size_);

		if (out_raw.size() != raw_[idx].size()) out_raw.resize(raw_[idx].size());
		if (!raw_[idx].empty()) {
			::memcpy(out_raw.data(), raw_[idx].data(), raw_[idx].size());
		}

		int idx_after = latest_.load(std::memory_order_acquire);
		if (idx == idx_after) {
			seq = frame_seq_[idx].load(std::memory_order_acquire);
			ts_ns = frame_ts_ns_[idx].load(std::memory_order_acquire);
			pixfmt = cfg_.pixelformat;
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

size_t Camera::frame_size() const {
	return rgb_size_;
}

uint32_t Camera::width() const {
	return cfg_.width;
}

uint32_t Camera::height() const {
	return cfg_.height;
}

bool Camera::open_dma_heap() {
	if (dma_heap_fd_ >= 0) return true;
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
	size_t raw_size = packed_raw_size(cfg_.pixelformat, cfg_.width, cfg_.height);
	raw_[0].assign(raw_size, 0);
	raw_[1].assign(raw_size, 0);
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
		// Fallback to visible width if stride is still zero
		if (!stride) {
			stride = cfg_.width;          // NV12/NV21 或其他至少等于宽度
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
		// Also clamp rows by mapped length if available.
		if (stride && buffers_[buf.index].planes[p].length >= stride) {
			uint32_t max_rows_by_len = static_cast<uint32_t>(buffers_[buf.index].planes[p].length / stride);
			rows = std::min<uint32_t>(rows, max_rows_by_len);
		}
		// Ensure rows even for chroma so j/2 does not exceed plane data due to odd rows.
		if ((pix_mode_ == PixelMode::NV12 || pix_mode_ == PixelMode::NV21) && p == 1) {
			rows &= ~1U;
			// Also clamp chroma rows to luma rows/2 when luma is smaller than expected.
			if (plane_rows[0]) {
				rows = std::min<uint32_t>(rows, plane_rows[0] / 2);
			}
		}
		plane_rows[p] = rows;
	}

	uint32_t rows_written = cfg_.height;

	// Pack raw frame into tight layout for recording path.
	if (!raw_[write_index].empty()) {
		switch (pix_mode_) {
			case PixelMode::UYVY: {
				uint32_t stride = plane_stride[0] ? plane_stride[0] : cfg_.width * 2;
				uint32_t rows = plane_rows[0] ? plane_rows[0] : cfg_.height;
				uint8_t *dst = raw_[write_index].data();
				const uint8_t *src = plane_base[0];
				for (uint32_t row = 0; row < std::min(rows, cfg_.height); ++row) {
					::memcpy(dst + static_cast<size_t>(row) * cfg_.width * 2,
							 src + static_cast<size_t>(row) * stride,
							 static_cast<size_t>(cfg_.width) * 2);
				}
				break;
			}
			case PixelMode::NV12:
			case PixelMode::NV21: {
				uint8_t *dst = raw_[write_index].data();
				uint32_t y_stride = plane_stride[0] ? plane_stride[0] : cfg_.width;
				uint32_t c_stride = plane_stride[1] ? plane_stride[1] : cfg_.width;
				uint32_t y_rows = plane_rows[0] ? plane_rows[0] : cfg_.height;
				uint32_t c_rows = plane_rows[1] ? plane_rows[1] : (cfg_.height / 2);
				for (uint32_t row = 0; row < std::min(y_rows, cfg_.height); ++row) {
					::memcpy(dst + static_cast<size_t>(row) * cfg_.width,
							 plane_base[0] + static_cast<size_t>(row) * y_stride,
							 cfg_.width);
				}
				uint8_t *dst_c = dst + static_cast<size_t>(cfg_.width) * cfg_.height;
				const uint8_t *src_c = (num_planes_ > 1 && plane_base[1])
					? plane_base[1]
					: (plane_base[0] + static_cast<size_t>(y_stride) * cfg_.height);
				for (uint32_t row = 0; row < std::min(c_rows, cfg_.height / 2); ++row) {
					::memcpy(dst_c + static_cast<size_t>(row) * cfg_.width,
							 src_c + static_cast<size_t>(row) * c_stride,
							 cfg_.width);
				}
				break;
			}			
		}
	}

	switch (pix_mode_) {
		case PixelMode::UYVY: {
			const uint8_t *base = plane_base[0];
			uint32_t stride = plane_stride[0] ? plane_stride[0] : cfg_.width * 2;
			uint32_t rows = plane_rows[0] ? plane_rows[0] : cfg_.height;
			uint32_t hstride = rows ? rows : cfg_.height;
			if (try_rga_convert(pix_mode_, cfg_.width, cfg_.height, stride, hstride,
					 buffers_[buf.index].planes[0].fd, base, rgb_[write_index].data())) {
				rows_written = std::min(cfg_.height, hstride);
				break;
			} else {
				std::fprintf(stderr, "[rga] UYVY -> RGB fallback to CPU\n");
			}
			rows_written = std::min(rows, cfg_.height);
			for (uint32_t row = 0; row < rows_written; ++row) {
				const uint8_t *src = base + row * stride;
				uint8_t *dst = rgb_[write_index].data() + row * cfg_.width * 3;
				for (uint32_t col = 0; col + 1 < cfg_.width; col += 2) {
					int u = src[0] - 128;
					int y0 = src[1];
					int v = src[2] - 128;
					int y1 = src[3];
					src += 4;
					yuv_to_rgb_pair(y0, y1, u, v, dst + col * 3);
				}
			}
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
			// If the allocator padded height (len0 encodes aligned height), prefer the padded UV start to avoid
			// landing inside the tail of the Y plane. This helps when bytesused reports active height but the
			// buffer length includes alignment (e.g., height rounded to 16).
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
				// Single-plane: start UV at the padded Y height when available; otherwise fall back to bytesused.
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
			uint32_t rows_uv_len = 0;
			if (uv_stride) {
				rows_uv_len = static_cast<uint32_t>(len_uv_avail / uv_stride);
				rows_uv = std::min<uint32_t>(rows_uv, rows_uv_len);
			}
			rows_uv = std::min<uint32_t>(rows_uv, rows_y / 2);
			rows_written = std::min<uint32_t>({cfg_.height, rows_y, rows_uv * 2});
			uint32_t y_hstride = y_stride ? static_cast<uint32_t>(uv_offset / y_stride) : rows_y;
			if (!y_hstride) y_hstride = rows_y;
			if (buffers_[buf.index].planes.size() == 1 &&
				try_rga_convert(pix_mode_, cfg_.width, cfg_.height, y_stride, y_hstride,
					 buffers_[buf.index].planes[0].fd, y_base, rgb_[write_index].data())) {
				rows_written = std::min(cfg_.height, y_hstride);
				break;
			}
			for (uint32_t j = 0; j < rows_written; ++j) {
				size_t y_off = static_cast<size_t>(j) * y_stride;
				if (y_off + cfg_.width > used0) break;
				const uint8_t *y_row = y_base + y_off;
				uint32_t uv_row_idx = std::min<uint32_t>(j / 2, rows_uv ? rows_uv - 1 : j / 2);
				size_t uv_off = static_cast<size_t>(uv_row_idx) * uv_stride;
				if (uv_off + cfg_.width > len_uv_avail) break;
				const uint8_t *uv_row = uv_base + uv_off;
				uint8_t *out = rgb_[write_index].data() + j * cfg_.width * 3;
				for (uint32_t i = 0; i < cfg_.width; i += 2) {
					int Y0 = y_row[i];
					int Y1 = y_row[i + 1];
					int U = uv_row[i & ~1] - 128;
					int V = uv_row[(i & ~1) + 1] - 128;
					yuv_to_rgb_pair(Y0, Y1, U, V, out + i * 3);
				}
			}
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
				// Single-plane: prefer padded Y height derived from buffer length to locate chroma start.
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
				uint32_t rows_vu_len = static_cast<uint32_t>(len_vu_avail / vu_stride);
				rows_vu = std::min<uint32_t>(rows_vu, rows_vu_len);
			}
			rows_vu = std::min<uint32_t>(rows_vu, rows_y / 2);
			rows_written = std::min<uint32_t>({cfg_.height, rows_y, rows_vu * 2});
			uint32_t y_hstride = y_stride ? static_cast<uint32_t>(vu_offset / y_stride) : rows_y;
			if (!y_hstride) y_hstride = rows_y;
			if (buffers_[buf.index].planes.size() == 1 &&
				try_rga_convert(pix_mode_, cfg_.width, cfg_.height, y_stride, y_hstride,
					 buffers_[buf.index].planes[0].fd, y_base, rgb_[write_index].data())) {
				rows_written = std::min(cfg_.height, y_hstride);
				break;
			}
			for (uint32_t j = 0; j < rows_written; ++j) {
				size_t y_off = static_cast<size_t>(j) * y_stride;
				if (y_off + cfg_.width > used0) break;
				const uint8_t *y_row = y_base + y_off;
				uint32_t vu_row_idx = std::min<uint32_t>(j / 2, rows_vu ? rows_vu - 1 : j / 2);
				size_t vu_off = static_cast<size_t>(vu_row_idx) * vu_stride;
				if (vu_off + cfg_.width > len_vu_avail) break;
				const uint8_t *vu_row = vu_base + vu_off;
				uint8_t *out = rgb_[write_index].data() + j * cfg_.width * 3;
				for (uint32_t i = 0; i < cfg_.width; i += 2) {
					int Y0 = y_row[i];
					int Y1 = y_row[i + 1];
					int V = vu_row[i & ~1] - 128;
					int U = vu_row[(i & ~1) + 1] - 128;
					yuv_to_rgb_pair(Y0, Y1, U, V, out + i * 3);
				}
			}
			break;
		}
		
	}

	// For NV12/NV21, defensively clear a few bottom rows to mask any tail padding artifacts.
	if ((pix_mode_ == PixelMode::NV12 || pix_mode_ == PixelMode::NV21) && rows_written > 0) {
		uint8_t *dst = rgb_[write_index].data();
		uint32_t rows_to_clear = std::min<uint32_t>(rows_written, 16);
		size_t off = static_cast<size_t>(rows_written - rows_to_clear) * cfg_.width * 3;
		size_t bytes = static_cast<size_t>(rows_to_clear) * cfg_.width * 3;
		memset(dst + off, 0, bytes);
	}

	// Emit partial-frame info after conversion.
	// Clear any trailing rows not written (e.g., if bytesused reported fewer rows) to avoid flicker/green line.
	if (rows_written < cfg_.height) {
		uint8_t *dst = rgb_[write_index].data();
		size_t remain = static_cast<size_t>(cfg_.height - rows_written) * cfg_.width * 3;
		memset(dst + rows_written * cfg_.width * 3, 0, remain);
	}

	if (xioctl(fd_, VIDIOC_QBUF, &buf) == -1) {
		std::perror("VIDIOC_QBUF failed");
		std::fflush(stderr);
		return false;
	}
	return true;
}

void Camera::cleanup_buffers() {
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
	// Convert limited-range YUV to RGB with BT.601 coefficients (full-range tends暗/偏色).
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
		const uint8_t *uv_row = uv + (j / 2) * width; // UV is interleaved, width bytes per row
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
