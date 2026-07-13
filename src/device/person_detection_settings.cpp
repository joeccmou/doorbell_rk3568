#include "device/person_detection_settings.h"

float person_sensitivity_threshold(const std::string &sensitivity) {
    if (sensitivity == "low") return 0.8F;
    if (sensitivity == "high") return 0.3F;
    return 0.5F;
}

bool person_score_exceeds_threshold(float score, float threshold) {
    return score > threshold;
}
