#include <signal.h>
#include <unistd.h>

#include <atomic>
#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstdarg>
#include <limits.h>
#include <string>
#include <cstring>
#include <filesystem>
#include <functional>
#include <mutex>
#include <memory>
#include <optional>
#include <thread>
#include <vector>
#include <cassert>

#include <lvgl.h>
#include <linux/videodev2.h>
#include <SDL2/SDL.h>

#include "src/drivers/sdl/lv_sdl_window.h"
#include "src/drivers/sdl/lv_sdl_mouse.h"

#include "camera.h"
#include "device/provisioning.h"
#include "device/person_detection_settings.h"
#include "events/device_event_processor.h"
#include "events/event_paths.h"
#include "events/person_event_gate.h"
#include "events/person_recording_policy.h"
#include "events/snapshot_candidate_selector.h"
#include "ui/rga_display_pipeline.h"
#include "ui/settings.h"
#include "ai/yolo_person_detector.h"
#include "utils/mp4_recorder.h"
#include "utils/audio_capture_manager.h"
#include "utils/doorbell_chime_player.h"
#include "utils/perf_logger.h"
#include "utils/image_utils.h"
#include "utils/image_drawing.h"

namespace {
std::atomic<bool> g_stop{false};
constexpr uint32_t kDetectIntervalMs = 250;
constexpr uint32_t kRecordFpsLimit = 24;
constexpr uint64_t kRecordFrameIntervalNs = 1000000000ULL / kRecordFpsLimit;
constexpr uint64_t kOverlayFreshNs = 300ULL * 1000ULL * 1000ULL;
constexpr uint32_t kRingSnapshotSampleIntervalMs = 100;
constexpr uint32_t kMasterCaptureWidth = 2560;
constexpr uint32_t kMasterCaptureHeight = 1440;
constexpr uint32_t kMasterCaptureFps = 24;
constexpr uint32_t kDisplayWidth = 1280;
constexpr uint32_t kDisplayHeight = 800;

struct RecordingProfile {
    uint32_t width;
    uint32_t height;
    uint32_t bitrate;
};

RecordingProfile recording_profile_for(const std::string &quality) {
    if (quality == "360p") return {640, 360, 800000};
    if (quality == "1080p") return {1920, 1080, 4000000};
    if (quality == "1440p") return {2560, 1440, 6000000};
    return {1280, 720, 2000000};
}

void perf_log(const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    perf_logger_vlog(fmt, args);
    va_end(args);
}

bool init_perf_log_file(const std::string &log_path) {
    return perf_logger_init(log_path);
}

void close_perf_log_file() {
    perf_logger_close();
}

struct FramePacket {
    std::vector<uint8_t> rgb;
    std::vector<uint8_t> raw;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t pixfmt = 0;
    uint64_t seq = 0;
    uint64_t ts_ns = 0;
};

struct UiContext {
    Camera *camera = nullptr;
    std::mutex camera_mtx;
    Camera::Config cfg{};
    lv_image_dsc_t image_dsc{};
    lv_obj_t *image_obj = nullptr;

    std::vector<uint8_t> blank;
    lv_obj_t *fps_label = nullptr;
    uint64_t frame_counter_last = 0;
    uint32_t fps_last_ms = 0;

    lv_display_t *disp = nullptr;
    YoloPersonDetector detector;
    bool detector_ready = false;
    std::atomic<bool> person_detection_enabled{true};
    std::atomic<float> person_confidence_threshold{0.5F};
    uint64_t last_detect_ms = 0;
    uint64_t last_person_ts_ns = 0;
    uint64_t latest_boxes_ts_ns = 0;
    std::vector<YoloPersonDetector::PersonBox> latest_boxes;
    std::mutex detect_mtx;

    FramePacket infer_input;
    bool infer_has_input = false;
    bool infer_exit = false;
    std::mutex infer_mtx;
    std::condition_variable infer_cv;
    std::thread infer_thread;
    PersonEventGate person_event_gate{std::chrono::seconds(30)};
    SnapshotCandidateSelector snapshot_candidate_selector{std::chrono::seconds(2)};
    std::optional<EventRecord> pending_person_event;
    std::mutex ring_snapshot_mtx;
    SnapshotCandidateSelector ring_snapshot_candidate_selector{
        std::chrono::seconds(1), true};
    std::optional<EventRecord> pending_ring_snapshot_event;
    uint32_t ring_snapshot_last_sample_ms = 0;
    std::unique_ptr<DeviceEventProcessor> event_processor;

    bool record_exit = false;
    std::mutex record_mtx;
    std::condition_variable record_cv;
    uint64_t record_frame_gen = 0;
    std::thread record_thread;

    uint32_t perf_last_log_ms = 0;

    std::vector<uint8_t> display_fallback;
    std::unique_ptr<RgaDisplayPipeline> display_pipeline;
    Mp4Recorder recorder;
    AudioCaptureManager audio_capture;
    std::unique_ptr<DoorbellChimePlayer> doorbell_chime;
    std::string record_dir;
    std::string current_recording_id;
    std::string last_recording_attempt_id;
    std::string pending_ring_recording_id;
    std::chrono::system_clock::time_point pending_ring_recording_started_at{};
    std::string active_ring_event_id;
    bool ring_event_open = false;
    bool ring_press_in_progress = false;
    bool recording_stop_in_progress = false;
    std::chrono::system_clock::time_point current_recording_last_segment_ended_at{};
    std::function<std::string()> recording_quality_provider;
    // SDL_Window *sdl_window = nullptr;
};

void notify_record_new_frame(UiContext *ctx) {
    if (!ctx) return;
    {
        std::lock_guard<std::mutex> lock(ctx->record_mtx);
        ++ctx->record_frame_gen;
    }
    ctx->record_cv.notify_one();
}

uint64_t monotonic_time_ns() {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}

bool detection_is_fresh(uint64_t frame_ts_ns, uint64_t det_ts_ns) {
    if (frame_ts_ns == 0 || det_ts_ns == 0) return false;
    if (frame_ts_ns < det_ts_ns) {
        return (det_ts_ns - frame_ts_ns) <= kOverlayFreshNs;
    }
    return (frame_ts_ns - det_ts_ns) <= kOverlayFreshNs;
}

bool ring_recording_active(bool ring_event_open) {
    return ring_event_open;
}

void inference_worker(UiContext *ctx) {
    while (!g_stop.load()) {
        FramePacket frame;
        {
            std::unique_lock<std::mutex> lock(ctx->infer_mtx);
            ctx->infer_cv.wait(lock, [ctx] {
                return ctx->infer_exit || ctx->infer_has_input || g_stop.load();
            });
            if (ctx->infer_exit || g_stop.load()) break;
            frame = std::move(ctx->infer_input);
            ctx->infer_has_input = false;
        }

        if (!ctx->detector_ready || frame.rgb.empty() || frame.width == 0 || frame.height == 0) {
            continue;
        }

        std::vector<YoloPersonDetector::PersonBox> boxes;
        uint64_t infer_start_ns = monotonic_time_ns();
        ctx->detector.detect_person(frame.rgb.data(), frame.width, frame.height, boxes);
        const float threshold = ctx->person_confidence_threshold.load();
        boxes.erase(
            std::remove_if(boxes.begin(), boxes.end(), [threshold](const auto &box) {
                return !person_score_exceeds_threshold(box.score, threshold);
            }),
            boxes.end());
        const bool has_person = ctx->person_detection_enabled.load() && !boxes.empty();
        uint64_t infer_end_ns = monotonic_time_ns();
        double infer_ms = static_cast<double>(infer_end_ns - infer_start_ns) / 1000000.0;
        perf_log("infer ts_ns=%llu seq=%llu latency_ms=%.3f has_person=%d boxes=%zu\n",
                 static_cast<unsigned long long>(frame.ts_ns),
                 static_cast<unsigned long long>(frame.seq),
                 infer_ms,
                 has_person ? 1 : 0,
                 boxes.size());
        const auto selection_now = std::chrono::steady_clock::now();
        const bool create_event = ctx->person_event_gate.update(has_person, selection_now);
        if (has_person) {
            {
                std::lock_guard<std::mutex> guard(ctx->detect_mtx);
                ctx->last_person_ts_ns = frame.ts_ns;
            }
            {
                std::lock_guard<std::mutex> record_lock(ctx->record_mtx);
                ++ctx->record_frame_gen;
            }
            ctx->record_cv.notify_all();
        }
        if (create_event && ctx->event_processor) {
            double confidence = 0.0;
            for (const auto &box : boxes) confidence = std::max(confidence, static_cast<double>(box.score));
            std::string recording_id;
            {
                std::unique_lock<std::mutex> record_lock(ctx->record_mtx);
                ctx->record_cv.wait_for(
                    record_lock,
                    std::chrono::seconds(3),
                    [ctx] { return !ctx->current_recording_id.empty() || g_stop.load(); });
                recording_id = ctx->current_recording_id;
            }
            if (recording_id.empty()) {
                std::fprintf(stderr, "[event] skip person event because recorder did not start\n");
                ctx->person_event_gate.reset();
            } else {
                std::string error;
                auto event = ctx->event_processor->begin_person_event(
                    confidence, std::chrono::system_clock::now(), recording_id, &error);
                if (!event) {
                    std::fprintf(stderr, "[event] persist person event failed error=%s\n", error.c_str());
                    ctx->person_event_gate.reset();
                } else {
                    std::fprintf(stdout,
                                 "[event] created person event_id=%s recording_id=%s\n",
                                 event->event_id.c_str(),
                                 event->recording_id.c_str());
                    ctx->pending_person_event = *event;
                    ctx->snapshot_candidate_selector.start(selection_now);
                }
            }
        }
        if (ctx->snapshot_candidate_selector.active()) {
            if (has_person) {
                std::vector<SnapshotPersonBox> snapshot_boxes;
                snapshot_boxes.reserve(boxes.size());
                for (const auto &box : boxes) {
                    snapshot_boxes.push_back(SnapshotPersonBox{
                        box.left, box.top, box.right, box.bottom, box.score});
                }
                ctx->snapshot_candidate_selector.consider(
                    frame.rgb, frame.width, frame.height, snapshot_boxes, selection_now);
            }
            auto selected = ctx->snapshot_candidate_selector.take_if_ready(selection_now);
            if (selected && ctx->pending_person_event && ctx->event_processor) {
                const std::string event_id = ctx->pending_person_event->event_id;
                ctx->event_processor->submit_person_snapshot(
                    std::move(*ctx->pending_person_event),
                    std::move(selected->rgb),
                    selected->width,
                    selected->height);
                ctx->pending_person_event.reset();
                std::fprintf(stdout,
                             "[event] selected snapshot event_id=%s score=%.3f confidence=%.3f\n",
                             event_id.c_str(), selected->score, selected->confidence);
            }
        }
        {
            std::lock_guard<std::mutex> guard(ctx->detect_mtx);
            if (has_person) {
                ctx->latest_boxes = std::move(boxes);
                ctx->latest_boxes_ts_ns = frame.ts_ns;
            } else {
                ctx->latest_boxes.clear();
                ctx->latest_boxes_ts_ns = 0;
            }
        }

    }
}

void recorder_worker(UiContext *ctx) {
    uint64_t last_written_ts_ns = 0;
    uint64_t last_seen_seq = 0;
    uint64_t seq_zero_count = 0;
    uint64_t seq_duplicate_count = 0;
    uint64_t seq_regress_count = 0;
    Camera::MediaFrame media_frame;
    uint64_t consumed_frame_gen = 0;

    {
        std::lock_guard<std::mutex> lock(ctx->record_mtx);
        consumed_frame_gen = ctx->record_frame_gen;
    }

    auto finalize_recording = [ctx](const std::string &status) {
        std::string recording_id;
        std::chrono::system_clock::time_point ended_at;
        {
            std::lock_guard<std::mutex> lock(ctx->record_mtx);
            recording_id = ctx->current_recording_id;
            ended_at = ctx->current_recording_last_segment_ended_at;
            ctx->current_recording_id.clear();
            ctx->current_recording_last_segment_ended_at = {};
            if (ctx->last_recording_attempt_id == recording_id) {
                ctx->last_recording_attempt_id.clear();
            }
            ctx->recording_stop_in_progress = false;
        }
        ctx->record_cv.notify_all();
        if (recording_id.empty() || !ctx->event_processor) return;
        if (ended_at.time_since_epoch().count() == 0) {
            ended_at = std::chrono::system_clock::now();
        }
        std::string error;
        if (!ctx->event_processor->finalize_recording(recording_id, ended_at, status, &error)) {
            std::fprintf(stderr,
                         "[record] persist completion failed recording_id=%s error=%s\n",
                         recording_id.c_str(),
                         error.c_str());
        }
    };
    auto finalize_pending_ring_recording = [ctx]() {
        std::string recording_id;
        std::chrono::system_clock::time_point started_at;
        {
            std::lock_guard<std::mutex> lock(ctx->record_mtx);
            recording_id = std::move(ctx->pending_ring_recording_id);
            started_at = ctx->pending_ring_recording_started_at;
            ctx->pending_ring_recording_id.clear();
            ctx->pending_ring_recording_started_at = {};
            if (ctx->last_recording_attempt_id == recording_id) {
                ctx->last_recording_attempt_id.clear();
            }
        }
        if (recording_id.empty() || !ctx->event_processor) return;
        if (started_at.time_since_epoch().count() == 0) {
            started_at = std::chrono::system_clock::now();
        }
        std::string error;
        if (!ctx->event_processor->finalize_recording(
                recording_id, started_at, "failed", &error)) {
            std::fprintf(stderr,
                         "[record] persist pending ring recording failure failed "
                         "recording_id=%s error=%s\n",
                         recording_id.c_str(),
                         error.c_str());
        }
    };

    while (!g_stop.load()) {
        uint64_t wait_begin_ns = monotonic_time_ns();
 
        {
            std::unique_lock<std::mutex> lock(ctx->record_mtx);
            ctx->record_cv.wait(lock, [ctx, &consumed_frame_gen] {
                return ctx->record_exit || g_stop.load() || ctx->record_frame_gen != consumed_frame_gen;
            });
            if (ctx->record_exit || g_stop.load()) break;
            consumed_frame_gen = ctx->record_frame_gen;
        }

        uint64_t wait_end_ns = monotonic_time_ns();
        double wait_ms = static_cast<double>(wait_end_ns - wait_begin_ns) / 1000000.0;

        uint64_t last_person_ts_ns = 0;
        {
            std::lock_guard<std::mutex> lock(ctx->detect_mtx);
            last_person_ts_ns = ctx->last_person_ts_ns;
        }

        const uint64_t now_ns = monotonic_time_ns();
        bool ring_open = false;
        {
            std::lock_guard<std::mutex> lock(ctx->record_mtx);
            ring_open = ctx->ring_event_open;
        }
        const bool want_record =
            person_recording_active(last_person_ts_ns, now_ns) ||
            ring_recording_active(ring_open);

        if (!want_record) {
            if (ctx->recorder.running()) {
                {
                    std::lock_guard<std::mutex> lock(ctx->record_mtx);
                    if (ctx->ring_press_in_progress || ctx->ring_event_open) {
                        continue;
                    }
                    ctx->recording_stop_in_progress = true;
                }
                std::fprintf(stdout, "[record] stop %s\n", ctx->recorder.last_file().c_str());
                uint64_t stop_begin_ns = monotonic_time_ns();
                ctx->recorder.stop();
                finalize_recording("finalized");
                uint64_t stop_end_ns = monotonic_time_ns();
                perf_log("record stop ts_ns=%llu stop_ms=%.3f reason=no_want\n",
                         static_cast<unsigned long long>(stop_end_ns),
                         static_cast<double>(stop_end_ns - stop_begin_ns) / 1000000.0);
                last_written_ts_ns = 0;
            } else {
                finalize_pending_ring_recording();
            }
            continue;
        }

        uint32_t camera_width = 0;
        uint32_t camera_height = 0;
        uint32_t camera_pixfmt = 0;
        bool camera_ready = false;
        {
            std::lock_guard<std::mutex> camera_lock(ctx->camera_mtx);
            if (ctx->camera && ctx->camera->ready()) {
                camera_width = ctx->camera->width();
                camera_height = ctx->camera->height();
                camera_pixfmt = ctx->camera->media_pixel_format();
                camera_ready = true;
            }
        }
        if (!camera_ready) {
            finalize_pending_ring_recording();
            continue;
        }

		if (!ctx->recorder.running()) {
            uint64_t start_begin_ns = monotonic_time_ns();
            auto recording_started_at = std::chrono::system_clock::now();
            std::string recording_error;
            std::optional<std::string> recording_id;
            bool waiting_for_ring_identity = false;
            {
                std::lock_guard<std::mutex> lock(ctx->record_mtx);
                if (!ctx->pending_ring_recording_id.empty()) {
                    recording_id = ctx->pending_ring_recording_id;
                    recording_started_at = ctx->pending_ring_recording_started_at;
                } else if (ctx->ring_event_open) {
                    waiting_for_ring_identity = true;
                } else if (ctx->event_processor) {
                    recording_id = ctx->event_processor->register_started_recording(
                        recording_started_at, &recording_error);
                    if (recording_id) {
                        ctx->last_recording_attempt_id = *recording_id;
                    }
                }
            }
            if (waiting_for_ring_identity) {
                continue;
            }
            bool started = false;
            if (recording_id) {
                ctx->record_cv.notify_all();
                const std::string temporary_pattern_ref =
                    clip_directory_relative_path(recording_started_at) + "/." +
                    *recording_id + "-%06d.mp4.part";
                const std::string temporary_pattern =
                    (std::filesystem::path(ctx->record_dir) / temporary_pattern_ref).string();
                const std::string recording_quality = ctx->recording_quality_provider
                    ? ctx->recording_quality_provider()
                    : "1080p";
                const RecordingProfile recording_profile =
                    recording_profile_for(recording_quality);
                started = ctx->recorder.start_segmented(
                    temporary_pattern,
                    camera_width,
                    camera_height,
                    recording_profile.width,
                    recording_profile.height,
                    kRecordFpsLimit,
                    camera_pixfmt,
                    recording_profile.bitrate,
                    [ctx, recording_id = *recording_id, recording_started_at](
                        const Mp4Recorder::ClosedSegment &segment) {
                        const int segment_index = static_cast<int>(segment.fragment_index) + 1;
                        const std::string clip_ref = recording_segment_relative_path(
                            recording_started_at,
                            static_cast<uint32_t>(segment_index));
                        const auto segment_started_at = recording_started_at +
                            std::chrono::nanoseconds(segment.started_running_time_ns);
                        const auto segment_ended_at = recording_started_at +
                            std::chrono::nanoseconds(segment.ended_running_time_ns);
                        ctx->event_processor->submit_completed_recording_segment(
                            recording_id,
                            segment_index,
                            segment.temporary_file,
                            clip_ref,
                            segment_started_at,
                            segment_ended_at);
                        {
                            std::lock_guard<std::mutex> lock(ctx->record_mtx);
                            if (ctx->current_recording_id == recording_id) {
                                ctx->current_recording_last_segment_ended_at = segment_ended_at;
                            }
                        }
                    });
                if (!started) {
                    std::string finalize_error;
                    if (!ctx->event_processor->finalize_recording(
                            *recording_id,
                            recording_started_at,
                            "failed",
                            &finalize_error)) {
                        std::fprintf(stderr,
                                     "[record] persist failed startup state failed recording_id=%s error=%s\n",
                                     recording_id->c_str(),
                                     finalize_error.c_str());
                    }
                    {
                        std::lock_guard<std::mutex> lock(ctx->record_mtx);
                        if (ctx->pending_ring_recording_id == *recording_id) {
                            ctx->pending_ring_recording_id.clear();
                            ctx->pending_ring_recording_started_at = {};
                        }
                        if (ctx->last_recording_attempt_id == *recording_id) {
                            ctx->last_recording_attempt_id.clear();
                        }
                    }
                }
            }
            uint64_t start_end_ns = monotonic_time_ns();
            perf_log("record start wait_ms=%.3f start_ms=%.3f ok=%d w=%u h=%u fmt=%c%c%c%c\n",
                     wait_ms,
                     static_cast<double>(start_end_ns - start_begin_ns) / 1000000.0,
                     started ? 1 : 0,
                     camera_width,
                     camera_height,
                     camera_pixfmt & 0xFF,
                     (camera_pixfmt >> 8) & 0xFF,
                     (camera_pixfmt >> 16) & 0xFF,
                     (camera_pixfmt >> 24) & 0xFF);
            if (!started) {
                std::fprintf(stderr,
                             "[record] failed to start recorder error=%s\n",
                             recording_error.c_str());
                continue;
            }
            last_written_ts_ns = 0;
            std::fprintf(stdout, "[record] start %s\n", ctx->recorder.last_file().c_str());
            {
                std::lock_guard<std::mutex> lock(ctx->record_mtx);
                ctx->current_recording_id = *recording_id;
                ctx->current_recording_last_segment_ended_at = {};
                if (ctx->pending_ring_recording_id == *recording_id) {
                    ctx->pending_ring_recording_id.clear();
                    ctx->pending_ring_recording_started_at = {};
                }
            }
            ctx->record_cv.notify_all();
        }

        FramePacket frame;
        {
            std::lock_guard<std::mutex> camera_lock(ctx->camera_mtx);
            if (!ctx->camera || !ctx->camera->ready()) {
                continue;
            }
            if (!ctx->camera->copy_latest_media_frame(media_frame)) {
                continue;
            }
            frame.width = media_frame.width;
            frame.height = media_frame.height;
            frame.seq = media_frame.seq;
            frame.ts_ns = media_frame.ts_ns;
            frame.pixfmt = media_frame.pixfmt;
        }
        if (frame.seq == 0) {
            ++seq_zero_count;
            perf_log("record drop seq=%llu reason=seq_zero last_seen_seq=%llu dup=%llu regress=%llu zero=%llu\n",
                     static_cast<unsigned long long>(frame.seq),
                     static_cast<unsigned long long>(last_seen_seq),
                     static_cast<unsigned long long>(seq_duplicate_count),
                     static_cast<unsigned long long>(seq_regress_count),
                     static_cast<unsigned long long>(seq_zero_count));
            continue;
        }
        if (last_seen_seq != 0 && frame.seq < last_seen_seq) {
            ++seq_regress_count;
            perf_log("record drop seq=%llu reason=seq_regress last_seen_seq=%llu dup=%llu regress=%llu zero=%llu\n",
                     static_cast<unsigned long long>(frame.seq),
                     static_cast<unsigned long long>(last_seen_seq),
                     static_cast<unsigned long long>(seq_duplicate_count),
                     static_cast<unsigned long long>(seq_regress_count),
                     static_cast<unsigned long long>(seq_zero_count));
            continue;
        }
        if (frame.seq == last_seen_seq) {
            ++seq_duplicate_count;
            perf_log("record drop seq=%llu reason=seq_duplicate last_seen_seq=%llu dup=%llu regress=%llu zero=%llu\n",
                     static_cast<unsigned long long>(frame.seq),
                     static_cast<unsigned long long>(last_seen_seq),
                     static_cast<unsigned long long>(seq_duplicate_count),
                     static_cast<unsigned long long>(seq_regress_count),
                     static_cast<unsigned long long>(seq_zero_count));
            continue;
        }
        last_seen_seq = frame.seq;
        if (media_frame.data == nullptr || media_frame.size == 0 || frame.width == 0 || frame.height == 0) {
            continue;
        }

        assert(frame.width > 0 && frame.height > 0 && media_frame.data != nullptr && media_frame.size > 0 && "invalid media frame");

        // if (frame.ts_ns != 0 && last_written_ts_ns != 0) {
        //     if (frame.ts_ns > last_written_ts_ns && (frame.ts_ns - last_written_ts_ns) < kRecordFrameIntervalNs) {
        //         perf_log("record drop ts_ns=%llu seq=%llu reason=fps_limit target_fps=%u\n",
        //                  static_cast<unsigned long long>(frame.ts_ns),
        //                  static_cast<unsigned long long>(frame.seq),
        //                  kRecordFpsLimit);
        //         continue;
        //     }
        // }

        uint64_t write_begin_ns = monotonic_time_ns();
        bool write_ok = ctx->recorder.write_frame(
            media_frame.data,
            media_frame.size,
            frame.ts_ns,
            media_frame.fd,
            media_frame.stride_y);
        uint64_t write_end_ns = monotonic_time_ns();
        perf_log("record write ts_ns=%llu seq=%llu wait_ms=%.3f write_ms=%.3f ok=%d bytes=%zu\n",
                 static_cast<unsigned long long>(frame.ts_ns),
                 static_cast<unsigned long long>(frame.seq),
                 wait_ms,
                 static_cast<double>(write_end_ns - write_begin_ns) / 1000000.0,
                 write_ok ? 1 : 0,
                 media_frame.size);

        if (write_ok && frame.ts_ns != 0) {
            last_written_ts_ns = frame.ts_ns;
        }

        if (!write_ok) {
            std::fprintf(stderr, "[record] write_frame failed, stopping recorder\n");
            uint64_t stop_begin_ns = monotonic_time_ns();
            {
                std::lock_guard<std::mutex> lock(ctx->record_mtx);
                ctx->recording_stop_in_progress = true;
            }
            ctx->recorder.stop();
            finalize_recording("interrupted");
            uint64_t stop_end_ns = monotonic_time_ns();
            perf_log("record stop ts_ns=%llu stop_ms=%.3f reason=write_fail\n",
                     static_cast<unsigned long long>(stop_end_ns),
                     static_cast<double>(stop_end_ns - stop_begin_ns) / 1000000.0);
        }
    }

    if (ctx->recorder.running()) {
        std::fprintf(stdout, "[record] stop %s\n", ctx->recorder.last_file().c_str());
        uint64_t stop_begin_ns = monotonic_time_ns();
        {
            std::lock_guard<std::mutex> lock(ctx->record_mtx);
            ctx->recording_stop_in_progress = true;
        }
        ctx->recorder.stop();
        finalize_recording("interrupted");
        uint64_t stop_end_ns = monotonic_time_ns();
        perf_log("record stop ts_ns=%llu stop_ms=%.3f reason=thread_exit\n",
                 static_cast<unsigned long long>(stop_end_ns),
                 static_cast<double>(stop_end_ns - stop_begin_ns) / 1000000.0);
    }
    perf_log("record seq_stats dup=%llu regress=%llu zero=%llu last_seen_seq=%llu\n",
             static_cast<unsigned long long>(seq_duplicate_count),
             static_cast<unsigned long long>(seq_regress_count),
             static_cast<unsigned long long>(seq_zero_count),
             static_cast<unsigned long long>(last_seen_seq));
}

void handle_signal(int) { g_stop.store(true); }

lv_display_t *create_display(const Camera::Config &) {
    uint32_t w = kDisplayWidth;
    uint32_t h = kDisplayHeight;
#if LV_USE_SDL
    lv_display_t *disp = lv_sdl_window_create(static_cast<int>(w), static_cast<int>(h));
    if (disp) {
        lv_display_set_default(disp);
        std::fprintf(stdout, "[create_display] use SDL window %ux%u.\n", w, h);
        return disp;
    }
#endif
    std::fprintf(stderr, "[create_display] failed to create SDL display\n");
    return nullptr;
}

std::string executable_dir() {
    char path[PATH_MAX] = {0};
    ssize_t n = ::readlink("/proc/self/exe", path, sizeof(path) - 1);
    if (n <= 0) return ".";
    path[n] = '\0';
    return std::filesystem::path(path).parent_path().string();
}

std::string select_model_path(const char *argv1_model_path) {
    if (argv1_model_path && argv1_model_path[0] != '\0' && ::access(argv1_model_path, R_OK) == 0) {
        return argv1_model_path;
    }

    const std::string exe_model = executable_dir() + "/yolov8s_single_person.rknn";
    const char *candidates[] = {
        exe_model.c_str(),
		"./yolov8s_single_person.rknn",
		"./yolov8s.rknn",
        
    };
    for (const char *p : candidates) {
        if (p && ::access(p, R_OK) == 0) return p;
    }
    return "";
}

std::string select_sd_record_dir() {
    if (const char *env = std::getenv("DOORBELL_SD_DIR")) {
        if (::access(env, W_OK) == 0) {
            return std::string(env) + "/doorbell_videos";
        }
    }
    const char *candidates[] = {
        "/mnt/sdcard",
        "/media/sdcard",
        "/sdcard",
        "/userdata/sdcard",
    };
    for (const char *root : candidates) {
        if (::access(root, W_OK) == 0) {
            return std::string(root) + "/doorbell_videos";
        }
    }
    return "/tmp/doorbell_videos";
}

void camera_timer_cb(lv_timer_t *timer) {
    auto *ctx = static_cast<UiContext *>(lv_timer_get_user_data(timer));
    if (!ctx || !ctx->camera || !ctx->camera->ready()) return;

    const uint64_t cb_begin_ns = monotonic_time_ns();

    const uint32_t width = ctx->camera->width();
    const uint32_t height = ctx->camera->height();
    const size_t rgb_size = static_cast<size_t>(width) * static_cast<size_t>(height) * 3;
    uint64_t frame_seq = 0;
    uint64_t frame_ts_ns = 0;
    uint64_t copy_begin_ns = monotonic_time_ns();
    bool result = ctx->camera->copy_latest_frame(ctx->blank, frame_seq, frame_ts_ns);
    uint64_t copy_end_ns = monotonic_time_ns();
    const uint64_t copy_stage_begin_ns = copy_begin_ns;
    const uint64_t copy_stage_end_ns = copy_end_ns;
    double copy_ms = static_cast<double>(copy_stage_end_ns - copy_stage_begin_ns) / 1000000.0;
	// perf_logger_log("[camera_timer_cb] copy_latest_frames ts_ns=%llu seq=%llu latency_ms=%.3f\n",
	// 				static_cast<unsigned long long>(frame_ts_ns),
	// 				static_cast<unsigned long long>(frame_seq),
	// 				copy_ms);
	
	if (!result) {
        uint64_t cb_end_ns = monotonic_time_ns();
        perf_logger_log("[camera_timer_cb] total_ms=%.3f reason=no_data\n",
                    static_cast<double>(cb_end_ns - cb_begin_ns) / 1000000.0);        	
		return; 
	}

    if (ctx->blank.size() < rgb_size) {
        ctx->blank.resize(rgb_size);
    }
    const uint32_t now = lv_tick_get();
    std::optional<EventRecord> completed_ring_snapshot_event;
    std::optional<SelectedSnapshot> selected_ring_snapshot;
    {
        std::lock_guard<std::mutex> lock(ctx->ring_snapshot_mtx);
        if (ctx->ring_snapshot_candidate_selector.active()) {
            const auto selection_now = std::chrono::steady_clock::now();
            if (ctx->ring_snapshot_last_sample_ms == 0 ||
                now - ctx->ring_snapshot_last_sample_ms >=
                    kRingSnapshotSampleIntervalMs) {
                ctx->ring_snapshot_last_sample_ms = now;
                ctx->ring_snapshot_candidate_selector.consider(
                    ctx->blank, width, height, {}, selection_now);
            }
            selected_ring_snapshot =
                ctx->ring_snapshot_candidate_selector.take_if_ready(selection_now);
            if (!ctx->ring_snapshot_candidate_selector.active() &&
                ctx->pending_ring_snapshot_event) {
                completed_ring_snapshot_event =
                    std::move(ctx->pending_ring_snapshot_event);
                ctx->pending_ring_snapshot_event.reset();
                ctx->ring_snapshot_last_sample_ms = 0;
            }
        }
    }
    if (completed_ring_snapshot_event && ctx->event_processor) {
        if (selected_ring_snapshot) {
            ctx->event_processor->submit_ring_snapshot(
                std::move(*completed_ring_snapshot_event),
                std::move(selected_ring_snapshot->rgb),
                selected_ring_snapshot->width,
                selected_ring_snapshot->height);
        } else {
            ctx->event_processor->submit_ring_snapshot(
                std::move(*completed_ring_snapshot_event), {}, 0, 0);
        }
    }

    image_buffer_t rgb_img{};
    rgb_img.width = static_cast<int>(width);
    rgb_img.height = static_cast<int>(height);
    rgb_img.format = IMAGE_FORMAT_RGB888;
    rgb_img.size = static_cast<int>(rgb_size);
    rgb_img.virt_addr = ctx->blank.data();
    rgb_img.fd = -1;

    copy_begin_ns = monotonic_time_ns();
    if (ctx->detector_ready && ctx->person_detection_enabled.load() &&
        now - ctx->last_detect_ms >= kDetectIntervalMs) {
        ctx->last_detect_ms = now;
        {
            std::lock_guard<std::mutex> lock(ctx->infer_mtx);
            ctx->infer_input.width = width;
            ctx->infer_input.height = height;
            ctx->infer_input.seq = frame_seq;
            ctx->infer_input.ts_ns = frame_ts_ns;
            ctx->infer_input.rgb = ctx->blank;
            ctx->infer_has_input = true;
        }
        ctx->infer_cv.notify_one();
    }
	copy_end_ns = monotonic_time_ns();
	// perf_logger_log("[camera_timer_cb] inference_worker submit ts_ns=%llu seq=%llu latency_ms=%.3f\n",
	// 				static_cast<unsigned long long>(frame_ts_ns),
	// 				static_cast<unsigned long long>(frame_seq),
	// 				static_cast<double>(copy_end_ns - copy_begin_ns) / 1000000.0);
    const uint64_t infer_submit_begin_ns = copy_begin_ns;
    const uint64_t infer_submit_end_ns = copy_end_ns;

    std::vector<YoloPersonDetector::PersonBox> overlay_boxes;
    uint64_t latest_boxes_ts_ns = 0;
    uint64_t last_person_ts_ns = 0;
    {
        std::lock_guard<std::mutex> lock(ctx->detect_mtx);
        latest_boxes_ts_ns = ctx->latest_boxes_ts_ns;
        last_person_ts_ns = ctx->last_person_ts_ns;
        if (detection_is_fresh(frame_ts_ns, latest_boxes_ts_ns)) {
            overlay_boxes = ctx->latest_boxes;
        }
    }
    const uint64_t overlay_fetch_end_ns = monotonic_time_ns();

    bool ring_open = false;
    {
        std::lock_guard<std::mutex> lock(ctx->record_mtx);
        ring_open = ctx->ring_event_open;
    }
    const bool want_record =
        person_recording_active(last_person_ts_ns, frame_ts_ns) ||
        ring_recording_active(ring_open);
    const uint64_t record_state_end_ns = monotonic_time_ns();

	uint64_t draw_rectangle_begin_ns = monotonic_time_ns();
	for (const auto &box : overlay_boxes) {
        int left = std::clamp(box.left, 0, static_cast<int>(width) - 1);
        int top = std::clamp(box.top, 0, static_cast<int>(height) - 1);
        int right = std::clamp(box.right, 0, static_cast<int>(width) - 1);
        int bottom = std::clamp(box.bottom, 0, static_cast<int>(height) - 1);
        int bw = right - left;
        int bh = bottom - top;
        if (bw <= 0 || bh <= 0) continue;
        draw_rectangle(&rgb_img, left, top, bw, bh, COLOR_RED, 2);
    }
	uint64_t draw_rectangle_end_ns = monotonic_time_ns();
	// perf_logger_log("[camera_timer_cb] draw_rectangle  latency_ms=%.3f\n",
	// 				static_cast<double>(draw_rectangle_end_ns - draw_rectangle_begin_ns) / 1000000.0);

    perf_logger_log("[camera_timer_cb] want_record=%d\n", want_record ? 1 : 0);

    if (ctx->perf_last_log_ms == 0) {
        ctx->perf_last_log_ms = now;
    }
    if (now - ctx->perf_last_log_ms >= 1000) {
        int infer_pending = 0;
        {
            std::lock_guard<std::mutex> lock(ctx->infer_mtx);
            infer_pending = ctx->infer_has_input ? 1 : 0;
        }
        perf_log("queue ts_ns=%llu frame_seq=%llu infer_pending=%d\n",
                 static_cast<unsigned long long>(frame_ts_ns),
                 static_cast<unsigned long long>(frame_seq),
                 infer_pending);
        ctx->perf_last_log_ms = now;
    }

    if (!ctx->display_pipeline) {
        return;
    }
    const RgaDisplayPipeline::RenderResult display_result =
        ctx->display_pipeline->render(ctx->blank.data(), rgb_size);
    if (display_result != RgaDisplayPipeline::RenderResult::Rendered) {
        if (display_result == RgaDisplayPipeline::RenderResult::Failed &&
            !ctx->display_fallback.empty() &&
            ctx->image_dsc.data != ctx->display_fallback.data()) {
            ctx->image_dsc.data = ctx->display_fallback.data();
            ctx->image_dsc.data_size = ctx->display_fallback.size();
            if (ctx->image_obj) {
                lv_image_set_src(ctx->image_obj, &ctx->image_dsc);
                lv_obj_invalidate(ctx->image_obj);
            }
        }
        return;
    }
    const uint64_t convert_end_ns = monotonic_time_ns();

    const uint8_t *display_data = ctx->display_pipeline->output_data();
    const bool display_buffer_changed = ctx->image_dsc.data != display_data;
    ctx->image_dsc.data = display_data;
    ctx->image_dsc.data_size = ctx->display_pipeline->output_size();
    if (display_buffer_changed && ctx->image_obj) {
        lv_image_set_src(ctx->image_obj, &ctx->image_dsc);
    }
    // Invalidate the whole screen to clear leftover regions when resolution shrinks.
    lv_obj_invalidate(lv_screen_active());

    // FPS counter in the top-left corner.
    if (ctx->fps_last_ms == 0) {
        ctx->fps_last_ms = now;
        ctx->frame_counter_last = ctx->camera->frame_counter();
    }
    if (now - ctx->fps_last_ms >= 1000 && ctx->fps_label) {
        uint64_t frames = ctx->camera->frame_counter();
        if (frames < ctx->frame_counter_last) {
            // Camera likely restarted; resync counters to avoid overflow.
            ctx->frame_counter_last = frames;
        }
        uint64_t delta = frames - ctx->frame_counter_last;
        uint32_t elapsed = now - ctx->fps_last_ms;
        double fps = elapsed ? (static_cast<double>(delta) * 1000.0 / static_cast<double>(elapsed)) : 0.0;
        char buf[32];
        std::snprintf(buf, sizeof(buf), "FPS:%.1f", fps);
        lv_label_set_text(ctx->fps_label, buf);
        ctx->frame_counter_last = frames;
        ctx->fps_last_ms = now;
    }

    const uint64_t cb_end_ns = monotonic_time_ns();
    perf_logger_log(
        "[camera_timer_cb] stage_ms total=%.3f copy=%.3f infer_submit=%.3f overlay_fetch=%.3f draw=%.3f record_enqueue=%.3f convert=%.3f ui=%.3f\n",
        static_cast<double>(cb_end_ns - cb_begin_ns) / 1000000.0,
        static_cast<double>(copy_stage_end_ns - copy_stage_begin_ns) / 1000000.0,
        static_cast<double>(infer_submit_end_ns - infer_submit_begin_ns) / 1000000.0,
        static_cast<double>(overlay_fetch_end_ns - infer_submit_end_ns) / 1000000.0,
        static_cast<double>(draw_rectangle_end_ns - draw_rectangle_begin_ns) / 1000000.0,
        static_cast<double>(record_state_end_ns - draw_rectangle_end_ns) / 1000000.0,
        static_cast<double>(convert_end_ns - record_state_end_ns) / 1000000.0,
        static_cast<double>(cb_end_ns - convert_end_ns) / 1000000.0);

}

void exit_btn_event_cb(lv_event_t *e) {
    if (lv_event_get_code(e) == LV_EVENT_CLICKED) {
        g_stop.store(true);
    }
}

bool restart_camera(void *user_ctx, uint32_t pixfmt, uint32_t w, uint32_t h) {
    auto *ctx = static_cast<UiContext *>(user_ctx);
    if (!ctx) return false;

    Camera::Config new_cfg = ctx->cfg;
    new_cfg.pixelformat = pixfmt;
    new_cfg.width = kMasterCaptureWidth;
    new_cfg.height = kMasterCaptureHeight;
    new_cfg.fps = kMasterCaptureFps;

    std::lock_guard<std::mutex> camera_lock(ctx->camera_mtx);
    Camera *old = ctx->camera;
    const bool rotate180 = old && old->rotate180();
    if (old) old->stop();

    Camera *cam = new Camera(new_cfg);
    cam->set_rotate180(rotate180);
    if (!cam->start()) {
        std::fprintf(stderr, "[restart] camera start failed fmt=%c%c%c%c %ux%u\n",
                     pixfmt & 0xFF, (pixfmt >> 8) & 0xFF, (pixfmt >> 16) & 0xFF, (pixfmt >> 24) & 0xFF,
                     w, h);
        delete cam;
        if (old) old->start();
        return false;
    }
    cam->set_frame_ready_callback([ctx] {
        notify_record_new_frame(ctx);
    });

    delete old;
    ctx->camera = cam;
    ctx->cfg = new_cfg;

    ctx->image_dsc.header.w = kDisplayWidth;
    ctx->image_dsc.header.h = kDisplayHeight;
    ctx->image_dsc.header.cf = LV_COLOR_FORMAT_ARGB8888;
    ctx->image_dsc.header.stride = kDisplayWidth * 4;
    size_t bgra_size =
        static_cast<size_t>(kDisplayWidth) * static_cast<size_t>(kDisplayHeight) * 4;
    ctx->image_dsc.data_size = bgra_size;
    if (!ctx->image_dsc.data) {
        ctx->display_fallback.assign(bgra_size, 0);
        ctx->image_dsc.data = ctx->display_fallback.data();
    }
    ctx->image_dsc.reserved = nullptr;
    ctx->image_dsc.reserved_2 = nullptr;
    ctx->blank.clear();
    {
        std::lock_guard<std::mutex> lock(ctx->detect_mtx);
        ctx->latest_boxes.clear();
        ctx->latest_boxes_ts_ns = 0;
        ctx->last_person_ts_ns = 0;
    }
    // Rebind image src so LVGL refreshes cached header/stride when resolution changes.
    lv_image_set_src(ctx->image_obj, &ctx->image_dsc);
    lv_obj_set_size(ctx->image_obj, kDisplayWidth, kDisplayHeight);
    lv_obj_center(ctx->image_obj);
    lv_obj_invalidate(ctx->image_obj);
    // Reset FPS counters after format/size switch to avoid a large transient value.
    ctx->frame_counter_last = cam->frame_counter();
    ctx->fps_last_ms = lv_tick_get();
    std::fprintf(stdout, "[restart] camera restarted fmt=%c%c%c%c %ux%u\n",
                 pixfmt & 0xFF, (pixfmt >> 8) & 0xFF, (pixfmt >> 16) & 0xFF, (pixfmt >> 24) & 0xFF,
                 w, h);
    return true;
}
}

int main(int argc, char **argv) {
    Camera::Config cfg;
    const char *model_arg = (argc >= 2) ? argv[1] : nullptr;
    const int arg_base = model_arg ? 2 : 1;
    // 正式媒体架构固定一路 1440P 主采集，分辨率只在下游分支切换。
    cfg.width = kMasterCaptureWidth;
    cfg.height = kMasterCaptureHeight;
    cfg.fps = kMasterCaptureFps;
    if (argc >= arg_base + 3) {
        cfg.device = argv[arg_base + 2];
    }
    if (argc >= arg_base + 4) {
        std::fprintf(stdout,
                     "[main] ignore legacy capture fps argument; master capture is fixed at %u fps\n",
                     kMasterCaptureFps);
    }

    signal(SIGINT, handle_signal);
    signal(SIGTERM, handle_signal);

    DoorbellProvisioning provisioning;
    if (!provisioning.start()) {
        std::fprintf(stderr, "[main] provisioning service disabled\n");
    }

    lv_init();

    const std::string perf_log_path = executable_dir() + "/doorbell_rk3568_perf.log";
    init_perf_log_file(perf_log_path);

    lv_display_t *disp = create_display(cfg);
    if (!disp) {
        std::fprintf(stderr, "Display init failed. Check lv_conf.h backend selection.\n");
        return EXIT_FAILURE;
    }
	// lv_display_set_rotation(disp, LV_DISPLAY_ROTATION_90);
    std::fprintf(stdout, "[main] display ready\n");

#if LV_USE_SDL
    lv_indev_t *indev = lv_sdl_mouse_create();
    if (indev) {
        lv_indev_set_display(indev, disp);
        std::fprintf(stdout, "[main] SDL mouse input ready\n");
    } else {
        std::fprintf(stderr, "[main] failed to create SDL mouse input\n");
    }
#endif

    UiContext ctx;
    ctx.cfg = cfg;
    ctx.recording_quality_provider = [&provisioning] {
        return provisioning.current_settings().recording_quality;
    };
    ctx.disp = disp;
    ctx.recorder.set_audio_dispatcher(ctx.audio_capture.dispatcher());
    provisioning.set_live_audio_manager(&ctx.audio_capture);
    std::string audio_error;
    if (!ctx.audio_capture.start("plughw:0,0", &audio_error)) {
        std::fprintf(stderr, "[audio] capture disabled: %s\n", audio_error.c_str());
    } else {
        std::fprintf(stdout, "[audio] capture manager started device=plughw:0,0\n");
        const char *configured_chime = std::getenv("DOORBELL_CHIME_WAV");
        const std::string chime_path =
            configured_chime && *configured_chime
            ? configured_chime
            : "/userdata/doorbell/voices/dingdong.wav";
        ctx.doorbell_chime =
            std::make_unique<DoorbellChimePlayer>(&ctx.audio_capture, chime_path);
        ctx.doorbell_chime->start();
    }
    // ctx.sdl_window = lv_sdl_window_get_window(disp);
    // if (ctx.sdl_window) {
    //     if (SDL_SetWindowFullscreen(ctx.sdl_window, SDL_WINDOW_FULLSCREEN_DESKTOP) != 0) {
    //         std::fprintf(stderr, "[main] failed to set fullscreen: %s\n", SDL_GetError());
    //     }
    // }
    std::memset(&ctx.image_dsc, 0, sizeof(ctx.image_dsc));
    ctx.camera = new Camera(ctx.cfg);
    ctx.camera->set_rotate180(provisioning.current_settings().image_rotate180);
    if (!ctx.camera->start()) {
        std::fprintf(stderr, "Camera start failed on %s\n", cfg.device.c_str());
        return EXIT_FAILURE;
    }
    ctx.camera->set_frame_ready_callback([&ctx] {
        notify_record_new_frame(&ctx);
    });
    provisioning.set_live_frame_provider([&ctx](LiveWebRtcSession::VideoFrame &frame) {
        Camera::MediaFrame media_frame;
        std::lock_guard<std::mutex> camera_lock(ctx.camera_mtx);
        if (!ctx.camera || !ctx.camera->ready()) {
            return false;
        }
        if (!ctx.camera->copy_latest_media_frame(media_frame)) {
            return false;
        }
        frame.data = media_frame.data;
        frame.size = media_frame.size;
        frame.dmabuf_fd = media_frame.fd;
        frame.width = media_frame.width;
        frame.height = media_frame.height;
        frame.pixfmt = media_frame.pixfmt;
        frame.stride_y = media_frame.stride_y;
        frame.stride_uv = media_frame.stride_uv;
        frame.seq = media_frame.seq;
        frame.ts_ns = media_frame.ts_ns;
        return true;
    });
    std::fprintf(stdout, "[main] camera started\n");

    provisioning.set_settings_apply_handlers(
        [&ctx](bool enabled, const std::string &sensitivity) {
            ctx.person_confidence_threshold.store(person_sensitivity_threshold(sensitivity));
            ctx.person_detection_enabled.store(enabled);
            if (!enabled) {
                std::lock_guard<std::mutex> lock(ctx.detect_mtx);
                ctx.latest_boxes.clear();
                ctx.latest_boxes_ts_ns = 0;
                ctx.last_person_ts_ns = 0;
            }
            return true;
        },
        [&ctx](bool enabled) {
            std::lock_guard<std::mutex> lock(ctx.camera_mtx);
            return ctx.camera && ctx.camera->set_rotate180(enabled);
        });

    // Wait briefly for first frame; fallback to blank buffer
    for (int i = 0; i < 50 && !ctx.camera->ready(); ++i) {
        usleep(10 * 1000);
    }

    ctx.image_dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
    ctx.image_dsc.header.cf = LV_COLOR_FORMAT_ARGB8888;
    ctx.image_dsc.header.flags = 0;
    ctx.image_dsc.header.w = kDisplayWidth;
    ctx.image_dsc.header.h = kDisplayHeight;
    ctx.image_dsc.header.stride = kDisplayWidth * 4;
    ctx.image_dsc.header.reserved_2 = 0;
    size_t bgra_size =
        static_cast<size_t>(kDisplayWidth) * static_cast<size_t>(kDisplayHeight) * 4;
    ctx.image_dsc.data_size = bgra_size;
    ctx.display_fallback.assign(bgra_size, 0);
    ctx.image_dsc.data = ctx.display_fallback.data();
    ctx.display_pipeline = std::make_unique<RgaDisplayPipeline>(
        kMasterCaptureWidth, kMasterCaptureHeight, kDisplayWidth, kDisplayHeight);
    if (ctx.display_pipeline->initialize()) {
        ctx.image_dsc.data = ctx.display_pipeline->output_data();
        ctx.image_dsc.data_size = ctx.display_pipeline->output_size();
    }
    ctx.image_dsc.reserved = nullptr;
    ctx.image_dsc.reserved_2 = nullptr;

    ctx.image_obj = lv_image_create(lv_screen_active());
    lv_obj_set_style_bg_color(lv_screen_active(), lv_color_black(), 0);
    lv_obj_set_style_bg_opa(lv_screen_active(), LV_OPA_COVER, 0);
    lv_image_set_src(ctx.image_obj, &ctx.image_dsc);
    lv_obj_set_size(ctx.image_obj, kDisplayWidth, kDisplayHeight);
    lv_obj_center(ctx.image_obj);
    std::fprintf(stdout, "[main] image object created\n");

    // FPS label at top-left
    ctx.fps_label = lv_label_create(lv_screen_active());
    lv_obj_set_style_text_color(ctx.fps_label, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(ctx.fps_label, LV_OPA_50, 0);
    lv_obj_set_style_bg_color(ctx.fps_label, lv_color_black(), 0);
    lv_obj_align(ctx.fps_label, LV_ALIGN_TOP_LEFT, 4, 4);
    lv_label_set_text(ctx.fps_label, "FPS:0.0");

    // Exit button at bottom-right.
    lv_obj_t *exit_btn = lv_button_create(lv_screen_active());
    lv_obj_align(exit_btn, LV_ALIGN_BOTTOM_RIGHT, -8, -8);
    lv_obj_t *exit_lbl = lv_label_create(exit_btn);
    lv_label_set_text(exit_lbl, "Exit");
    lv_obj_center(exit_lbl);
    lv_obj_add_event_cb(exit_btn, exit_btn_event_cb, LV_EVENT_CLICKED, nullptr);

    ctx.blank.clear();
    ctx.record_dir = select_sd_record_dir();
    std::filesystem::create_directories(ctx.record_dir);
    std::fprintf(stdout, "[record] output dir: %s\n", ctx.record_dir.c_str());

    if (!provisioning.device_id().empty()) {
        const char *api_base_env = std::getenv("DOORBELL_API_BASE_URL");
        const std::string api_base = api_base_env && api_base_env[0] != '\0'
            ? api_base_env
            : "https://smartdoorbell.site/api";
        ctx.event_processor = std::make_unique<DeviceEventProcessor>(
            provisioning.device_id(),
            provisioning.device_secret(),
            api_base,
            "/userdata/doorbell",
            ctx.record_dir,
            [&provisioning](const std::string &payload) { return provisioning.publish_event(payload); });
        std::string event_error;
        if (!ctx.event_processor->start(&event_error)) {
            std::fprintf(stderr, "[event] processor disabled error=%s\n", event_error.c_str());
            ctx.event_processor.reset();
        } else {
            ctx.event_processor->set_ring_publisher(
                [&provisioning](const std::string &payload) {
                    return provisioning.publish_ring_press(payload);
                });
            ctx.event_processor->set_ring_lifecycle_handler(
                [&ctx](const std::string &event_id, const std::string &state) {
                    bool applied = false;
                    {
                        std::lock_guard<std::mutex> lock(ctx.record_mtx);
                        if (state == "ended") {
                            if (ctx.active_ring_event_id == event_id) {
                                ctx.active_ring_event_id.clear();
                                ctx.ring_event_open = false;
                                applied = true;
                            }
                        } else {
                            ctx.active_ring_event_id = event_id;
                            ctx.ring_event_open = true;
                            applied = true;
                        }
                        if (applied) {
                            ++ctx.record_frame_gen;
                        }
                    }
                    if (applied) ctx.record_cv.notify_all();
                    std::fprintf(stdout,
                                 "[ring] local lifecycle event_id=%s state=%s applied=%d\n",
                                 event_id.c_str(),
                                 state.c_str(),
                                 applied ? 1 : 0);
                });
            provisioning.set_ring_button_handler([&ctx] {
                if (ctx.doorbell_chime) {
                    ctx.doorbell_chime->play_once();
                }
                if (!ctx.event_processor) {
                    std::fprintf(stderr, "[ring] ignore short press: event processor unavailable\n");
                    return;
                }
                const auto pressed_at = std::chrono::system_clock::now();
                std::string recording_id;
                {
                    std::lock_guard<std::mutex> lock(ctx.record_mtx);
                    ctx.ring_press_in_progress = true;
                    if (!ctx.recording_stop_in_progress) {
                        recording_id = !ctx.current_recording_id.empty()
                            ? ctx.current_recording_id
                            : ctx.last_recording_attempt_id;
                    }
                }

                std::string error;
                auto press = ctx.event_processor->begin_ring_press(
                    pressed_at, recording_id, &error);
                {
                    std::lock_guard<std::mutex> lock(ctx.record_mtx);
                    ctx.ring_press_in_progress = false;
                    ++ctx.record_frame_gen;
                }
                ctx.record_cv.notify_all();
                if (!press) {
                    std::fprintf(stderr,
                                 "[ring] persist short press failed recording_id=%s error=%s\n",
                                 recording_id.c_str(),
                                 error.c_str());
                    return;
                }
                if (press->initial) {
                    if (recording_id.empty()) {
                        {
                            std::lock_guard<std::mutex> lock(ctx.record_mtx);
                            ctx.pending_ring_recording_id =
                                press->event.recording_id;
                            ctx.pending_ring_recording_started_at = pressed_at;
                            ctx.last_recording_attempt_id =
                                press->event.recording_id;
                            ++ctx.record_frame_gen;
                        }
                        ctx.record_cv.notify_all();
                    }
                    {
                        std::lock_guard<std::mutex> lock(ctx.ring_snapshot_mtx);
                        ctx.pending_ring_snapshot_event = press->event;
                        ctx.ring_snapshot_last_sample_ms = 0;
                        ctx.ring_snapshot_candidate_selector.start(
                            std::chrono::steady_clock::now());
                    }
                    std::fprintf(stdout,
                                 "[ring] one-second snapshot selection started "
                                 "event_id=%s\n",
                                 press->event.event_id.c_str());
                }
                std::fprintf(stdout,
                             "[ring] short press persisted event_id=%s press_seq=%d initial=%d "
                             "recording_id=%s state=%s\n",
                             press->event.event_id.c_str(),
                             press->press_seq,
                             press->initial ? 1 : 0,
                             press->event.recording_id.c_str(),
                             press->ring_state.c_str());
            });
            provisioning.set_event_report_ack_handler([&ctx](const std::string &payload) {
                if (ctx.event_processor) ctx.event_processor->handle_report_ack(payload);
            });
            provisioning.set_event_media_command_handler(
                [&ctx, &provisioning](const std::string &payload) {
                    if (!ctx.event_processor) return false;
                    return ctx.event_processor->handle_media_command(
                        payload,
                        [&provisioning](
                            const std::string &trace_id,
                            const std::string &cmd_id,
                            bool ok,
                            const std::string &error_code,
                            const std::string &data_json) {
                            return provisioning.publish_command_ack(
                                trace_id, cmd_id, ok, error_code, data_json);
                        });
                });
        }
    }

    std::string yolo_model = select_model_path(model_arg);
    ctx.detector_ready = ctx.detector.load(yolo_model);
    if (!ctx.detector_ready) {
        std::fprintf(stderr, "[yolo] detector disabled, model not loaded\n");
    }

    ctx.infer_thread = std::thread(inference_worker, &ctx);
    ctx.record_thread = std::thread(recorder_worker, &ctx);

    SettingsHooks hooks{.user_ctx = &ctx, .restart_camera = restart_camera};
    settings_init(lv_screen_active(), hooks);

    lv_timer_create(camera_timer_cb, 16, &ctx);
    std::fprintf(stdout, "[main] timer created, entering loop\n");

    uint64_t last_tick_ns = monotonic_time_ns();
    uint64_t tick_remainder_ns = 0;
    while (!g_stop.load()) {
		uint64_t handler_begin_ns = monotonic_time_ns();
        lv_timer_handler();
		uint64_t handler_end_ns = monotonic_time_ns();
		perf_logger_log("[main loop] lv_timer_handler timer_handler_ms=%.3f\n",
					static_cast<double>(handler_end_ns - handler_begin_ns) / 1000000.0);
        // usleep(2000);
        uint64_t now_ns = monotonic_time_ns();
        uint64_t delta_ns = now_ns - last_tick_ns;
        last_tick_ns = now_ns;

        tick_remainder_ns += delta_ns;
        uint32_t delta_ms = static_cast<uint32_t>(tick_remainder_ns / 1000000ULL);
        if (delta_ms > 0) {
            lv_tick_inc(delta_ms);
			perf_logger_log("[main loop] lv_tick_inc delta_ms=%u\n", delta_ms);
            tick_remainder_ns -= static_cast<uint64_t>(delta_ms) * 1000000ULL;
        }
    }

    {
        std::lock_guard<std::mutex> infer_lock(ctx.infer_mtx);
        ctx.infer_exit = true;
    }
    ctx.infer_cv.notify_all();
    {
        std::lock_guard<std::mutex> record_lock(ctx.record_mtx);
        ctx.record_exit = true;
    }
    ctx.record_cv.notify_all();
    if (ctx.infer_thread.joinable()) {
        ctx.infer_thread.join();
    }
    if (ctx.record_thread.joinable()) {
        ctx.record_thread.join();
    }
    provisioning.set_event_report_ack_handler({});
    provisioning.set_event_media_command_handler({});
    if (ctx.event_processor) ctx.event_processor->stop();
    close_perf_log_file();
    provisioning.stop();
    if (ctx.doorbell_chime) ctx.doorbell_chime->stop();
    ctx.audio_capture.stop();
    ctx.camera->stop();
    delete ctx.camera;
    return 0;
}
