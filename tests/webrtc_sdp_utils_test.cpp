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

int main() {
    test_maps_mline_index_to_actual_mid();
    test_rejects_missing_or_out_of_range_mid();
    return 0;
}
