#include "device/device_settings_store.h"

#include <cassert>
#include <filesystem>

int main() {
    const auto root = std::filesystem::temp_directory_path() / "doorbell_device_settings_store_test";
    std::error_code ec;
    std::filesystem::remove_all(root, ec);
    std::filesystem::create_directories(root, ec);
    assert(!ec);

    const auto path = (root / "settings.json").string();
    DeviceSettingsStore store(path);
    std::string error;
    assert(store.initialize(&error));
    assert(std::filesystem::exists(path));

    auto initial = store.snapshot();
    assert(initial.settings.person_detection);
    assert(initial.settings.person_sensitivity == "medium");
    assert(initial.settings.status_led);
    assert(!initial.settings.image_rotate180);
    assert(initial.settings.timezone == "Asia/Shanghai");
    assert(initial.runtime.time_sync.state == "unsynced");

    auto changed = initial.settings;
    changed.person_detection = false;
    changed.person_sensitivity = "high";
    changed.status_led = false;
    changed.image_rotate180 = true;
    changed.timezone = "Asia/Urumqi";
    assert(store.replace_settings(changed, &error));

    DeviceTimeSyncRuntime sync;
    sync.state = "synced";
    sync.last_success_at = "2026-07-13T10:00:00Z";
    sync.last_offset_ms = 8;
    assert(store.update_time_sync(sync, &error));

    DeviceSettingsStore reloaded(path);
    assert(reloaded.initialize(&error));
    const auto snapshot = reloaded.snapshot();
    assert(!snapshot.settings.person_detection);
    assert(snapshot.settings.person_sensitivity == "high");
    assert(!snapshot.settings.status_led);
    assert(snapshot.settings.image_rotate180);
    assert(snapshot.settings.timezone == "Asia/Urumqi");
    assert(snapshot.runtime.time_sync.state == "synced");
    assert(snapshot.runtime.time_sync.last_offset_ms == 8);

    std::filesystem::remove_all(root, ec);
    return 0;
}
