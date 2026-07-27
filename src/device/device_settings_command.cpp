#include "device/device_settings_command.h"

#include <array>

#include <nlohmann/json.hpp>

bool DeviceSettingsPatch::empty() const {
    return !person_detection && !person_sensitivity && !status_led &&
           !image_rotate180 && !recording_quality && !timezone;
}

DeviceSettingsCommand parse_device_settings_command(const std::string &payload) {
    DeviceSettingsCommand command;
    try {
        const auto body = nlohmann::json::parse(payload);
        const std::string action = body.value("action", "");
        if (action == "query_settings") {
            command.type = DeviceSettingsCommandType::Query;
        } else if (action == "apply_settings") {
            command.type = DeviceSettingsCommandType::Apply;
        } else {
            return command;
        }

        command.trace_id = body.value("trace_id", "");
        command.cmd_id = body.value("cmd_id", "");
        command.device_id = body.value("device_id", "");
        if (command.trace_id.empty() || command.cmd_id.empty()) {
            command.error_code = "INVALID_DEVICE_SETTINGS";
            return command;
        }
        if (command.type == DeviceSettingsCommandType::Query) {
            command.valid = true;
            return command;
        }

        if (!body.contains("params") || !body["params"].is_object() ||
            !body["params"].contains("settings") || !body["params"]["settings"].is_object()) {
            command.error_code = "INVALID_DEVICE_SETTINGS";
            return command;
        }
        const auto &settings = body["params"]["settings"];
        static constexpr std::array<const char *, 6> allowed = {
            "person_detection", "person_sensitivity", "status_led", "image_rotate180",
            "recording_quality", "timezone"};
        for (auto it = settings.begin(); it != settings.end(); ++it) {
            bool known = false;
            for (const char *name : allowed) known = known || it.key() == name;
            if (!known) {
                command.error_code = "INVALID_DEVICE_SETTINGS";
                return command;
            }
        }
        if (settings.contains("person_detection")) command.patch.person_detection = settings.at("person_detection").get<bool>();
        if (settings.contains("person_sensitivity")) command.patch.person_sensitivity = settings.at("person_sensitivity").get<std::string>();
        if (settings.contains("status_led")) command.patch.status_led = settings.at("status_led").get<bool>();
        if (settings.contains("image_rotate180")) command.patch.image_rotate180 = settings.at("image_rotate180").get<bool>();
        if (settings.contains("recording_quality")) command.patch.recording_quality = settings.at("recording_quality").get<std::string>();
        if (settings.contains("timezone")) command.patch.timezone = settings.at("timezone").get<std::string>();
        if (command.patch.empty()) {
            command.error_code = "INVALID_DEVICE_SETTINGS";
            return command;
        }
        command.valid = true;
        return command;
    } catch (const std::exception &) {
        command.error_code = "INVALID_DEVICE_SETTINGS";
        return command;
    }
}
