#include "device/time_sync_service.h"

#include <array>
#include <cmath>
#include <ctime>
#include <iomanip>
#include <sstream>

#ifndef _WIN32
#include <time.h>
#endif

namespace {

std::string format_utc_millis(std::int64_t unix_ms) {
    const std::time_t seconds = static_cast<std::time_t>(unix_ms / 1000);
    std::tm utc{};
#ifdef _WIN32
    gmtime_s(&utc, &seconds);
#else
    gmtime_r(&seconds, &utc);
#endif
    std::ostringstream output;
    output << std::put_time(&utc, "%Y-%m-%dT%H:%M:%S") << '.'
           << std::setw(3) << std::setfill('0') << std::llabs(unix_ms % 1000) << 'Z';
    return output.str();
}

}  // namespace

TimeSyncService::TimeSyncService(std::vector<std::string> servers,
                                 QueryFunction query,
                                 SetClockFunction set_clock,
                                 NowFunction now,
                                 StatusCallback status_callback)
    : servers_(std::move(servers)),
      query_(std::move(query)),
      set_clock_(std::move(set_clock)),
      now_(std::move(now)),
      status_callback_(std::move(status_callback)) {}

TimeSyncService::~TimeSyncService() {
    stop();
}

void TimeSyncService::start() {
    if (worker_.joinable()) return;
    stop_.store(false);
    worker_ = std::thread(&TimeSyncService::worker_loop, this);
}

void TimeSyncService::stop() {
    stop_.store(true);
    cv_.notify_all();
    if (worker_.joinable()) worker_.join();
}

void TimeSyncService::notify_network_online() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        immediate_ = true;
    }
    cv_.notify_all();
}

DeviceTimeSyncStatus TimeSyncService::status() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return status_;
}

std::chrono::milliseconds TimeSyncService::success_interval() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::hours(6));
}

std::chrono::milliseconds TimeSyncService::failure_delay(std::size_t failure_count) {
    static constexpr std::array<std::chrono::seconds, 5> delays = {
        std::chrono::seconds(30),
        std::chrono::minutes(1),
        std::chrono::minutes(5),
        std::chrono::minutes(15),
        std::chrono::minutes(30),
    };
    const auto index = failure_count < delays.size() ? failure_count : delays.size() - 1;
    return std::chrono::duration_cast<std::chrono::milliseconds>(delays[index]);
}

bool TimeSyncService::requires_clock_update(std::int64_t offset_ms) {
    return std::llabs(offset_ms) > 1000;
}

bool TimeSyncService::set_system_clock(std::int64_t target_unix_ms) {
#ifdef _WIN32
    (void)target_unix_ms;
    return false;
#else
    timespec value{};
    value.tv_sec = static_cast<time_t>(target_unix_ms / 1000);
    value.tv_nsec = static_cast<long>((target_unix_ms % 1000) * 1000000);
    return clock_settime(CLOCK_REALTIME, &value) == 0;
#endif
}

std::int64_t TimeSyncService::system_now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

void TimeSyncService::worker_loop() {
    std::size_t failure_count = 0;
    std::unique_lock<std::mutex> lock(mutex_);
    cv_.wait(lock, [this] { return stop_.load() || immediate_; });
    while (!stop_.load()) {
        immediate_ = false;
        lock.unlock();

        const bool success = synchronize_once();
        const auto delay = success ? success_interval() : failure_delay(failure_count++);
        if (success) failure_count = 0;

        lock.lock();
        cv_.wait_for(lock, delay, [this] { return stop_.load() || immediate_; });
    }
}

bool TimeSyncService::synchronize_once() {
    publish_status("syncing", status().last_success_at, status().last_offset_ms);
    for (const auto &server : servers_) {
        const auto result = query_(server);
        if (!result.ok) continue;

        const auto measured_at = now_();
        if (requires_clock_update(result.offset_ms) && !set_clock_(measured_at + result.offset_ms)) {
            publish_status("failed", status().last_success_at, result.offset_ms);
            return false;
        }
        const auto success_at = measured_at + (requires_clock_update(result.offset_ms) ? result.offset_ms : 0);
        publish_status("synced", format_utc_millis(success_at), result.offset_ms);
        return true;
    }
    publish_status("failed", status().last_success_at, status().last_offset_ms);
    return false;
}

void TimeSyncService::publish_status(const std::string &state,
                                     std::optional<std::string> success_at,
                                     std::optional<std::int64_t> offset_ms) {
    DeviceTimeSyncStatus snapshot;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        status_.state = state;
        status_.last_success_at = std::move(success_at);
        status_.last_offset_ms = offset_ms;
        snapshot = status_;
    }
    if (status_callback_) status_callback_(snapshot);
}
