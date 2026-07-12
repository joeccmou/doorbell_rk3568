#include "events/snapshot_candidate_selector.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>

namespace {
constexpr uint32_t kSharpnessMaxWidth = 320;
constexpr uint32_t kSharpnessMaxHeight = 180;

double clamp01(double value) {
    return std::clamp(value, 0.0, 1.0);
}
}

SnapshotCandidateSelector::SnapshotCandidateSelector(
    std::chrono::steady_clock::duration window)
    : window_(window) {}

void SnapshotCandidateSelector::start(std::chrono::steady_clock::time_point now) {
    deadline_ = now + window_;
    active_ = true;
    best_.reset();
}

bool SnapshotCandidateSelector::active() const {
    return active_;
}

void SnapshotCandidateSelector::consider(
    const std::vector<uint8_t> &rgb,
    uint32_t width,
    uint32_t height,
    const std::vector<SnapshotPersonBox> &boxes,
    std::chrono::steady_clock::time_point now) {
    const size_t expected = static_cast<size_t>(width) * height * 3;
    if (!active_ || now > deadline_ || width == 0 || height == 0 ||
        rgb.size() < expected || boxes.empty()) {
        return;
    }

    const double sharpness = sharpness_score(rgb, width, height);
    double best_box_score = -std::numeric_limits<double>::infinity();
    double best_confidence = 0.0;
    for (const auto &box : boxes) {
        const double score = person_score(box, width, height, sharpness);
        if (score > best_box_score) {
            best_box_score = score;
            best_confidence = clamp01(box.confidence);
        }
    }
    if (!std::isfinite(best_box_score)) return;
    if (best_ && best_box_score <= best_->score) return;

    best_ = SelectedSnapshot{rgb, width, height, best_confidence, best_box_score};
}

std::optional<SelectedSnapshot> SnapshotCandidateSelector::take_if_ready(
    std::chrono::steady_clock::time_point now) {
    if (!active_ || now < deadline_) return std::nullopt;
    active_ = false;
    auto selected = std::move(best_);
    best_.reset();
    return selected;
}

double SnapshotCandidateSelector::sharpness_score(
    const std::vector<uint8_t> &rgb,
    uint32_t width,
    uint32_t height) {
    const uint32_t sample_width = std::min(width, kSharpnessMaxWidth);
    const uint32_t sample_height = std::min(height, kSharpnessMaxHeight);
    if (sample_width < 3 || sample_height < 3) return 0.0;

    std::vector<double> gray(static_cast<size_t>(sample_width) * sample_height);
    for (uint32_t y = 0; y < sample_height; ++y) {
        const uint32_t source_y = static_cast<uint32_t>(
            static_cast<uint64_t>(y) * height / sample_height);
        for (uint32_t x = 0; x < sample_width; ++x) {
            const uint32_t source_x = static_cast<uint32_t>(
                static_cast<uint64_t>(x) * width / sample_width);
            const size_t source = (static_cast<size_t>(source_y) * width + source_x) * 3;
            gray[static_cast<size_t>(y) * sample_width + x] =
                0.299 * rgb[source] + 0.587 * rgb[source + 1] + 0.114 * rgb[source + 2];
        }
    }

    double sum = 0.0;
    double sum_squared = 0.0;
    size_t count = 0;
    for (uint32_t y = 1; y + 1 < sample_height; ++y) {
        for (uint32_t x = 1; x + 1 < sample_width; ++x) {
            const size_t center = static_cast<size_t>(y) * sample_width + x;
            const double laplacian = 4.0 * gray[center] - gray[center - 1] -
                                     gray[center + 1] - gray[center - sample_width] -
                                     gray[center + sample_width];
            sum += laplacian;
            sum_squared += laplacian * laplacian;
            ++count;
        }
    }
    if (count == 0) return 0.0;
    const double mean = sum / static_cast<double>(count);
    const double variance = std::max(
        0.0, sum_squared / static_cast<double>(count) - mean * mean);
    return clamp01(variance / (variance + 1000.0));
}

double SnapshotCandidateSelector::person_score(
    const SnapshotPersonBox &box,
    uint32_t width,
    uint32_t height,
    double sharpness) {
    const int left = std::clamp(box.left, 0, static_cast<int>(width));
    const int top = std::clamp(box.top, 0, static_cast<int>(height));
    const int right = std::clamp(box.right, 0, static_cast<int>(width));
    const int bottom = std::clamp(box.bottom, 0, static_cast<int>(height));
    if (right <= left || bottom <= top) {
        return -std::numeric_limits<double>::infinity();
    }

    const double left_margin = static_cast<double>(left) / width;
    const double right_margin = static_cast<double>(width - right) / width;
    const double top_margin = static_cast<double>(top) / height;
    const double bottom_margin = static_cast<double>(height - bottom) / height;
    double composition = 1.0;
    if (left_margin <= 0.02 || right_margin <= 0.02) composition *= 0.1;
    if (top_margin <= 0.05) composition *= 0.1;
    if (bottom_margin <= 0.02) composition *= 0.6;

    const double area_ratio =
        static_cast<double>(right - left) * (bottom - top) /
        (static_cast<double>(width) * height);
    double area = 1.0;
    if (area_ratio < 0.10) {
        area = area_ratio / 0.10;
    } else if (area_ratio > 0.60) {
        area = 1.0 - (area_ratio - 0.60) / 0.40;
    }

    return clamp01(composition) * 0.35 +
           clamp01(sharpness) * 0.30 +
           clamp01(area) * 0.20 +
           clamp01(box.confidence) * 0.15;
}
