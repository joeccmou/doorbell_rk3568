#pragma once

#include <chrono>
#include <cstdint>
#include <optional>
#include <vector>

struct SnapshotPersonBox {
    int left = 0;
    int top = 0;
    int right = 0;
    int bottom = 0;
    float confidence = 0.0F;
};

struct SelectedSnapshot {
    std::vector<uint8_t> rgb;
    uint32_t width = 0;
    uint32_t height = 0;
    double confidence = 0.0;
    double score = 0.0;
};

class SnapshotCandidateSelector {
public:
    explicit SnapshotCandidateSelector(
        std::chrono::steady_clock::duration window = std::chrono::seconds(2),
        bool allow_empty_boxes = false);

    void start(std::chrono::steady_clock::time_point now);
    bool active() const;

    void consider(const std::vector<uint8_t> &rgb,
                  uint32_t width,
                  uint32_t height,
                  const std::vector<SnapshotPersonBox> &boxes,
                  std::chrono::steady_clock::time_point now);

    std::optional<SelectedSnapshot> take_if_ready(
        std::chrono::steady_clock::time_point now);

private:
    static double sharpness_score(const std::vector<uint8_t> &rgb,
                                  uint32_t width,
                                  uint32_t height);
    static double person_score(const SnapshotPersonBox &box,
                               uint32_t width,
                               uint32_t height,
                               double sharpness);

    std::chrono::steady_clock::duration window_;
    bool allow_empty_boxes_ = false;
    std::chrono::steady_clock::time_point deadline_{};
    bool active_ = false;
    std::optional<SelectedSnapshot> best_;
};
