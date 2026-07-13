#pragma once

#include <optional>
#include <string>

enum class DeviceSettingsCommandType {
    None,
    Query,
    Apply,
};

struct DeviceSettingsPatch {
    std::optional<bool> person_detection;
    std::optional<std::string> person_sensitivity;
    std::optional<bool> status_led;
    std::optional<bool> image_rotate180;
    std::optional<std::string> timezone;

    bool empty() const;
};

struct DeviceSettingsCommand {
    DeviceSettingsCommandType type = DeviceSettingsCommandType::None;
    bool valid = false;
    std::string trace_id;
    std::string cmd_id;
    std::string device_id;
    std::string error_code;
    DeviceSettingsPatch patch;
};

DeviceSettingsCommand parse_device_settings_command(const std::string &payload);
