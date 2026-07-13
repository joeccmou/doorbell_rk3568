#pragma once

#include <atomic>
#include <cstdint>

class CameraRgaState {
public:
    void set_rotate180(bool enabled) noexcept;
    bool rotate180() const noexcept;

    bool record_failure() noexcept;
    void record_success() noexcept;
    uint32_t consecutive_failures() const noexcept;

private:
    std::atomic<bool> rotate180_{false};
    std::atomic<uint32_t> consecutive_failures_{0};
};
