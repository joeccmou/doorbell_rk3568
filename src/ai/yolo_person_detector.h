#pragma once

#include <cstdint>
#include <string>
#include <vector>

#ifdef DOORBELL_USE_RKNN_YOLO
#include "yolov8.h"
#endif

class YoloPersonDetector {
public:
    struct PersonBox {
        int left = 0;
        int top = 0;
        int right = 0;
        int bottom = 0;
        float score = 0.0f;
    };

    YoloPersonDetector() = default;
    ~YoloPersonDetector();

    bool load(const std::string &model_path);
    bool ready() const { return ready_; }

    bool detect_person(const uint8_t *rgb, uint32_t width, uint32_t height, std::vector<PersonBox> &boxes);

private:
    bool ready_ = false;
#ifdef DOORBELL_USE_RKNN_YOLO
    rknn_app_context_t app_ctx_{};
#endif
};
