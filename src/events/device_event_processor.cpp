#include "events/device_event_processor.h"

#include "events/event_paths.h"

#include <unistd.h>

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <iomanip>
#include <ctime>
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
      uploader_(std::move(api_base_url), device_id_, std::move(device_secret)) {}

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
    for (auto &event : store_.pending_events(error)) enqueue(Job{event});
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
    std::string *error) {
    return begin_person_event_with_clip(confidence, at, "", error);
}

std::optional<EventRecord> DeviceEventProcessor::begin_person_event_with_clip(
    double confidence,
    std::chrono::system_clock::time_point at,
    const std::string &existing_clip_ref,
    std::string *error) {
    std::string clip_ref = existing_clip_ref;
    if (clip_ref.empty() && media_available_) {
        auto allocated = store_.allocate_clip_ref(at, media_root_, error);
        if (!allocated) return std::nullopt;
        clip_ref = *allocated;
    }
    return store_.create_person_event(
        device_id_, at, snapshot_relative_path(at), clip_ref, confidence, error);
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
    enqueue(Job{std::move(event), std::move(rgb), width, height});
}

std::optional<EventRecord> DeviceEventProcessor::create_person_event(const std::vector<uint8_t> &rgb,
                                                                     uint32_t width,
                                                                     uint32_t height,
                                                                     double confidence,
                                                                     std::string *error) {
    const auto now = std::chrono::system_clock::now();
    auto event = begin_person_event(confidence, now, error);
    if (!event) return std::nullopt;
    submit_person_snapshot(*event, rgb, width, height);
    return event;
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
            // 启动时已恢复 SQLite pending；运行期新事件与失败重试都由内存队列驱动。
            // 不周期扫描数据库，避免 2 秒选帧完成前抢跑并误判抓拍缺失。
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
    const std::string snapshot_file = media_root_ + "/" + job->event.snapshot_path;
    if (!std::filesystem::exists(snapshot_file)) {
        if (job->rgb.empty()) {
            std::string error;
            if (!store_.mark_unreportable(job->event.event_id, &error)) {
                std::fprintf(stderr, "[event] snapshot missing and state update failed event_id=%s error=%s\n",
                             job->event.event_id.c_str(), error.c_str());
                return false;
            }
            std::fprintf(stderr, "[event] snapshot missing, event marked unreportable event_id=%s\n",
                         job->event.event_id.c_str());
            return true;
        }
        std::string error;
        if (!snapshots_.save_rgb_jpeg(snapshot_file, job->rgb, job->width, job->height, &error)) {
            std::fprintf(stderr, "[event] save snapshot failed event_id=%s error=%s\n", job->event.event_id.c_str(), error.c_str());
            return false;
        }
        job->rgb.clear();
    }

    if (job->event.snapshot_url.empty()) {
        std::string error;
        if (!uploader_.upload(snapshot_file, job->event.snapshot_path, &job->event.snapshot_url, &error)) {
            std::fprintf(stderr, "[event] upload snapshot failed event_id=%s error=%s\n", job->event.event_id.c_str(), error.c_str());
            return false;
        }
        if (!store_.set_snapshot_url(job->event.event_id, job->event.snapshot_url, &error)) {
            std::fprintf(stderr, "[event] persist snapshot URL failed event_id=%s error=%s\n", job->event.event_id.c_str(), error.c_str());
            return false;
        }
    }

    nlohmann::json extra = nlohmann::json::object();
    try {
        extra = nlohmann::json::parse(job->event.extra_json);
    } catch (const std::exception &) {
    }
    nlohmann::json payload{
        {"trace_id", "tr_" + job->event.event_id},
        {"event_id", job->event.event_id},
        {"device_id", job->event.device_id},
        {"type", job->event.type},
        {"at", iso_utc_millis(job->event.at_ms)},
        {"occurred_timezone", job->event.occurred_timezone},
        {"occurred_utc_offset_minutes", job->event.occurred_utc_offset_minutes},
        {"occurred_local_date", job->event.occurred_local_date},
        {"snapshot_url", job->event.snapshot_url},
        {"clip_ref", job->event.clip_ref.empty() ? nlohmann::json(nullptr) : nlohmann::json(job->event.clip_ref)},
        {"extra", extra},
    };
    if (!publisher_ || !publisher_(payload.dump())) return false;
    std::string error;
    if (!store_.mark_reported(job->event.event_id, &error)) {
        std::fprintf(stderr, "[event] mark reported failed event_id=%s error=%s\n", job->event.event_id.c_str(), error.c_str());
        return false;
    }
    std::fprintf(stdout, "[event] reported person event_id=%s\n", job->event.event_id.c_str());
    return true;
}
