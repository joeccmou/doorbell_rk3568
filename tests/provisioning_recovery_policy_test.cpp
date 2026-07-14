#include "device/provisioning_recovery_policy.h"

#include <cassert>

int main() {
    assert(recovery_after_cloud_failure_with_valid_wifi() ==
           ProvisioningRecoveryAction::KeepStaAndRetryCloud);
    return 0;
}
