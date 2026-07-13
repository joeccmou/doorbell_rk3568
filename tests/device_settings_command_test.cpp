#include "device/device_settings_command.h"

#include <cassert>

int main() {
    const auto query = parse_device_settings_command(
        R"({"trace_id":"tr_1","cmd_id":"cmd_1","device_id":"RK3568-000001","action":"query_settings","params":{}})");
    assert(query.type == DeviceSettingsCommandType::Query);
    assert(query.valid);
    assert(query.trace_id == "tr_1");
    assert(query.cmd_id == "cmd_1");

    const auto apply = parse_device_settings_command(
        R"({"trace_id":"tr_2","cmd_id":"cmd_2","device_id":"RK3568-000001","action":"apply_settings","params":{"settings":{"person_detection":false,"person_sensitivity":"high","status_led":false,"image_rotate180":true,"timezone":"Asia/Urumqi"}}})");
    assert(apply.type == DeviceSettingsCommandType::Apply);
    assert(apply.valid);
    assert(apply.patch.person_detection && !*apply.patch.person_detection);
    assert(apply.patch.person_sensitivity && *apply.patch.person_sensitivity == "high");
    assert(apply.patch.status_led && !*apply.patch.status_led);
    assert(apply.patch.image_rotate180 && *apply.patch.image_rotate180);
    assert(apply.patch.timezone && *apply.patch.timezone == "Asia/Urumqi");

    const auto missing = parse_device_settings_command(
        R"({"trace_id":"tr_3","cmd_id":"cmd_3","action":"apply_settings","params":{"settings":{}}})");
    assert(missing.type == DeviceSettingsCommandType::Apply);
    assert(!missing.valid);
    assert(missing.error_code == "INVALID_DEVICE_SETTINGS");

    const auto unknown = parse_device_settings_command(
        R"({"trace_id":"tr_4","cmd_id":"cmd_4","action":"apply_settings","params":{"settings":{"night_vision":true}}})");
    assert(!unknown.valid);
    assert(unknown.error_code == "INVALID_DEVICE_SETTINGS");

    const auto live = parse_device_settings_command(
        R"({"trace_id":"tr_5","cmd_id":"cmd_5","action":"start_live","quality":"720p"})");
    assert(live.type == DeviceSettingsCommandType::None);
    return 0;
}
