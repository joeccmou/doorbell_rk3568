#include "events/device_event_processor.h"

#include "events/event_paths.h"
#include "utils/file_sha256.h"

#include <unistd.h>

#include <chrono>
#include <algorithm>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <iomanip>
#include <nlohmann/json.hpp>
#include <sstream>
#include <utility>

namespace {
std::string iso_utc_millis(int64_t epoch_ms) {
    const std::time_t seconds = static_cast<std::time_t>(epoch_ms / 1000);
    const int millis = static_cast<int>(epoch_ms % 1000);
    std::tm utc{};
#ifdef _WIN32
    gmtime_s(&utc, &seconds);
#else
    gmtime_r(&seconds, &utc);
#endif
    char value[24]{};
    std::strftime(value, sizeof(value), "%Y-%m-%dT%H:%M:%S", &utc);
    std::ostringstream out;
    out << value << '.' << std::setw(3) << std::setfill('0') << millis << 'Z';
    return out.str();
}
}

DeviceEventProcessor::DeviceEventProcessor(std::string device_id,
                                           std::string device_secret,
                                           std::string api_base_url,
                                           std::string data_dir,
                                           std::string media_root,
                                           Publisher publisher)
    : device_id_(std::move(device_id)),
      data_dir_(std::move(data_dir)),
      media_root_(std::move(media_root)),
      publisher_(std::move(publisher)),
      uploader_(api_base_url, device_id_, device_secret),
      clip_uploader_(std::move(api_base_url), device_id_, std::move(device_secret)) {}

DeviceEventProcessor::~DeviceEventProcessor() {
    stop();
}

bool DeviceEventProcessor::start(std::string *error) {
    if (worker_.joinable()) return true;
    if (!store_.open(data_dir_ + "/doorbell.db", error)) return false;
    std::string recovered_ring_event_id;
    if (!store_.recover_stale_accepted_ring(&recovered_ring_event_id, error)) {
        store_.close();
        return false;
    }
    if (!recovered_ring_event_id.empty()) {
        std::fprintf(
            stdout,
            "[ring] startup recovered stale accepted event_id=%s state=ended "
            "end_reason=media_failed recovery_reason=process_restart\n",
            recovered_ring_event_id.c_str());
    }
    std::error_code ec;
    std::filesystem::create_directories(media_root_ + "/snapshots", ec);
    std::filesystem::create_directories(media_root_ + "/clips", ec);
    media_available_ = !ec && ::access(media_root_.c_str(), W_OK) == 0 &&
                       media_root_ != "/tmp" && media_root_.rfind("/tmp/", 0) != 0;
    stop_.store(false);
    for (auto &event : store_.pending_events(error)) {
        Job job;
        job.kind = Job::Kind::ReportEvent;
        job.event = std::move(event);
        enqueue(std::move(job));
    }
    for (auto &press : store_.pending_ring_presses(error)) {
        Job job;
        job.kind = Job::Kind::ReportRingPress;
        job.ring_press = std::move(press);
        enqueue(std::move(job));
    }
    worker_ = std::thread(&DeviceEventProcessor::worker_loop, this);
    report_worker_ =
        std::thread(&DeviceEventProcessor::report_worker_loop, this);
    return true;
}

void DeviceEventProcessor::set_ring_publisher(RingPublisher publisher) {
    std::lock_guard<std::mutex> lock(mutex_);
    ring_publisher_ = std::move(publisher);
}

void DeviceEventProcessor::set_ring_lifecycle_handler(RingLifecycleHandler handler) {
    std::lock_guard<std::mutex> lock(mutex_);
    ring_lifecycle_handler_ = std::move(handler);
}

std::optional<RingPressRecord> DeviceEventProcessor::begin_ring_press(
    std::chrono::system_clock::time_point at,
    const std::string &recording_id,
    std::string *error) {
    expire_ring_if_needed(at);
    auto press = store_.record_ring_press(
        device_id_, recording_id, at, snapshot_relative_path(at), error);
    if (!press) return std::nullopt;

    Job job;
    if (press->initial) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            pending_ring_snapshots_.insert(press->event.event_id);
        }
        job.kind = Job::Kind::PrepareRingEvent;
        job.event = press->event;
        // 1 秒选帧窗口结束后给主画面线程一个调度余量，再无结果才无图上报。
        job.ready_at =
            std::chrono::steady_clock::now() + std::chrono::milliseconds(1100);
        job.ring_snapshot_timeout = true;
    } else {
        job.kind = Job::Kind::ReportRingPress;
        job.ring_press = *press;
    }
    enqueue(std::move(job));

    RingLifecycleHandler handler;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        handler = ring_lifecycle_handler_;
    }
    if (handler) handler(press->event.event_id, press->ring_state);
    return press;
}

void DeviceEventProcessor::stop() {
    stop_.store(true);
    cv_.notify_all();
    if (worker_.joinable()) worker_.join();
    if (report_worker_.joinable()) report_worker_.join();
    store_.close();
}

std::optional<EventRecord> DeviceEventProcessor::begin_person_event(
    double confidence,
    std::chrono::system_clock::time_point at,
    const std::string &recording_id,
    std::string *error) {
    auto event = store_.create_person_event(
        device_id_, recording_id, at, snapshot_relative_path(at), confidence, error);
    if (event) {
        Job job;
        job.kind = Job::Kind::ReportEvent;
        job.event = *event;
        enqueue(std::move(job));
    }
    return event;
}

std::optional<std::string> DeviceEventProcessor::register_started_recording(
    std::chrono::system_clock::time_point started_at,
    std::string *error) {
    if (!media_available_) {
        if (error) *error = "SD media root is not writable";
        return std::nullopt;
    }
    const int64_t started_at_ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(started_at.time_since_epoch()).count();
    const std::string recording_id = device_id_ + "-" + std::to_string(started_at_ms);
    if (!store_.begin_recording(recording_id, started_at, error)) return std::nullopt;
    return recording_id;
}

std::optional<std::string> DeviceEventProcessor::register_failed_ring_recording(
    std::chrono::system_clock::time_point started_at,
    std::string *error) {
    const int64_t started_at_ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(
            started_at.time_since_epoch()).count();
    const std::string recording_id = device_id_ + "-" + std::to_string(started_at_ms);
    if (!store_.begin_recording(recording_id, started_at, error)) return std::nullopt;
    if (!store_.finalize_recording(recording_id, started_at, "failed", error)) {
        return std::nullopt;
    }
    return recording_id;
}

std::optional<std::string> DeviceEventProcessor::allocate_recording_clip(
    std::chrono::system_clock::time_point at,
    std::string *error) {
    if (!media_available_) {
        if (error) *error = "SD media root is not writable";
        return std::nullopt;
    }
    return store_.allocate_clip_ref(at, media_root_, error);
}

void DeviceEventProcessor::submit_completed_recording_segment(
    const std::string &recording_id,
    int segment_index,
    const std::string &temporary_file,
    const std::string &clip_ref,
    std::chrono::system_clock::time_point started_at,
    std::chrono::system_clock::time_point ended_at) {
    Job job;
    job.kind = Job::Kind::PersistRecordingSegment;
    job.recording_id = recording_id;
    job.segment_index = segment_index;
    job.temporary_file = temporary_file;
    job.clip_ref = clip_ref;
    job.started_at = started_at;
    job.ended_at = ended_at;
    enqueue(std::move(job));
}

bool DeviceEventProcessor::finalize_recording(
    const std::string &recording_id,
    std::chrono::system_clock::time_point ended_at,
    const std::string &status,
    std::string *error) {
    if (status != "finalized" && status != "interrupted" && status != "failed") {
        if (error) *error = "invalid terminal recording status";
        return false;
    }
    if (!worker_.joinable()) {
        if (error) *error = "event processor worker is not running";
        return false;
    }
    auto completion = std::make_shared<std::promise<bool>>();
    auto result = completion->get_future();
    Job job;
    job.kind = Job::Kind::FinalizeRecording;
    job.recording_id = recording_id;
    job.ended_at = ended_at;
    job.recording_status = status;
    job.completion = std::move(completion);
    enqueue(std::move(job));
    if (result.wait_for(std::chrono::seconds(30)) != std::future_status::ready) {
        if (error) *error = "timed out waiting for recording finalization";
        return false;
    }
    const bool ok = result.get();
    if (!ok && error && error->empty()) {
        *error = "recording finalization failed";
    }
    return ok;
}

void DeviceEventProcessor::submit_person_snapshot(EventRecord event,
                                                  std::vector<uint8_t> rgb,
                                                  uint32_t width,
                                                  uint32_t height) {
    Job job;
    job.kind = Job::Kind::SaveSnapshot;
    job.event = std::move(event);
    job.rgb = std::move(rgb);
    job.width = width;
    job.height = height;
    enqueue(std::move(job));
}

void DeviceEventProcessor::submit_ring_snapshot(EventRecord event,
                                                std::vector<uint8_t> rgb,
                                                uint32_t width,
                                                uint32_t height) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (pending_ring_snapshots_.erase(event.event_id) == 0) {
            return;
        }
    }
    Job job;
    job.kind = Job::Kind::PrepareRingEvent;
    job.event = std::move(event);
    job.rgb = std::move(rgb);
    job.width = width;
    job.height = height;
    enqueue(std::move(job));
}

void DeviceEventProcessor::enqueue(Job job) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (job.kind == Job::Kind::ReportEvent ||
            job.kind == Job::Kind::ReportRingPress) {
            report_jobs_.push_back(std::move(job));
        } else if (job.kind == Job::Kind::PersistRecordingSegment ||
            job.kind == Job::Kind::FinalizeRecording) {
            recording_jobs_.push_back(std::move(job));
        } else {
            jobs_.push_back(std::move(job));
        }
    }
    cv_.notify_all();
}

void DeviceEventProcessor::worker_loop() {
    while (!stop_.load()) {
        expire_ring_if_needed(std::chrono::system_clock::now());
        Job job;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            while (!stop_.load()) {
                if (!recording_jobs_.empty()) {
                    job = std::move(recording_jobs_.front());
                    recording_jobs_.pop_front();
                    break;
                }
                const auto now = std::chrono::steady_clock::now();
                auto ready = jobs_.end();
                auto next_ready = std::chrono::steady_clock::time_point::max();
                for (auto it = jobs_.begin(); it != jobs_.end(); ++it) {
                    if (it->ready_at.time_since_epoch().count() == 0 ||
                        it->ready_at <= now) {
                        ready = it;
                        break;
                    }
                    next_ready = std::min(next_ready, it->ready_at);
                }
                if (ready != jobs_.end()) {
                    job = std::move(*ready);
                    jobs_.erase(ready);
                    break;
                }
                if (next_ready == std::chrono::steady_clock::time_point::max()) {
                    cv_.wait_for(lock, std::chrono::seconds(1));
                } else {
                    cv_.wait_until(lock, next_ready);
                }
            }
            if (stop_.load()) break;
        }
        if (!process(&job) && !stop_.load()) {
            // 失败任务只延迟自身，不能让等待业务 ACK 的 5 秒退避饿死抓拍等其他任务。
            job.ready_at = std::chrono::steady_clock::now() + std::chrono::seconds(5);
            enqueue(std::move(job));
        }
    }
}

void DeviceEventProcessor::report_worker_loop() {
    while (!stop_.load()) {
        Job job;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            while (!stop_.load()) {
                const auto now = std::chrono::steady_clock::now();
                auto ready = report_jobs_.end();
                auto next_ready = std::chrono::steady_clock::time_point::max();
                for (auto it = report_jobs_.begin(); it != report_jobs_.end(); ++it) {
                    if (it->ready_at.time_since_epoch().count() == 0 ||
                        it->ready_at <= now) {
                        ready = it;
                        break;
                    }
                    next_ready = std::min(next_ready, it->ready_at);
                }
                if (ready != report_jobs_.end()) {
                    job = std::move(*ready);
                    report_jobs_.erase(ready);
                    break;
                }
                if (next_ready == std::chrono::steady_clock::time_point::max()) {
                    cv_.wait_for(lock, std::chrono::seconds(1));
                } else {
                    cv_.wait_until(lock, next_ready);
                }
            }
            if (stop_.load()) break;
        }
        if (!process(&job) && !stop_.load()) {
            job.ready_at =
                std::chrono::steady_clock::now() + std::chrono::seconds(5);
            enqueue(std::move(job));
        }
    }
}

void DeviceEventProcessor::expire_ring_if_needed(
    std::chrono::system_clock::time_point at) {
    std::string error;
    const auto event_id = store_.expire_open_ring(at, &error);
    if (!event_id) {
        if (!error.empty()) {
            std::fprintf(stderr, "[ring] local expiry check failed error=%s\n", error.c_str());
        }
        return;
    }
    RingLifecycleHandler handler;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        handler = ring_lifecycle_handler_;
    }
    if (handler) handler(*event_id, "ended");
    std::fprintf(stdout,
                 "[ring] local deadline ended event_id=%s\n",
                 event_id->c_str());
}

bool DeviceEventProcessor::process(Job *job) {
    if (!job) return false;
    if (job->kind == Job::Kind::PersistRecordingSegment) {
        auto fail_segment = [this, job](const std::string &message) {
            failed_recordings_.insert(job->recording_id);
            std::fprintf(stderr,
                         "[record] persist segment failed recording_id=%s segment_index=%d error=%s\n",
                         job->recording_id.c_str(),
                         job->segment_index,
                         message.c_str());
            return true;
        };
        if (job->recording_id.empty() || job->segment_index <= 0 ||
            job->temporary_file.empty() || !is_canonical_clip_ref(job->clip_ref)) {
            return fail_segment("invalid completed segment metadata");
        }

        const std::filesystem::path temporary_file(job->temporary_file);
        const std::filesystem::path final_file = std::filesystem::path(media_root_) / job->clip_ref;
        if (temporary_file.parent_path().lexically_normal() !=
            final_file.parent_path().lexically_normal()) {
            return fail_segment("temporary and final segment paths must share one directory");
        }
        std::error_code file_error;
        const bool final_file_exists = std::filesystem::exists(final_file, file_error);
        if (file_error) return fail_segment(file_error.message());
        if (final_file_exists) return fail_segment("final segment path already exists");
        std::filesystem::rename(temporary_file, final_file, file_error);
        if (file_error) {
            return fail_segment(std::string("publish completed MP4 failed: ") + file_error.message());
        }
        const int64_t size_bytes = static_cast<int64_t>(
            std::filesystem::file_size(final_file, file_error));
        if (file_error) return fail_segment(file_error.message());
        std::string sha256;
        std::string error;
        if (!file_sha256(final_file.string(), &sha256, &error)) {
            return fail_segment(error);
        }
        const int64_t media_duration_ms = std::max<int64_t>(
            0,
            std::chrono::duration_cast<std::chrono::milliseconds>(
                job->ended_at - job->started_at).count());
        std::ostringstream segment_id;
        segment_id << job->recording_id << '-'
                   << std::setw(6) << std::setfill('0') << job->segment_index;
        if (!store_.append_recording_segment(
                job->recording_id,
                segment_id.str(),
                job->segment_index,
                job->clip_ref,
                job->started_at,
                job->ended_at,
                media_duration_ms,
                size_bytes,
                sha256,
                &error)) {
            return fail_segment(error);
        }
        std::fprintf(stdout,
                     "[record] segment committed recording_id=%s segment_index=%d duration_ms=%lld bytes=%lld\n",
                     job->recording_id.c_str(),
                     job->segment_index,
                     static_cast<long long>(media_duration_ms),
                     static_cast<long long>(size_bytes));
        return true;
    }

    if (job->kind == Job::Kind::FinalizeRecording) {
        std::string status = job->recording_status;
        std::string error;
        const auto recording = store_.find_recording(job->recording_id, &error);
        if (!recording) {
            std::fprintf(stderr,
                         "[record] read recording before finalization failed recording_id=%s error=%s\n",
                         job->recording_id.c_str(),
                         error.c_str());
            return false;
        }
        if (recording->segment_count == 0 || failed_recordings_.count(job->recording_id) != 0) {
            status = "failed";
        }
        if (!store_.finalize_recording(job->recording_id, job->ended_at, status, &error)) {
            std::fprintf(stderr,
                         "[record] finalize recording failed recording_id=%s error=%s\n",
                         job->recording_id.c_str(),
                         error.c_str());
            return false;
        }
        failed_recordings_.erase(job->recording_id);
        std::fprintf(stdout,
                     "[record] recording finalized recording_id=%s status=%s segments=%d\n",
                     job->recording_id.c_str(),
                     status.c_str(),
                     recording->segment_count);
        if (job->completion) job->completion->set_value(true);
        return true;
    }

    if (job->kind == Job::Kind::SaveSnapshot) {
        const std::string snapshot_file = media_root_ + "/" + job->event.snapshot_path;
        if (!std::filesystem::exists(snapshot_file) && !job->rgb.empty()) {
            std::string error;
            if (!snapshots_.save_rgb_jpeg(
                    snapshot_file, job->rgb, job->width, job->height, &error)) {
                // 抓拍失败不得阻断真实事件上报。
                std::fprintf(stderr,
                             "[event] save snapshot failed event_id=%s error=%s\n",
                             job->event.event_id.c_str(),
                             error.c_str());
                return true;
            }
        }
        std::fprintf(stdout,
                     "[event] snapshot saved locally event_id=%s path=%s\n",
                     job->event.event_id.c_str(),
                     job->event.snapshot_path.c_str());
        return true;
    }

    if (job->kind == Job::Kind::PrepareRingEvent) {
        if (job->ring_snapshot_timeout) {
            std::lock_guard<std::mutex> lock(mutex_);
            if (pending_ring_snapshots_.erase(job->event.event_id) == 0) {
                return true;
            }
        }

        const std::string snapshot_file = media_root_ + "/" + job->event.snapshot_path;
        bool snapshot_saved = false;
        std::string error;
        if (!job->rgb.empty()) {
            snapshot_saved = snapshots_.save_rgb_jpeg(
                snapshot_file, job->rgb, job->width, job->height, &error);
            if (!snapshot_saved) {
                std::fprintf(stderr,
                             "[ring] save selected snapshot failed event_id=%s error=%s\n",
                             job->event.event_id.c_str(),
                             error.c_str());
            }
        }
        if (snapshot_saved) {
            std::string snapshot_url;
            error.clear();
            if (uploader_.upload(
                    snapshot_file,
                    job->event.snapshot_path,
                    &snapshot_url,
                    &error,
                    2000L)) {
                if (store_.update_event_snapshot_url(
                        job->event.event_id, snapshot_url, &error)) {
                    job->event.snapshot_url = snapshot_url;
                    std::fprintf(stdout,
                                 "[ring] snapshot pre-uploaded event_id=%s url=%s\n",
                                 job->event.event_id.c_str(),
                                 snapshot_url.c_str());
                } else {
                    std::fprintf(stderr,
                                 "[ring] persist snapshot URL failed event_id=%s error=%s\n",
                                 job->event.event_id.c_str(),
                                 error.c_str());
                }
            } else {
                std::fprintf(stderr,
                             "[ring] snapshot pre-upload failed event_id=%s error=%s; "
                             "continue reporting without URL\n",
                             job->event.event_id.c_str(),
                             error.c_str());
            }
        } else if (job->ring_snapshot_timeout) {
            std::fprintf(stderr,
                         "[ring] one-second snapshot window expired without frame event_id=%s\n",
                         job->event.event_id.c_str());
        }

        Job report;
        report.kind = Job::Kind::ReportEvent;
        report.event = std::move(job->event);
        enqueue(std::move(report));
        return true;
    }

    if (job->kind == Job::Kind::ReportRingPress) {
        RingPublisher publisher;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            publisher = ring_publisher_;
        }
        const nlohmann::json payload{
            {"trace_id", "tr_ring_" + job->ring_press.event.event_id + "_" +
                             std::to_string(job->ring_press.press_seq)},
            {"device_id", job->ring_press.event.device_id},
            {"event_id", job->ring_press.event.event_id},
            {"press_seq", job->ring_press.press_seq},
            {"pressed_at", iso_utc_millis(job->ring_press.pressed_at_ms)},
            {"ts", iso_utc_millis(
                       std::chrono::duration_cast<std::chrono::milliseconds>(
                           std::chrono::system_clock::now().time_since_epoch()).count())},
        };
        if (!publisher || !publisher(payload.dump())) return false;
        std::string error;
        if (!store_.mark_ring_press_reported(
                job->ring_press.event.event_id,
                job->ring_press.press_seq,
                &error)) {
            std::fprintf(stderr,
                         "[ring] persist queued ring_press failed event_id=%s press_seq=%d error=%s\n",
                         job->ring_press.event.event_id.c_str(),
                         job->ring_press.press_seq,
                         error.c_str());
            return false;
        }
        std::fprintf(stdout,
                     "[ring] queued ring_press publish event_id=%s press_seq=%d\n",
                     job->ring_press.event.event_id.c_str(),
                     job->ring_press.press_seq);
        return true;
    }

    std::string state_error;
    const auto state = store_.report_state(job->event.event_id, &state_error);
    if (!state) {
        std::fprintf(stderr,
                     "[event] read report state failed event_id=%s error=%s\n",
                     job->event.event_id.c_str(),
                     state_error.c_str());
        return false;
    }
    if (*state != 0) return true;

    nlohmann::json extra = nlohmann::json::object();
    try {
        extra = nlohmann::json::parse(job->event.extra_json);
    } catch (const std::exception &) {
    }
    nlohmann::json payload{
        {"trace_id", "tr_" + job->event.event_id},
        {"event_id", job->event.event_id},
        {"device_id", job->event.device_id},
        {"recording_id", job->event.recording_id},
        {"type", job->event.type},
        {"at", iso_utc_millis(job->event.at_ms)},
        {"occurred_timezone", job->event.occurred_timezone},
        {"occurred_utc_offset_minutes", job->event.occurred_utc_offset_minutes},
        {"occurred_local_date", job->event.occurred_local_date},
        {"extra", extra},
    };
    if (!job->event.snapshot_url.empty()) {
        payload["snapshot_url"] = job->event.snapshot_url;
    }
    if (!publisher_ || !publisher_(payload.dump())) return false;
    std::fprintf(stdout,
                 "[event] published pending event_id=%s; waiting business ACK\n",
                 job->event.event_id.c_str());
    // MQTT PUBACK 不是业务成功；保持 pending 并按退避复报。
    return false;
}

void DeviceEventProcessor::handle_report_ack(const std::string &payload) {
    try {
        const auto ack = nlohmann::json::parse(payload);
        if (ack.value("kind", "") != "event" ||
            ack.value("device_id", "") != device_id_) {
            return;
        }
        const std::string event_id = ack.value("event_id", "");
        if (event_id.empty()) return;
        std::string error;
        if (ack.value("ok", false)) {
            if (!store_.mark_reported(event_id, &error)) {
                std::fprintf(stderr,
                             "[event] persist success ACK failed event_id=%s error=%s\n",
                             event_id.c_str(),
                             error.c_str());
                return;
            }
            std::fprintf(stdout, "[event] business ACK committed event_id=%s\n", event_id.c_str());
            return;
        }
        if (!ack.value("retryable", true)) {
            if (!store_.mark_unreportable(event_id, &error)) {
                std::fprintf(stderr,
                             "[event] persist permanent rejection failed event_id=%s error=%s\n",
                             event_id.c_str(),
                             error.c_str());
                return;
            }
            const std::string error_code = ack.value("error_code", "");
            std::fprintf(stderr,
                         "[event] permanently rejected event_id=%s error_code=%s\n",
                         event_id.c_str(),
                         error_code.c_str());
        }
    } catch (const std::exception &exception) {
        std::fprintf(stderr, "[event] invalid report_ack error=%s\n", exception.what());
    }
}

bool DeviceEventProcessor::handle_media_command(
    const std::string &payload,
    const CommandAckPublisher &ack_publisher) {
    try {
        const auto command = nlohmann::json::parse(payload);
        const std::string action = command.value("action", "");
        if (action == "respond_ring" || action == "close_ring") {
            const std::string trace_id = command.value("trace_id", "");
            const std::string cmd_id = command.value("cmd_id", "");
            const std::string call_id = command.value("call_id", "");
            const auto params = command.value("params", nlohmann::json::object());
            const std::string ring_event_id = params.value("ring_event_id", "");
            const std::string event_id = params.value("event_id", "");
            if (trace_id.empty() || cmd_id.empty() || event_id.empty() || !ack_publisher) {
                std::fprintf(stderr,
                             "[ring] invalid command action=%s trace_id=%s cmd_id=%s "
                             "call_id=%s ring_event_id=%s event_id=%s error_code=INVALID_COMMAND\n",
                             action.c_str(),
                             trace_id.c_str(),
                             cmd_id.c_str(),
                             call_id.c_str(),
                             ring_event_id.c_str(),
                             event_id.c_str());
                return true;
            }
            std::string state = "ringing";
            if (action == "close_ring") {
                state = "ended";
            } else if (params.value("decision", "") == "answer") {
                state = "accepted";
            }
            std::string error;
            if (!store_.update_ring_state(event_id, state, &error)) {
                ack_publisher(trace_id, cmd_id, false, "RING_STATE_STORE_FAILED", "{}");
                std::fprintf(stderr,
                             "[ring] update local state failed event_id=%s state=%s error=%s\n",
                             event_id.c_str(),
                             state.c_str(),
                             error.c_str());
                return true;
            }
            RingLifecycleHandler handler;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                handler = ring_lifecycle_handler_;
            }
            if (handler) handler(event_id, state);
            ack_publisher(trace_id, cmd_id, true, "", "{}");
            std::fprintf(stdout,
                         "[ring] command applied action=%s trace_id=%s call_id=%s "
                         "ring_event_id=%s event_id=%s decision=%s state=%s "
                         "result=%s end_reason=%s\n",
                         action.c_str(),
                         trace_id.c_str(),
                         call_id.c_str(),
                         ring_event_id.c_str(),
                         event_id.c_str(),
                         params.value("decision", "").c_str(),
                         state.c_str(),
                         params.value("result", "").c_str(),
                         params.value("end_reason", "").c_str());
            return true;
        }
        if (action != "query_snapshot" &&
            action != "query_recording_manifest" &&
            action != "prepare_recording_segment") {
            return false;
        }
        const std::string trace_id = command.value("trace_id", "");
        const std::string cmd_id = command.value("cmd_id", "");
        const auto params = command.value("params", nlohmann::json::object());
        if (action == "prepare_recording_segment") {
            const std::string recording_id = params.value("recording_id", "");
            const std::string segment_id = params.value("segment_id", "");
            const std::string upload_id = params.value("upload_id", "");
            if (trace_id.empty() || cmd_id.empty() || recording_id.empty() ||
                segment_id.empty() || upload_id.empty() || !ack_publisher) {
                return true;
            }
            std::string error;
            const auto segments = store_.recording_segments(recording_id, &error);
            const auto found = std::find_if(
                segments.begin(), segments.end(), [&](const auto &segment) {
                    return segment.segment_id == segment_id;
                });
            if (found == segments.end()) {
                ack_publisher(trace_id, cmd_id, false, "CLIP_NOT_FOUND", "{}");
                return true;
            }
            std::string clip_url;
            if (!clip_uploader_.upload(
                    media_root_ + "/" + found->clip_ref,
                    found->clip_ref,
                    upload_id,
                    found->sha256,
                    &clip_url,
                    &error)) {
                ack_publisher(trace_id, cmd_id, false, "CLIP_UPLOAD_FAILED", "{}");
                return true;
            }
            const nlohmann::json data{
                {"upload_id", upload_id},
                {"segment_id", segment_id},
                {"state", "uploaded"},
            };
            ack_publisher(trace_id, cmd_id, true, "", data.dump());
            return true;
        }
        if (action == "query_recording_manifest") {
            const std::string recording_id = params.value("recording_id", "");
            if (trace_id.empty() || cmd_id.empty() || recording_id.empty() || !ack_publisher) {
                return true;
            }
            std::string error;
            const auto recording = store_.find_recording(recording_id, &error);
            if (!recording) {
                ack_publisher(trace_id, cmd_id, false, "RECORDING_NOT_FOUND", "{}");
                return true;
            }
            nlohmann::json recording_json{
                {"recording_id", recording->recording_id},
                {"device_id", device_id_},
                {"status", recording->status},
                {"started_at", iso_utc_millis(recording->started_at_ms)},
                {"ended_at", recording->ended_at_ms > 0
                                 ? nlohmann::json(iso_utc_millis(recording->ended_at_ms))
                                 : nlohmann::json(nullptr)},
                {"recorded_timezone", recording->recorded_timezone},
                {"started_utc_offset_minutes", recording->started_utc_offset_minutes},
                {"started_local_date", recording->started_local_date},
                {"segment_count", recording->segment_count},
                {"total_media_duration_ms", recording->total_media_duration_ms},
                {"segments", nlohmann::json::array()},
            };
            auto segments = store_.recording_segments(recording_id, &error);
            for (auto &segment : segments) {
                if (segment.sha256.empty()) {
                    if (!file_sha256(
                            media_root_ + "/" + segment.clip_ref,
                            &segment.sha256,
                            &error) ||
                        !store_.update_segment_sha256(
                            segment.segment_id,
                            segment.sha256,
                            &error)) {
                        ack_publisher(
                            trace_id,
                            cmd_id,
                            false,
                            "RECORDING_CHECKSUM_UNAVAILABLE",
                            "{}");
                        return true;
                    }
                }
                recording_json["segments"].push_back({
                    {"segment_id", segment.segment_id},
                    {"segment_index", segment.segment_index},
                    {"clip_ref", segment.clip_ref},
                    {"started_at", iso_utc_millis(segment.started_at_ms)},
                    {"ended_at", iso_utc_millis(segment.ended_at_ms)},
                    {"duration_ms", segment.media_duration_ms},
                    {"size_bytes", segment.size_bytes},
                    {"sha256", segment.sha256},
                });
            }
            ack_publisher(
                trace_id,
                cmd_id,
                true,
                "",
                nlohmann::json{{"recording", recording_json}}.dump());
            return true;
        }
        const std::string event_id = params.value("event_id", "");
        if (trace_id.empty() || cmd_id.empty() || event_id.empty() || !ack_publisher) return true;

        std::string error;
        const auto event = store_.find_event(event_id, &error);
        if (!event) {
            ack_publisher(trace_id, cmd_id, false, "SNAPSHOT_NOT_FOUND", "{}");
            return true;
        }
        const std::string snapshot_file = media_root_ + "/" + event->snapshot_path;
        if (!std::filesystem::exists(snapshot_file)) {
            ack_publisher(trace_id, cmd_id, false, "SNAPSHOT_NOT_FOUND", "{}");
            return true;
        }
        std::string snapshot_url;
        if (!uploader_.upload(snapshot_file, event->snapshot_path, &snapshot_url, &error)) {
            ack_publisher(trace_id, cmd_id, false, "SNAPSHOT_UPLOAD_FAILED", "{}");
            return true;
        }
        const nlohmann::json data{
            {"event_id", event_id},
            {"snapshot_url", snapshot_url},
        };
        ack_publisher(trace_id, cmd_id, true, "", data.dump());
        return true;
    } catch (const std::exception &) {
        return false;
    }
}
