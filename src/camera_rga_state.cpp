#include "camera_rga_state.h"

void CameraRgaState::set_rotate180(bool enabled) noexcept {
    rotate180_.store(enabled, std::memory_order_release);
}

bool CameraRgaState::rotate180() const noexcept {
    return rotate180_.load(std::memory_order_acquire);
}

bool CameraRgaState::record_failure() noexcept {
    const uint32_t count = consecutive_failures_.fetch_add(1, std::memory_order_acq_rel) + 1;
    return (count & (count - 1U)) == 0;
}

void CameraRgaState::record_success() noexcept {
    consecutive_failures_.store(0, std::memory_order_release);
}

uint32_t CameraRgaState::consecutive_failures() const noexcept {
    return consecutive_failures_.load(std::memory_order_acquire);
}
