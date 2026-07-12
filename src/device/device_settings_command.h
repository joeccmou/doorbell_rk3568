#pragma once

#include <string>

struct DeviceSettingsCommand {
    bool is_apply_settings = false;
    bool valid = false;
    std::string trace_id;
    std::string cmd_id;
    std::string timezone;
    std::string error_code;
};

DeviceSettingsCommand parse_device_settings_command(const std::string &payload);
