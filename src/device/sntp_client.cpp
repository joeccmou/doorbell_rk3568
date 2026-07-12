#include "device/sntp_client.h"

#include <array>
#include <chrono>
#include <cstring>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netdb.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
#endif

namespace {

constexpr std::uint64_t kNtpUnixEpochSeconds = 2208988800ULL;

std::int64_t unix_now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

std::uint32_t read_u32(const std::uint8_t *source) {
    return (static_cast<std::uint32_t>(source[0]) << 24U) |
           (static_cast<std::uint32_t>(source[1]) << 16U) |
           (static_cast<std::uint32_t>(source[2]) << 8U) |
           static_cast<std::uint32_t>(source[3]);
}

void write_u32(std::uint8_t *target, std::uint32_t value) {
    target[0] = static_cast<std::uint8_t>(value >> 24U);
    target[1] = static_cast<std::uint8_t>(value >> 16U);
    target[2] = static_cast<std::uint8_t>(value >> 8U);
    target[3] = static_cast<std::uint8_t>(value);
}

#ifdef _WIN32
using SocketHandle = SOCKET;
constexpr SocketHandle kInvalidSocket = INVALID_SOCKET;
void close_socket(SocketHandle socket_fd) { closesocket(socket_fd); }
#else
using SocketHandle = int;
constexpr SocketHandle kInvalidSocket = -1;
void close_socket(SocketHandle socket_fd) { close(socket_fd); }
#endif

}  // namespace

void SntpClient::write_timestamp(std::uint8_t *target, std::int64_t unix_ms) {
    const auto unix_seconds = unix_ms / 1000;
    auto remainder_ms = unix_ms % 1000;
    if (remainder_ms < 0) remainder_ms += 1000;
    const auto ntp_seconds = static_cast<std::uint64_t>(unix_seconds) + kNtpUnixEpochSeconds;
    const auto fraction = (static_cast<std::uint64_t>(remainder_ms) << 32U) / 1000U;
    write_u32(target, static_cast<std::uint32_t>(ntp_seconds));
    write_u32(target + 4, static_cast<std::uint32_t>(fraction));
}

std::int64_t SntpClient::read_timestamp(const std::uint8_t *source) {
    const std::uint64_t ntp_seconds = read_u32(source);
    const std::uint64_t fraction = read_u32(source + 4);
    const auto unix_seconds = static_cast<std::int64_t>(ntp_seconds - kNtpUnixEpochSeconds);
    const auto milliseconds = static_cast<std::int64_t>((fraction * 1000U + (1ULL << 31U)) >> 32U);
    return unix_seconds * 1000 + milliseconds;
}

SntpResult SntpClient::parse_response(const std::uint8_t *packet,
                                      std::size_t size,
                                      std::int64_t request_unix_ms,
                                      std::int64_t response_unix_ms) {
    SntpResult result;
    result.error_code = "NTP_INVALID_RESPONSE";
    if (!packet || size < 48) return result;

    const int leap = (packet[0] >> 6U) & 0x03U;
    const int mode = packet[0] & 0x07U;
    const int stratum = packet[1];
    if (leap == 3 || (mode != 4 && mode != 5) || stratum < 1 || stratum > 15) return result;

    const auto originate_ms = read_timestamp(packet + 24);
    const auto receive_ms = read_timestamp(packet + 32);
    const auto transmit_ms = read_timestamp(packet + 40);
    if (originate_ms != request_unix_ms || receive_ms == 0 || transmit_ms == 0) return result;

    result.offset_ms = ((receive_ms - request_unix_ms) + (transmit_ms - response_unix_ms)) / 2;
    result.ok = true;
    result.error_code.clear();
    return result;
}

SntpResult SntpClient::query(const std::string &server, std::chrono::milliseconds timeout) const {
    SntpResult result;
    result.server = server;
    result.error_code = "NTP_UNREACHABLE";

#ifdef _WIN32
    WSADATA winsock{};
    if (WSAStartup(MAKEWORD(2, 2), &winsock) != 0) return result;
#endif

    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_DGRAM;
    hints.ai_protocol = IPPROTO_UDP;
    addrinfo *addresses = nullptr;
    if (getaddrinfo(server.c_str(), "123", &hints, &addresses) != 0) {
#ifdef _WIN32
        WSACleanup();
#endif
        return result;
    }

    for (auto *address = addresses; address != nullptr && !result.ok; address = address->ai_next) {
        const SocketHandle socket_fd = socket(address->ai_family, address->ai_socktype, address->ai_protocol);
        if (socket_fd == kInvalidSocket) continue;

#ifdef _WIN32
        const DWORD timeout_ms = static_cast<DWORD>(timeout.count());
        setsockopt(socket_fd, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char *>(&timeout_ms), sizeof(timeout_ms));
#else
        timeval socket_timeout{};
        socket_timeout.tv_sec = static_cast<long>(timeout.count() / 1000);
        socket_timeout.tv_usec = static_cast<long>((timeout.count() % 1000) * 1000);
        setsockopt(socket_fd, SOL_SOCKET, SO_RCVTIMEO, &socket_timeout, sizeof(socket_timeout));
#endif

        if (connect(socket_fd, address->ai_addr, static_cast<int>(address->ai_addrlen)) != 0) {
            close_socket(socket_fd);
            continue;
        }

        std::array<std::uint8_t, 48> request{};
        request[0] = 0x23;  // LI=0, VN=4, Mode=3(client)
        const auto request_ms = unix_now_ms();
        write_timestamp(request.data() + 40, request_ms);
#ifdef _WIN32
        const int sent = send(socket_fd, reinterpret_cast<const char *>(request.data()), static_cast<int>(request.size()), 0);
#else
        const auto sent = send(socket_fd, request.data(), request.size(), 0);
#endif
        if (sent != static_cast<decltype(sent)>(request.size())) {
            close_socket(socket_fd);
            continue;
        }

        std::array<std::uint8_t, 512> response{};
#ifdef _WIN32
        const int received = recv(socket_fd, reinterpret_cast<char *>(response.data()), static_cast<int>(response.size()), 0);
#else
        const auto received = recv(socket_fd, response.data(), response.size(), 0);
#endif
        const auto response_ms = unix_now_ms();
        close_socket(socket_fd);
        if (received < 48) continue;
        result = parse_response(response.data(), static_cast<std::size_t>(received), request_ms, response_ms);
        result.server = server;
    }

    freeaddrinfo(addresses);
#ifdef _WIN32
    WSACleanup();
#endif
    return result;
}
