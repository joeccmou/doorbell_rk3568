#include "events/person_event_gate.h"

PersonEventGate::PersonEventGate(std::chrono::steady_clock::duration cooldown)
    : cooldown_(cooldown) {}

bool PersonEventGate::update(bool has_person, std::chrono::steady_clock::time_point now) {
    if (!has_person) {
        person_present_ = false;
        appearance_pending_ = false;
        return false;
    }

    if (!person_present_) {
        person_present_ = true;
        appearance_pending_ = true;
    }

    if (!appearance_pending_) return false;
    if (has_last_event_ && now - last_event_at_ < cooldown_) return false;

    last_event_at_ = now;
    has_last_event_ = true;
    appearance_pending_ = false;
    return true;
}

void PersonEventGate::reset() {
    last_event_at_ = {};
    has_last_event_ = false;
    person_present_ = false;
    appearance_pending_ = false;
}
