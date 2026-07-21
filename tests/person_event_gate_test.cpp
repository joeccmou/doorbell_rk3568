#include "events/person_event_gate.h"

#include <cassert>
#include <chrono>

int main() {
    using namespace std::chrono;
    const auto start = steady_clock::time_point(seconds(100));
    PersonEventGate gate(seconds(30));

    // 首次检测到人时只生成一条事件，持续检测到人不重复生成。
    assert(gate.update(true, start));
    assert(!gate.update(true, start + seconds(1)));
    assert(!gate.update(true, start + seconds(20)));

    // 短暂漏检不结束本次人形出现，也不能在旧的冷却边界重复生成事件。
    assert(!gate.update(false, start + seconds(21)));
    assert(!gate.update(true, start + seconds(22)));
    assert(!gate.update(true, start + seconds(29)));
    assert(!gate.update(true, start + seconds(30)));
    assert(!gate.update(true, start + seconds(31)));

    // 只有连续未检测到人满 30 秒，再次检测到人时才生成下一条事件。
    assert(!gate.update(false, start + seconds(40)));
    assert(!gate.update(false, start + seconds(69)));
    assert(gate.update(true, start + seconds(70)));
    assert(!gate.update(true, start + seconds(71)));

    // 连续无人超过 30 秒也只在下一次检测到人时生成事件。
    assert(!gate.update(false, start + seconds(80)));
    assert(!gate.update(false, start + seconds(111)));
    assert(gate.update(true, start + seconds(112)));
    return 0;
}
