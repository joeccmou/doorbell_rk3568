#pragma once

#include <cstdint>
#include <mutex>
#include <optional>
#include <string>

struct DeviceSettingsValues {
    bool person_detection = true;
    std::string person_sensitivity = "medium";
    bool status_led = true;
    bool image_rotate180 = false;
    std::string recording_quality = "1080p";
    std::string timezone = "Asia/Shanghai";
};

struct DeviceTimeSyncRuntime {
    std::string state = "unsynced";
    std::optional<std::string> last_success_at;
    std::optional<std::int64_t> last_offset_ms;
};

struct DeviceSettingsRuntime {
    DeviceTimeSyncRuntime time_sync;
};

struct DeviceSettingsSnapshot {
    int version = 1;
    DeviceSettingsValues settings;
    DeviceSettingsRuntime runtime;
    std::string updated_at;
};

class DeviceSettingsStore {
public:
    explicit DeviceSettingsStore(std::string path);

    bool initialize(std::string *error = nullptr);
    DeviceSettingsSnapshot snapshot() const;
    bool replace_settings(const DeviceSettingsValues &settings, std::string *error = nullptr);
    bool update_time_sync(const DeviceTimeSyncRuntime &time_sync, std::string *error = nullptr);

private:
    bool persist_locked(const DeviceSettingsSnapshot &snapshot, std::string *error);

    std::string path_;
    mutable std::mutex mutex_;
    DeviceSettingsSnapshot snapshot_;
};
