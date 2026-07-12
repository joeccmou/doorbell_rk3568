#include "device/sntp_client.h"

#include <array>
#include <cassert>
#include <cstdint>

int main() {
    std::array<std::uint8_t, 48> response{};
    response[0] = 0x24;  // LI=0, VN=4, Mode=4(server)
    response[1] = 2;

    const std::int64_t t1 = 100000;
    const std::int64_t t2 = 100120;
    const std::int64_t t3 = 100140;
    const std::int64_t t4 = 100200;
    SntpClient::write_timestamp(response.data() + 24, t1);
    SntpClient::write_timestamp(response.data() + 32, t2);
    SntpClient::write_timestamp(response.data() + 40, t3);

    assert(SntpClient::read_timestamp(response.data() + 32) == t2);
    const auto result = SntpClient::parse_response(response.data(), response.size(), t1, t4);
    assert(result.ok);
    assert(result.offset_ms == 30);

    response[0] = 0x23;  // client mode is not a valid server response
    const auto invalid = SntpClient::parse_response(response.data(), response.size(), t1, t4);
    assert(!invalid.ok);
    return 0;
}
