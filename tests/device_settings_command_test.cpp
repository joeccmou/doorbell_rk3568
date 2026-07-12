#include "device/device_settings_command.h"

#include <cassert>

int main() {
    const auto parsed = parse_device_settings_command(
        R"({"trace_id":"tr_1","cmd_id":"cmd_1","device_id":"RK3568-000001","action":"apply_settings","params":{"timezone":"Asia/Shanghai"}})");
    assert(parsed.is_apply_settings);
    assert(parsed.valid);
    assert(parsed.trace_id == "tr_1");
    assert(parsed.cmd_id == "cmd_1");
    assert(parsed.timezone == "Asia/Shanghai");

    const auto missing = parse_device_settings_command(
        R"({"trace_id":"tr_1","cmd_id":"cmd_1","action":"apply_settings","params":{}})");
    assert(missing.is_apply_settings);
    assert(!missing.valid);
    assert(missing.error_code == "TIMEZONE_INVALID");

    const auto live = parse_device_settings_command(
        R"({"trace_id":"tr_1","cmd_id":"cmd_1","action":"start_live","params":{}})");
    assert(!live.is_apply_settings);
    return 0;
}
