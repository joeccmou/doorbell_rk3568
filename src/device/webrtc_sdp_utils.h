#pragma once

#include <optional>
#include <string>
#include <string_view>

std::optional<std::string> webrtc_sdp_mid_for_mline(
    std::string_view sdp,
    unsigned int mline_index);

