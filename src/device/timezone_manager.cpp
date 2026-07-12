#include "device/timezone_manager.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

namespace {

bool is_safe_timezone_name(const std::string &value) {
    if (value.empty() || value.front() == '/' || value.front() == '\\' ||
        value.find("..") != std::string::npos || value.find('\\') != std::string::npos) {
        return false;
    }
    const auto slash = value.find('/');
    if (slash == std::string::npos || slash == 0 || slash + 1 >= value.size()) {
        return false;
    }
    for (const unsigned char ch : value) {
        const bool allowed = (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
                             (ch >= '0' && ch <= '9') || ch == '/' || ch == '_' ||
                             ch == '-' || ch == '+' || ch == '.';
        if (!allowed) return false;
    }
    return true;
}

bool path_is_below(const std::filesystem::path &path, const std::filesystem::path &root) {
    auto path_it = path.begin();
    auto root_it = root.begin();
    for (; root_it != root.end(); ++root_it, ++path_it) {
        if (path_it == path.end() || *path_it != *root_it) return false;
    }
    return true;
}

bool copy_file_contents(const std::filesystem::path &source,
                        const std::filesystem::path &target,
                        std::error_code *error) {
    std::ifstream input(source, std::ios::binary);
    std::ofstream output(target, std::ios::binary | std::ios::trunc);
    if (!input || !output) return false;
    output << input.rdbuf();
    output.flush();
    if (!input.good() && !input.eof()) return false;
    if (!output.good()) return false;
    if (error) error->clear();
    return true;
}

bool write_text(const std::filesystem::path &target, const std::string &value) {
    std::ofstream output(target, std::ios::binary | std::ios::trunc);
    if (!output) return false;
    output << value;
    output.flush();
    return output.good();
}

bool replace_with_backup(const std::filesystem::path &target,
                         const std::filesystem::path &replacement,
                         const std::filesystem::path &backup,
                         bool *had_original) {
    std::error_code ec;
    const bool target_exists = std::filesystem::exists(target, ec);
    if (ec) return false;
    const bool backup_exists = std::filesystem::exists(backup, ec);
    if (ec) return false;
    // 上次若在替换中途掉电，优先把备份恢复成可用文件。
    if (backup_exists && !target_exists) {
        std::filesystem::rename(backup, target, ec);
        if (ec) return false;
    } else if (backup_exists) {
        std::filesystem::remove(backup, ec);
        if (ec) return false;
    }
    ec.clear();
    *had_original = std::filesystem::exists(target, ec);
    if (ec) return false;
    if (*had_original) {
        std::filesystem::rename(target, backup, ec);
        if (ec) return false;
    }
    std::filesystem::rename(replacement, target, ec);
    if (!ec) return true;
    if (*had_original) {
        std::error_code rollback_ec;
        std::filesystem::rename(backup, target, rollback_ec);
    }
    return false;
}

void restore_backup(const std::filesystem::path &target,
                    const std::filesystem::path &backup,
                    bool had_original) {
    std::error_code ec;
    std::filesystem::remove(target, ec);
    if (had_original) {
        ec.clear();
        std::filesystem::rename(backup, target, ec);
    }
}

}  // namespace

TimezoneManager::TimezoneManager(TimezonePaths paths) : paths_(std::move(paths)) {}

TimezoneApplyResult TimezoneManager::apply(const std::string &timezone) const {
    TimezoneApplyResult result;
    result.timezone = timezone;
    result.error_code = "TIMEZONE_INVALID";

    if (!is_safe_timezone_name(timezone)) return result;

    std::error_code ec;
    const auto zoneinfo_root = std::filesystem::weakly_canonical(paths_.zoneinfo_root, ec);
    if (ec) return result;
    const auto source = std::filesystem::weakly_canonical(zoneinfo_root / timezone, ec);
    if (ec || !path_is_below(source, zoneinfo_root) || !std::filesystem::is_regular_file(source, ec) || ec) {
        return result;
    }

    const std::filesystem::path localtime(paths_.localtime_path);
    const std::filesystem::path timezone_file(paths_.timezone_path);
    const auto localtime_tmp = localtime.string() + ".doorbell-tmp";
    const auto timezone_tmp = timezone_file.string() + ".doorbell-tmp";
    const auto localtime_backup = localtime.string() + ".doorbell-backup";
    const auto timezone_backup = timezone_file.string() + ".doorbell-backup";

    std::filesystem::create_directories(localtime.parent_path(), ec);
    if (ec) {
        result.error_code = "TIMEZONE_APPLY_FAILED";
        return result;
    }
    std::filesystem::create_directories(timezone_file.parent_path(), ec);
    if (ec || !copy_file_contents(source, localtime_tmp, &ec) || !write_text(timezone_tmp, timezone + "\n")) {
        std::filesystem::remove(localtime_tmp, ec);
        std::filesystem::remove(timezone_tmp, ec);
        result.error_code = "TIMEZONE_APPLY_FAILED";
        return result;
    }

    bool had_localtime = false;
    if (!replace_with_backup(localtime, localtime_tmp, localtime_backup, &had_localtime)) {
        std::filesystem::remove(timezone_tmp, ec);
        result.error_code = "TIMEZONE_APPLY_FAILED";
        return result;
    }
    bool had_timezone = false;
    if (!replace_with_backup(timezone_file, timezone_tmp, timezone_backup, &had_timezone)) {
        restore_backup(localtime, localtime_backup, had_localtime);
        result.error_code = "TIMEZONE_APPLY_FAILED";
        return result;
    }

    std::filesystem::remove(localtime_backup, ec);
    std::filesystem::remove(timezone_backup, ec);
#ifdef _WIN32
    _putenv_s("TZ", timezone.c_str());
    _tzset();
#else
    unsetenv("TZ");
    tzset();
#endif
    result.ok = true;
    result.error_code.clear();
    return result;
}
