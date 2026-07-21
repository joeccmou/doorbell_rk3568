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
    worker_ = std::thread(&DeviceEventProcessor::worker_loop, this);
    return true;
}

void DeviceEventProcessor::stop() {
    stop_.store(true);
    cv_.notify_all();
    if (worker_.joinable()) worker_.join();
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
    const int64_t started_at_ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(started_at.time_since_epoch()).count();
    const std::string recording_id = device_id_ + "-" + std::to_string(started_at_ms);
    if (!store_.begin_recording(recording_id, started_at, error)) return std::nullopt;
    return recording_id;
}

bool DeviceEventProcessor::complete_recording(
    const std::string &recording_id,
    const std::string &clip_ref,
    std::chrono::system_clock::time_point started_at,
    std::chrono::system_clock::time_point ended_at,
    int64_t media_duration_ms,
    int64_t size_bytes,
    const std::string &sha256,
    const std::string &status,
    std::string *error) {
    return store_.complete_recording(
        recording_id,
        recording_id + "-000001",
        1,
        clip_ref,
        started_at,
        ended_at,
        media_duration_ms,
        size_bytes,
        sha256,
        status,
        error);
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

void DeviceEventProcessor::enqueue(Job job) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        jobs_.push_back(std::move(job));
    }
    cv_.notify_one();
}

void DeviceEventProcessor::worker_loop() {
    while (!stop_.load()) {
        Job job;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            cv_.wait(lock, [this] { return stop_.load() || !jobs_.empty(); });
            if (stop_.load()) break;
            job = std::move(jobs_.front());
            jobs_.pop_front();
        }
        if (!process(&job) && !stop_.load()) {
            std::unique_lock<std::mutex> lock(mutex_);
            cv_.wait_for(lock, std::chrono::seconds(5), [this] { return stop_.load(); });
            lock.unlock();
            if (!stop_.load()) enqueue(std::move(job));
        }
    }
}

bool DeviceEventProcessor::process(Job *job) {
    if (!job) return false;
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
    const nlohmann::json payload{
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
