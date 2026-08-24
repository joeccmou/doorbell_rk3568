#include "device/webrtc_sdp_utils.h"

#include <utility>

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

bool webrtc_sdp_has_single_audio_video_pair(std::string_view sdp) {
    unsigned int video_sections = 0;
    unsigned int audio_sections = 0;
    unsigned int media_sections = 0;
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
            ++media_sections;
            const size_t media_end = line.find(' ', 2);
            const std::string_view media_type = line.substr(
                2,
                media_end == std::string_view::npos
                    ? std::string_view::npos
                    : media_end - 2);
            if (media_type == "video") {
                ++video_sections;
            } else if (media_type == "audio") {
                ++audio_sections;
            } else {
                return false;
            }
        }

        if (line_end == std::string_view::npos) {
            break;
        }
        offset = line_end + 1;
    }

    return media_sections == 2 && video_sections == 1 && audio_sections == 1;
}

void RemoteIceCandidateBuffer::begin_remote_description_update() {
    remote_description_set_ = false;
    pending_.clear();
}

bool RemoteIceCandidateBuffer::enqueue_if_pending(
    unsigned int mline_index,
    std::string candidate) {
    if (remote_description_set_) {
        return false;
    }
    pending_.push_back(RemoteIceCandidate{mline_index, std::move(candidate)});
    return true;
}

std::vector<RemoteIceCandidate>
RemoteIceCandidateBuffer::mark_remote_description_set() {
    remote_description_set_ = true;
    std::vector<RemoteIceCandidate> pending = std::move(pending_);
    pending_.clear();
    return pending;
}
