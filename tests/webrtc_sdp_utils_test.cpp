#include "device/webrtc_sdp_utils.h"

#include <cassert>
#include <optional>
#include <string>

void test_maps_mline_index_to_actual_mid() {
    const std::string sdp =
        "v=0\r\n"
        "m=video 9 UDP/TLS/RTP/SAVPF 96\r\n"
        "a=sendonly\r\n"
        "a=mid:video0\r\n"
        "m=audio 9 UDP/TLS/RTP/SAVPF 97\r\n"
        "a=mid:audio1\r\n";

    assert(webrtc_sdp_mid_for_mline(sdp, 0) == std::optional<std::string>("video0"));
    assert(webrtc_sdp_mid_for_mline(sdp, 1) == std::optional<std::string>("audio1"));
}

void test_rejects_missing_or_out_of_range_mid() {
    const std::string missing_mid =
        "v=0\n"
        "m=video 9 UDP/TLS/RTP/SAVPF 96\n"
        "a=sendonly\n";

    assert(!webrtc_sdp_mid_for_mline(missing_mid, 0).has_value());
    assert(!webrtc_sdp_mid_for_mline(missing_mid, 1).has_value());
}

void test_accepts_only_one_video_and_one_audio_media_section() {
    const std::string expected_offer =
        "v=0\r\n"
        "m=video 9 UDP/TLS/RTP/SAVPF 96\r\n"
        "a=mid:video0\r\n"
        "m=audio 9 UDP/TLS/RTP/SAVPF 97\r\n"
        "a=mid:audio1\r\n";
    const std::string duplicated_offer =
        "v=0\r\n"
        "m=video 9 UDP/TLS/RTP/SAVPF 96\r\n"
        "a=mid:video0\r\n"
        "m=audio 9 UDP/TLS/RTP/SAVPF 97\r\n"
        "a=mid:audio1\r\n"
        "m=video 9 UDP/TLS/RTP/SAVPF 96\r\n"
        "a=mid:video2\r\n"
        "m=audio 9 UDP/TLS/RTP/SAVPF 97\r\n"
        "a=mid:audio3\r\n";

    assert(webrtc_sdp_has_single_audio_video_pair(expected_offer));
    assert(!webrtc_sdp_has_single_audio_video_pair(duplicated_offer));
}

void test_defers_remote_candidates_until_answer_is_applied() {
    RemoteIceCandidateBuffer buffer;
    buffer.begin_remote_description_update();

    assert(buffer.enqueue_if_pending(0, "candidate-video"));
    assert(buffer.enqueue_if_pending(1, "candidate-audio"));

    const auto pending = buffer.mark_remote_description_set();
    assert(pending.size() == 2);
    assert(pending[0].mline_index == 0);
    assert(pending[0].candidate == "candidate-video");
    assert(pending[1].mline_index == 1);
    assert(pending[1].candidate == "candidate-audio");

    assert(!buffer.enqueue_if_pending(0, "candidate-after-answer"));
}

int main() {
    test_maps_mline_index_to_actual_mid();
    test_rejects_missing_or_out_of_range_mid();
    test_accepts_only_one_video_and_one_audio_media_section();
    test_defers_remote_candidates_until_answer_is_applied();
    return 0;
}
