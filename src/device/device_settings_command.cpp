#include "device/device_settings_command.h"

#include <cctype>
#include <optional>

namespace {

std::optional<std::string> string_field(const std::string &json, const std::string &name) {
    const std::string key = "\"" + name + "\"";
    auto position = json.find(key);
    if (position == std::string::npos) return std::nullopt;
    position = json.find(':', position + key.size());
    if (position == std::string::npos) return std::nullopt;
    ++position;
    while (position < json.size() && std::isspace(static_cast<unsigned char>(json[position]))) ++position;
    if (position >= json.size() || json[position] != '"') return std::nullopt;
    ++position;
    std::string value;
    while (position < json.size() && json[position] != '"') {
        if (json[position] == '\\') return std::nullopt;
        value.push_back(json[position++]);
    }
    if (position >= json.size()) return std::nullopt;
    return value;
}

}  // namespace

DeviceSettingsCommand parse_device_settings_command(const std::string &payload) {
    DeviceSettingsCommand command;
    const auto action = string_field(payload, "action");
    if (!action || *action != "apply_settings") return command;
    command.is_apply_settings = true;
    command.trace_id = string_field(payload, "trace_id").value_or("");
    command.cmd_id = string_field(payload, "cmd_id").value_or("");
    command.timezone = string_field(payload, "timezone").value_or("");
    if (command.trace_id.empty() || command.cmd_id.empty() || command.timezone.empty()) {
        command.error_code = "TIMEZONE_INVALID";
        return command;
    }
    command.valid = true;
    return command;
}
