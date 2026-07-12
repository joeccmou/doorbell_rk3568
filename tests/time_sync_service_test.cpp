#include "device/time_sync_service.h"

#include <cassert>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <string>

int main() {
    using namespace std::chrono_literals;
    assert(TimeSyncService::success_interval() == 6h);
    assert(TimeSyncService::failure_delay(0) == 30s);
    assert(TimeSyncService::failure_delay(1) == 1min);
    assert(TimeSyncService::failure_delay(20) == 30min);
    assert(!TimeSyncService::requires_clock_update(1000));
    assert(TimeSyncService::requires_clock_update(1001));
    assert(TimeSyncService::requires_clock_update(-1001));

    std::mutex mutex;
    std::condition_variable cv;
    bool synced = false;
    bool clock_set = false;
    std::int64_t clock_target = 0;
    int query_count = 0;

    TimeSyncService service(
        {"ntp.aliyun.com", "ntp1.aliyun.com", "ntp2.aliyun.com"},
        [&](const std::string &server) {
            ++query_count;
            assert(server == "ntp.aliyun.com");
            return SntpResult{true, 1501, server, ""};
        },
        [&](std::int64_t target_unix_ms) {
            clock_set = true;
            clock_target = target_unix_ms;
            return true;
        },
        [] { return std::int64_t{100000}; },
        [&](const DeviceTimeSyncStatus &status) {
            if (status.state == "synced") {
                std::lock_guard<std::mutex> lock(mutex);
                synced = true;
                cv.notify_all();
            }
        });

    service.start();
    service.notify_network_online();
    {
        std::unique_lock<std::mutex> lock(mutex);
        assert(cv.wait_for(lock, 2s, [&] { return synced; }));
    }
    service.stop();

    assert(query_count == 1);
    assert(clock_set);
    assert(clock_target == 101501);
    return 0;
}
