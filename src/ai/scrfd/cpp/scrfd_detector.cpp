#include "scrfd_detector.h"

#include <cstdio>

bool ScrfdDetector::load(const std::string &model_path, int input_w, int input_h) {
    model_path_ = model_path;
    input_w_ = input_w;
    input_h_ = input_h;
    if (model_path_.empty()) {
        std::fprintf(stderr, "[scrfd] model path empty; detection disabled\n");
        ready_ = false;
        return false;
    }
    // TODO: Replace stub with RKNN init/load using model_path_.
    std::fprintf(stdout, "[scrfd] stub load model=%s input=%dx%d (no RKNN yet)\n", model_path_.c_str(), input_w_, input_h_);
    ready_ = false; // keep false until RKNN implementation provided
    return false;
}

bool ScrfdDetector::detect(const uint8_t *bgra, uint32_t width, uint32_t height, std::vector<Box> &out_boxes) {
    out_boxes.clear();
    if (!ready_) return false;
    if (!bgra || width == 0 || height == 0) return false;

    // TODO: Implement RKNN inference. Stub returns no boxes.
    return true;
}
