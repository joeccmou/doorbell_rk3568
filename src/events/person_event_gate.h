#pragma once

#include <chrono>

// PersonEventGate 把逐帧检测结果收敛为“本次出现只产生一次”的业务事件。
class PersonEventGate {
public:
    explicit PersonEventGate(std::chrono::steady_clock::duration cooldown);

    // 返回 true 表示本次更新应创建一条 person 事件。
    bool update(bool has_person, std::chrono::steady_clock::time_point now);
    void reset();

private:
    std::chrono::steady_clock::duration cooldown_;
    std::chrono::steady_clock::time_point last_event_at_{};
    bool has_last_event_ = false;
    bool person_present_ = false;
    bool appearance_pending_ = false;
};
