#pragma once

#include <chrono>

// PersonEventGate 把逐帧检测结果收敛为“本次出现只产生一次”的业务事件。
class PersonEventGate {
public:
    explicit PersonEventGate(std::chrono::steady_clock::duration absence_rearm_duration);

    // 返回 true 表示本次更新应创建一条 person 事件。
    bool update(bool has_person, std::chrono::steady_clock::time_point now);
    void reset();

private:
    std::chrono::steady_clock::duration absence_rearm_duration_;
    std::chrono::steady_clock::time_point absence_started_at_{};
    bool has_active_appearance_ = false;
    bool absence_pending_ = false;
};
