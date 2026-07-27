#include "device/device_settings_store.h"

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <utility>

#if defined(_WIN32)
#include <io.h>
#include <windows.h>
#else
#include <unistd.h>
#endif

#include <nlohmann/json.hpp>

namespace {

std::string iso_utc_now() {
    std::time_t now = std::time(nullptr);
    std::tm tm{};
#if defined(_WIN32)
    gmtime_s(&tm, &now);
#else
    gmtime_r(&now, &tm);
#endif
    char buffer[32]{};
    std::strftime(buffer, sizeof(buffer), "%Y-%m-%dT%H:%M:%SZ", &tm);
    return buffer;
}

bool valid_sensitivity(const std::string &value) {
    return value == "low" || value == "medium" || value == "high";
}

bool valid_quality(const std::string &value) {
    return value == "360p" || value == "720p" ||
           value == "1080p" || value == "1440p";
}

nlohmann::json to_json(const DeviceSettingsSnapshot &snapshot) {
    return {
        {"version", snapshot.version},
        {"settings", {
            {"person_detection", snapshot.settings.person_detection},
            {"person_sensitivity", snapshot.settings.person_sensitivity},
            {"status_led", snapshot.settings.status_led},
            {"image_rotate180", snapshot.settings.image_rotate180},
            {"recording_quality", snapshot.settings.recording_quality},
            {"timezone", snapshot.settings.timezone},
        }},
        {"runtime", {{"time_sync", {
            {"state", snapshot.runtime.time_sync.state},
            {"last_success_at", snapshot.runtime.time_sync.last_success_at ? nlohmann::json(*snapshot.runtime.time_sync.last_success_at) : nlohmann::json(nullptr)},
            {"last_offset_ms", snapshot.runtime.time_sync.last_offset_ms ? nlohmann::json(*snapshot.runtime.time_sync.last_offset_ms) : nlohmann::json(nullptr)},
        }}}},
        {"updated_at", snapshot.updated_at},
    };
}

bool flush_file(FILE *file) {
    if (std::fflush(file) != 0) return false;
#if defined(_WIN32)
    return ::_commit(::_fileno(file)) == 0;
#else
    return ::fsync(::fileno(file)) == 0;
#endif
}

bool replace_file(const std::string &temporary, const std::string &target, std::error_code *error) {
#if defined(_WIN32)
    if (::MoveFileExA(temporary.c_str(), target.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        return true;
    }
    *error = std::error_code(static_cast<int>(::GetLastError()), std::system_category());
    return false;
#else
    std::filesystem::rename(temporary, target, *error);
    return !*error;
#endif
}

} // 命名空间

DeviceSettingsStore::DeviceSettingsStore(std::string path) : path_(std::move(path)) {}

bool DeviceSettingsStore::initialize(std::string *error) {
    std::lock_guard<std::mutex> lock(mutex_);
    std::error_code ec;
    std::filesystem::create_directories(std::filesystem::path(path_).parent_path(), ec);
    if (ec) {
        if (error) *error = ec.message();
        return false;
    }
    if (!std::filesystem::exists(path_)) {
        snapshot_ = DeviceSettingsSnapshot{};
        snapshot_.updated_at = iso_utc_now();
        return persist_locked(snapshot_, error);
    }
    try {
        std::ifstream input(path_);
        nlohmann::json body;
        input >> body;
        DeviceSettingsSnapshot loaded;
        loaded.version = body.value("version", 0);
        const auto &settings = body.at("settings");
        loaded.settings.person_detection = settings.at("person_detection").get<bool>();
        loaded.settings.person_sensitivity = settings.at("person_sensitivity").get<std::string>();
        loaded.settings.status_led = settings.at("status_led").get<bool>();
        loaded.settings.image_rotate180 = settings.at("image_rotate180").get<bool>();
        loaded.settings.recording_quality = settings.value("recording_quality", "1080p");
        loaded.settings.timezone = settings.at("timezone").get<std::string>();
        const auto &sync = body.at("runtime").at("time_sync");
        loaded.runtime.time_sync.state = sync.value("state", "unsynced");
        if (sync.contains("last_success_at") && !sync["last_success_at"].is_null()) {
            loaded.runtime.time_sync.last_success_at = sync["last_success_at"].get<std::string>();
        }
        if (sync.contains("last_offset_ms") && !sync["last_offset_ms"].is_null()) {
            loaded.runtime.time_sync.last_offset_ms = sync["last_offset_ms"].get<std::int64_t>();
        }
        loaded.updated_at = body.value("updated_at", "");
        if (loaded.version != 1 ||
            !valid_sensitivity(loaded.settings.person_sensitivity) ||
            !valid_quality(loaded.settings.recording_quality) ||
            loaded.settings.timezone.empty()) {
            if (error) *error = "invalid settings.json values";
            return false;
        }
        snapshot_ = std::move(loaded);
        return true;
    } catch (const std::exception &exception) {
        if (error) *error = exception.what();
        return false;
    }
}

DeviceSettingsSnapshot DeviceSettingsStore::snapshot() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return snapshot_;
}

bool DeviceSettingsStore::replace_settings(const DeviceSettingsValues &settings, std::string *error) {
    if (!valid_sensitivity(settings.person_sensitivity) ||
        !valid_quality(settings.recording_quality) ||
        settings.timezone.empty()) {
        if (error) *error = "invalid device settings";
        return false;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    auto next = snapshot_;
    next.settings = settings;
    next.updated_at = iso_utc_now();
    if (!persist_locked(next, error)) return false;
    snapshot_ = std::move(next);
    return true;
}

bool DeviceSettingsStore::update_time_sync(const DeviceTimeSyncRuntime &time_sync, std::string *error) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto next = snapshot_;
    next.runtime.time_sync = time_sync;
    next.updated_at = iso_utc_now();
    if (!persist_locked(next, error)) return false;
    snapshot_ = std::move(next);
    return true;
}

bool DeviceSettingsStore::persist_locked(const DeviceSettingsSnapshot &snapshot, std::string *error) {
    const std::string temporary = path_ + ".tmp";
    const std::string content = to_json(snapshot).dump(2) + "\n";
    FILE *file = std::fopen(temporary.c_str(), "wb");
    if (!file) {
        if (error) *error = std::strerror(errno);
        return false;
    }
    const bool wrote = std::fwrite(content.data(), 1, content.size(), file) == content.size();
    const bool flushed = wrote && flush_file(file);
    const int close_result = std::fclose(file);
    if (!flushed || close_result != 0) {
        std::filesystem::remove(temporary);
        if (error) *error = std::strerror(errno);
        return false;
    }
    std::error_code ec;
    if (!replace_file(temporary, path_, &ec)) {
        std::filesystem::remove(temporary);
        if (error) *error = ec.message();
        return false;
    }
    return true;
}
