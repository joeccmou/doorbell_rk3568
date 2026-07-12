#include "events/person_event_gate.h"

#include <cassert>
#include <chrono>

int main() {
    using namespace std::chrono;
    const auto start = steady_clock::time_point(seconds(100));
    PersonEventGate gate(seconds(30));

    assert(gate.update(true, start));
    assert(!gate.update(true, start + seconds(1)));
    assert(!gate.update(true, start + seconds(20)));

    assert(!gate.update(false, start + seconds(21)));
    assert(!gate.update(true, start + seconds(22)));
    assert(!gate.update(true, start + seconds(29)));
    assert(gate.update(true, start + seconds(30)));
    assert(!gate.update(true, start + seconds(31)));

    assert(!gate.update(false, start + seconds(40)));
    assert(gate.update(true, start + seconds(61)));
    return 0;
}
