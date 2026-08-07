#include "device/webrtc_sdp_utils.h"

std::optional<std::string> webrtc_sdp_mid_for_mline(
    std::string_view sdp,
    unsigned int mline_index) {
    unsigned int current_mline = 0;
    bool in_media_section = false;
    size_t offset = 0;

    while (offset <= sdp.size()) {
        const size_t line_end = sdp.find('\n', offset);
        const size_t length = line_end == std::string_view::npos
            ? sdp.size() - offset
            : line_end - offset;
        std::string_view line = sdp.substr(offset, length);
        if (!line.empty() && line.back() == '\r') {
            line.remove_suffix(1);
        }

        if (line.rfind("m=", 0) == 0) {
            if (in_media_section && current_mline == mline_index) {
                return std::nullopt;
            }
            if (in_media_section) {
                ++current_mline;
            } else {
                in_media_section = true;
            }
        } else if (in_media_section && current_mline == mline_index &&
                   line.rfind("a=mid:", 0) == 0) {
            const std::string_view mid = line.substr(6);
            if (!mid.empty()) {
                return std::string(mid);
            }
            return std::nullopt;
        }

        if (line_end == std::string_view::npos) {
            break;
        }
        offset = line_end + 1;
    }
    return std::nullopt;
}

