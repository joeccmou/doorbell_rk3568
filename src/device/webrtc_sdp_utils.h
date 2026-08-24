#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>

struct RemoteIceCandidate {
    unsigned int mline_index = 0;
    std::string candidate;
};

class RemoteIceCandidateBuffer {
public:
    void begin_remote_description_update();
    bool enqueue_if_pending(unsigned int mline_index, std::string candidate);
    std::vector<RemoteIceCandidate> mark_remote_description_set();

private:
    bool remote_description_set_ = false;
    std::vector<RemoteIceCandidate> pending_;
};

std::optional<std::string> webrtc_sdp_mid_for_mline(
    std::string_view sdp,
    unsigned int mline_index);

bool webrtc_sdp_has_single_audio_video_pair(std::string_view sdp);
