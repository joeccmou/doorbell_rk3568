#pragma once

#include <cstdint>
#include <lvgl.h>

struct SettingsHooks {
    void *user_ctx = nullptr;
    // Return true on success, false on failure
    bool (*restart_camera)(void *user_ctx, uint32_t pixfmt, uint32_t width, uint32_t height) = nullptr;
};

void settings_init(lv_obj_t *parent, SettingsHooks hooks);
