#pragma once

#include <cstdint>
#include <string>
#include <vector>

// Lightweight SCRFD detector stub. Replace implementation with RKNN-backed inference when model is available.
class ScrfdDetector {
public:
    struct Box {
        int x = 0;
        int y = 0;
        int w = 0;
        int h = 0;
        float score = 0.0f;
    };

    ScrfdDetector() = default;
    ~ScrfdDetector() = default;

    // Load RKNN model. Currently a stub; returns false if model path missing.
    bool load(const std::string &model_path, int input_w, int input_h);

    // Run detection on BGRA buffer; returns true on success.
    bool detect(const uint8_t *bgra, uint32_t width, uint32_t height, std::vector<Box> &out_boxes);

    bool ready() const { return ready_; }

private:
    bool ready_ = false;
    std::string model_path_;
    int input_w_ = 0;
    int input_h_ = 0;
};
