#pragma once

#include <string>

float person_sensitivity_threshold(const std::string &sensitivity);
bool person_score_exceeds_threshold(float score, float threshold);
