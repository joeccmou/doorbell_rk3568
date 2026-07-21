#include "events/person_event_gate.h"

PersonEventGate::PersonEventGate(std::chrono::steady_clock::duration absence_rearm_duration)
    : absence_rearm_duration_(absence_rearm_duration) {}

bool PersonEventGate::update(bool has_person, std::chrono::steady_clock::time_point now) {
    if (!has_active_appearance_) {
        if (!has_person) return false;

        has_active_appearance_ = true;
        absence_pending_ = false;
        return true;
    }

    if (!has_person) {
        if (!absence_pending_) {
            absence_started_at_ = now;
            absence_pending_ = true;
        }
        return false;
    }

    if (!absence_pending_) return false;

    const bool absence_confirmed = now - absence_started_at_ >= absence_rearm_duration_;
    absence_pending_ = false;
    return absence_confirmed;
}

void PersonEventGate::reset() {
    absence_started_at_ = {};
    has_active_appearance_ = false;
    absence_pending_ = false;
}
