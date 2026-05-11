#include "settings.h"

#include <cstdio>
#include <string>
#include <vector>

namespace {
struct FormatOpt {
	const char *label;
	uint32_t fmt;
};

struct ResOpt {
	const char *label;
	uint32_t w;
	uint32_t h;
};

const FormatOpt kFormats[] = {
	{"UYVY", 0x59565955},    // V4L2_PIX_FMT_UYVY
	{"NV12", 0x3231564E},
	{"NV21", 0x3132564E},
};

const ResOpt kResolutions[] = {
	{"640x480", 640, 480},
	{"1280x800", 1280, 800},
	{"1440x900", 1440, 900},
	{"1920x1080", 1920, 1080},
};

struct UiState {
	SettingsHooks hooks{};
	lv_obj_t *panel = nullptr;
	lv_obj_t *fmt_dd = nullptr;
	lv_obj_t *res_dd = nullptr;
};

UiState *get_state(lv_obj_t *obj) {
	return static_cast<UiState *>(lv_obj_get_user_data(obj));
}

std::string build_options(const FormatOpt *opts, size_t n) {
	std::string s;
	for (size_t i = 0; i < n; ++i) {
		s += opts[i].label;
		if (i + 1 < n) s += "\n";
	}
	return s;
}

std::string build_options(const ResOpt *opts, size_t n) {
	std::string s;
	for (size_t i = 0; i < n; ++i) {
		s += opts[i].label;
		if (i + 1 < n) s += "\n";
	}
	return s;
}

void apply_cb(lv_event_t *e) {
	lv_obj_t *panel = static_cast<lv_obj_t *>(lv_event_get_user_data(e));
	auto *st = panel ? get_state(panel) : nullptr;
	std::fprintf(stdout, "[settings] apply clicked panel=%p state=%p restart=%p\n", static_cast<void *>(panel), static_cast<void *>(st), st ? reinterpret_cast<void *>(st->hooks.restart_camera) : nullptr);
	if (!st) {
		std::fprintf(stdout, "[settings] apply aborted: missing state\n");
		return;
	}
	if (!st->hooks.restart_camera) {
		std::fprintf(stdout, "[settings] apply aborted: restart hook not set\n");
		return;
	}

	uint16_t fidx = lv_dropdown_get_selected(st->fmt_dd);
	uint16_t ridx = lv_dropdown_get_selected(st->res_dd);
	std::fprintf(stdout, "[settings] apply selections fmt_idx=%u res_idx=%u\n", fidx, ridx);
	if (fidx >= (sizeof(kFormats) / sizeof(kFormats[0])) || ridx >= (sizeof(kResolutions) / sizeof(kResolutions[0]))) {
		std::fprintf(stdout, "[settings] apply aborted: index out of range fmt_count=%zu res_count=%zu\n", sizeof(kFormats) / sizeof(kFormats[0]), sizeof(kResolutions) / sizeof(kResolutions[0]));
		return;
	}

	const auto &fmt = kFormats[fidx];
	const auto &res = kResolutions[ridx];
	bool ok = st->hooks.restart_camera(st->hooks.user_ctx, fmt.fmt, res.w, res.h);
	std::fprintf(stdout, "[settings] apply fmt=%s res=%s %s\n", fmt.label, res.label, ok ? "ok" : "fail");
	if (ok) {
		lv_obj_add_flag(panel, LV_OBJ_FLAG_HIDDEN);
	}
}

void close_cb(lv_event_t *e) {
	lv_obj_t *panel = static_cast<lv_obj_t *>(lv_event_get_user_data(e));
	if (panel) lv_obj_add_flag(panel, LV_OBJ_FLAG_HIDDEN);
}

void open_cb(lv_event_t *e) {
	auto *st = static_cast<UiState *>(lv_event_get_user_data(e));
	if (!st || !st->panel) return;
	lv_obj_clear_flag(st->panel, LV_OBJ_FLAG_HIDDEN);
}
}

void settings_init(lv_obj_t *parent, SettingsHooks hooks) {
	static UiState state;
	state.hooks = hooks;

	// Settings button
	lv_obj_t *btn = lv_button_create(parent);
	lv_obj_align(btn, LV_ALIGN_TOP_RIGHT, -10, 10);
	lv_obj_t *lbl = lv_label_create(btn);
	lv_label_set_text(lbl, "Settings");
	lv_obj_center(lbl);
	lv_obj_add_event_cb(btn, open_cb, LV_EVENT_CLICKED, &state);

	// Panel (hidden initially)
	state.panel = lv_obj_create(parent);
	lv_obj_set_size(state.panel, 260, 180);
	lv_obj_center(state.panel);
	lv_obj_add_flag(state.panel, LV_OBJ_FLAG_HIDDEN);
	lv_obj_set_user_data(state.panel, &state);

	// Format dropdown
	lv_obj_t *fmt_label = lv_label_create(state.panel);
	lv_label_set_text(fmt_label, "Format");
	lv_obj_align(fmt_label, LV_ALIGN_TOP_LEFT, 10, 10);

	state.fmt_dd = lv_dropdown_create(state.panel);
	auto fmt_opts = build_options(kFormats, sizeof(kFormats) / sizeof(kFormats[0]));
	lv_dropdown_set_options(state.fmt_dd, fmt_opts.c_str());
	lv_obj_set_width(state.fmt_dd, 120);
	lv_obj_align_to(state.fmt_dd, fmt_label, LV_ALIGN_OUT_RIGHT_MID, 10, 0);
	lv_dropdown_set_selected(state.fmt_dd, 0); // default UYVY

	// Resolution dropdown
	lv_obj_t *res_label = lv_label_create(state.panel);
	lv_label_set_text(res_label, "Resolution");
	lv_obj_align(res_label, LV_ALIGN_TOP_LEFT, 10, 60);

	state.res_dd = lv_dropdown_create(state.panel);
	auto res_opts = build_options(kResolutions, sizeof(kResolutions) / sizeof(kResolutions[0]));
	lv_dropdown_set_options(state.res_dd, res_opts.c_str());
	lv_obj_set_width(state.res_dd, 120);
	lv_obj_align_to(state.res_dd, res_label, LV_ALIGN_OUT_RIGHT_MID, 10, 0);
	lv_dropdown_set_selected(state.res_dd, 1); // default 1280x720

	// Buttons
	lv_obj_t *apply_btn = lv_button_create(state.panel);
	lv_obj_align(apply_btn, LV_ALIGN_BOTTOM_RIGHT, -10, -10);
	// Pass panel via event user_data to ensure handler can find state even if object user data is unchanged.
	lv_obj_add_event_cb(apply_btn, apply_cb, LV_EVENT_CLICKED, state.panel);
	lv_obj_t *apply_lbl = lv_label_create(apply_btn);
	lv_label_set_text(apply_lbl, "Apply");
	lv_obj_center(apply_lbl);

	lv_obj_t *close_btn = lv_button_create(state.panel);
	lv_obj_align(close_btn, LV_ALIGN_BOTTOM_LEFT, 10, -10);
	lv_obj_add_event_cb(close_btn, close_cb, LV_EVENT_CLICKED, state.panel);
	lv_obj_t *close_lbl = lv_label_create(close_btn);
	lv_label_set_text(close_lbl, "Close");
	lv_obj_center(close_lbl);
}
