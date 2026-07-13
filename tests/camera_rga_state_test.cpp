#include "camera_rga_state.h"

#include <cassert>

int main() {
    CameraRgaState state;
    assert(!state.rotate180());

    state.set_rotate180(true);
    assert(state.rotate180());
    state.set_rotate180(false);
    assert(!state.rotate180());

    assert(state.record_failure());
    assert(state.consecutive_failures() == 1);
    assert(state.record_failure());
    assert(state.consecutive_failures() == 2);
    assert(!state.record_failure());
    assert(state.consecutive_failures() == 3);
    assert(state.record_failure());
    assert(state.consecutive_failures() == 4);

    state.record_success();
    assert(state.consecutive_failures() == 0);
    assert(state.record_failure());
    assert(state.consecutive_failures() == 1);
    return 0;
}
