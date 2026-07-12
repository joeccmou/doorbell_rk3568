#pragma once

#include "device/sntp_client.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

struct DeviceTimeSyncStatus {
    std::string state = "unsynced";
    std::optional<std::string> last_success_at;
    std::optional<std::int64_t> last_offset_ms;
};

class TimeSyncService {
public:
    using QueryFunction = std::function<SntpResult(const std::string &server)>;
    using SetClockFunction = std::function<bool(std::int64_t target_unix_ms)>;
    using NowFunction = std::function<std::int64_t()>;
    using StatusCallback = std::function<void(const DeviceTimeSyncStatus &status)>;

    TimeSyncService(std::vector<std::string> servers,
                    QueryFunction query,
                    SetClockFunction set_clock,
                    NowFunction now,
                    StatusCallback status_callback);
    ~TimeSyncService();

    TimeSyncService(const TimeSyncService &) = delete;
    TimeSyncService &operator=(const TimeSyncService &) = delete;

    void start();
    void stop();
    void notify_network_online();
    DeviceTimeSyncStatus status() const;

    static std::chrono::milliseconds success_interval();
    static std::chrono::milliseconds failure_delay(std::size_t failure_count);
    static bool requires_clock_update(std::int64_t offset_ms);
    static bool set_system_clock(std::int64_t target_unix_ms);
    static std::int64_t system_now_ms();

private:
    void worker_loop();
    bool synchronize_once();
    void publish_status(const std::string &state,
                        std::optional<std::string> success_at,
                        std::optional<std::int64_t> offset_ms);

    std::vector<std::string> servers_;
    QueryFunction query_;
    SetClockFunction set_clock_;
    NowFunction now_;
    StatusCallback status_callback_;

    mutable std::mutex mutex_;
    std::condition_variable cv_;
    DeviceTimeSyncStatus status_;
    bool immediate_ = false;
    std::atomic<bool> stop_{false};
    std::thread worker_;
};
