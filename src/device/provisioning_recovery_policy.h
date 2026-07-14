#pragma once

enum class ProvisioningRecoveryAction {
    KeepStaAndRetryCloud,
    StartAccessPoint,
};

// Wi-Fi 已验证并保存后，云端故障不得重新进入 AP 配网。
constexpr ProvisioningRecoveryAction recovery_after_cloud_failure_with_valid_wifi() {
    return ProvisioningRecoveryAction::KeepStaAndRetryCloud;
}
