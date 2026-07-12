#include "events/snapshot_candidate_selector.h"

#include <cassert>
#include <chrono>
#include <cstdint>
#include <vector>

namespace {
constexpr uint32_t kWidth = 64;
constexpr uint32_t kHeight = 36;

std::vector<uint8_t> solid_frame(uint8_t value) {
    return std::vector<uint8_t>(static_cast<size_t>(kWidth) * kHeight * 3, value);
}

std::vector<uint8_t> checker_frame() {
    std::vector<uint8_t> rgb(static_cast<size_t>(kWidth) * kHeight * 3);
    for (uint32_t y = 0; y < kHeight; ++y) {
        for (uint32_t x = 0; x < kWidth; ++x) {
            const uint8_t value = ((x + y) % 2 == 0) ? 0 : 255;
            const size_t offset = (static_cast<size_t>(y) * kWidth + x) * 3;
            rgb[offset] = value;
            rgb[offset + 1] = value;
            rgb[offset + 2] = value;
        }
    }
    return rgb;
}

SnapshotPersonBox centered_box(float confidence = 0.8F) {
    return SnapshotPersonBox{16, 4, 48, 32, confidence};
}

void test_window_finishes_at_two_seconds_and_resets() {
    using namespace std::chrono;
    SnapshotCandidateSelector selector(seconds(2));
    const auto started_at = steady_clock::time_point(seconds(10));
    selector.start(started_at);
    selector.consider(solid_frame(20), kWidth, kHeight, {centered_box()}, started_at);

    assert(!selector.take_if_ready(started_at + milliseconds(1999)).has_value());
    auto selected = selector.take_if_ready(started_at + seconds(2));
    assert(selected.has_value());
    assert(selected->rgb.front() == 20);
    assert(!selector.active());
}

void test_complete_person_beats_edge_clipped_person() {
    using namespace std::chrono;
    SnapshotCandidateSelector selector(seconds(2));
    const auto started_at = steady_clock::time_point(seconds(20));
    selector.start(started_at);
    selector.consider(
        solid_frame(10), kWidth, kHeight,
        {SnapshotPersonBox{0, 0, 44, 35, 0.99F}}, started_at);
    selector.consider(
        solid_frame(30), kWidth, kHeight,
        {centered_box(0.75F)}, started_at + milliseconds(500));

    auto selected = selector.take_if_ready(started_at + seconds(2));
    assert(selected.has_value());
    assert(selected->rgb.front() == 30);
}

void test_sharp_frame_beats_flat_frame_with_same_box() {
    using namespace std::chrono;
    SnapshotCandidateSelector selector(seconds(2));
    const auto started_at = steady_clock::time_point(seconds(30));
    selector.start(started_at);
    selector.consider(solid_frame(80), kWidth, kHeight, {centered_box()}, started_at);
    const auto sharp = checker_frame();
    selector.consider(sharp, kWidth, kHeight, {centered_box()}, started_at + milliseconds(500));

    auto selected = selector.take_if_ready(started_at + seconds(2));
    assert(selected.has_value());
    assert(selected->rgb == sharp);
}

void test_first_frame_is_fallback_when_no_better_candidate_arrives() {
    using namespace std::chrono;
    SnapshotCandidateSelector selector(seconds(2));
    const auto started_at = steady_clock::time_point(seconds(40));
    selector.start(started_at);
    const auto first = solid_frame(55);
    selector.consider(first, kWidth, kHeight, {centered_box()}, started_at);

    auto selected = selector.take_if_ready(started_at + seconds(2));
    assert(selected.has_value());
    assert(selected->rgb == first);
}
}

int main() {
    test_window_finishes_at_two_seconds_and_resets();
    test_complete_person_beats_edge_clipped_person();
    test_sharp_frame_beats_flat_frame_with_same_box();
    test_first_frame_is_fallback_when_no_better_candidate_arrives();
    return 0;
}
