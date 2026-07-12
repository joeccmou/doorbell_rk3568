#pragma once

#include <cstdint>

constexpr uint64_t kPersonRecordingTailNs = 30ULL * 1000ULL * 1000ULL * 1000ULL;

// 最后一次检测到人形后继续录像 30 秒；到达边界时停止。
inline bool person_recording_active(uint64_t last_person_ts_ns,
                                    uint64_t now_ts_ns) {
    return last_person_ts_ns != 0 &&
           now_ts_ns >= last_person_ts_ns &&
           now_ts_ns - last_person_ts_ns < kPersonRecordingTailNs;
}
