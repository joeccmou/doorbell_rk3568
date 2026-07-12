#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>

struct SntpResult {
    bool ok = false;
    std::int64_t offset_ms = 0;
    std::string server;
    std::string error_code;
};

class SntpClient {
public:
    SntpResult query(const std::string &server,
                     std::chrono::milliseconds timeout = std::chrono::milliseconds(3000)) const;

    static void write_timestamp(std::uint8_t *target, std::int64_t unix_ms);
    static std::int64_t read_timestamp(const std::uint8_t *source);
    static SntpResult parse_response(const std::uint8_t *packet,
                                     std::size_t size,
                                     std::int64_t request_unix_ms,
                                     std::int64_t response_unix_ms);
};
