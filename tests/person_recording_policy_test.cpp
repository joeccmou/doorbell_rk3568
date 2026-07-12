#include "events/person_recording_policy.h"

#include <cassert>
#include <cstdint>

int main() {
    constexpr uint64_t second = 1000ULL * 1000ULL * 1000ULL;
    const uint64_t first_seen = 100ULL * second;

    assert(!person_recording_active(0, first_seen));
    assert(person_recording_active(first_seen, first_seen + 29ULL * second));
    assert(!person_recording_active(first_seen, first_seen + 30ULL * second));
    assert(!person_recording_active(first_seen, first_seen - second));

    const uint64_t seen_again = first_seen + 25ULL * second;
    assert(person_recording_active(seen_again, seen_again + 29ULL * second));
    assert(!person_recording_active(seen_again, seen_again + 30ULL * second));
    return 0;
}
