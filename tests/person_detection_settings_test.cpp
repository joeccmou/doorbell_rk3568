#include "device/person_detection_settings.h"

#include <cassert>

int main() {
    assert(person_sensitivity_threshold("low") == 0.8F);
    assert(person_sensitivity_threshold("medium") == 0.5F);
    assert(person_sensitivity_threshold("high") == 0.3F);
    assert(!person_score_exceeds_threshold(0.8F, person_sensitivity_threshold("low")));
    assert(person_score_exceeds_threshold(0.8001F, person_sensitivity_threshold("low")));
    assert(!person_score_exceeds_threshold(0.5F, person_sensitivity_threshold("medium")));
    assert(person_score_exceeds_threshold(0.5001F, person_sensitivity_threshold("medium")));
    assert(!person_score_exceeds_threshold(0.3F, person_sensitivity_threshold("high")));
    assert(person_score_exceeds_threshold(0.3001F, person_sensitivity_threshold("high")));
    return 0;
}
