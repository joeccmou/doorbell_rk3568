#include "events/event_paths.h"

#include <cstdio>
#include <cctype>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <regex>
#include <sstream>

namespace {
struct UtcParts {
    std::string date;
    std::string time;
    int milliseconds = 0;
};

UtcParts utc_parts(std::chrono::system_clock::time_point at) {
    using namespace std::chrono;
    const auto epoch_ms = duration_cast<milliseconds>(at.time_since_epoch());
    const auto seconds_since_epoch = duration_cast<seconds>(epoch_ms);
    const int millis = static_cast<int>((epoch_ms - seconds_since_epoch).count());
    std::time_t value = system_clock::to_time_t(system_clock::time_point(seconds_since_epoch));
    std::tm utc{};
#ifdef _WIN32
    gmtime_s(&utc, &value);
#else
    gmtime_r(&value, &utc);
#endif
    char date[9]{};
    char time[7]{};
    std::strftime(date, sizeof(date), "%Y%m%d", &utc);
    std::strftime(time, sizeof(time), "%H%M%S", &utc);
    return {date, time, millis};
}

UtcParts local_parts(std::chrono::system_clock::time_point at) {
    using namespace std::chrono;
    const auto epoch_ms = duration_cast<milliseconds>(at.time_since_epoch());
    const auto seconds_since_epoch = duration_cast<seconds>(epoch_ms);
    const int millis = static_cast<int>((epoch_ms - seconds_since_epoch).count());
    std::time_t value = system_clock::to_time_t(system_clock::time_point(seconds_since_epoch));
    std::tm local{};
#ifdef _WIN32
    localtime_s(&local, &value);
#else
    localtime_r(&value, &local);
#endif
    char date[9]{};
    char time[7]{};
    std::strftime(date, sizeof(date), "%Y%m%d", &local);
    std::strftime(time, sizeof(time), "%H%M%S", &local);
    return {date, time, millis};
}

std::string timestamp_name(std::chrono::system_clock::time_point at) {
    const auto parts = local_parts(at);
    std::ostringstream out;
    out << parts.date << '-' << parts.time << std::setw(3) << std::setfill('0') << parts.milliseconds;
    return out.str();
}

std::string trim(std::string value) {
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back()))) value.pop_back();
    size_t first = 0;
    while (first < value.size() && std::isspace(static_cast<unsigned char>(value[first]))) ++first;
    return value.substr(first);
}
}

std::string format_event_id(std::chrono::system_clock::time_point at, uint64_t sequence) {
    const auto parts = utc_parts(at);
    std::ostringstream out;
    out << parts.date << 'T' << parts.time << std::setw(3) << std::setfill('0') << parts.milliseconds
        << "Z-" << std::setw(7) << std::setfill('0') << sequence;
    return out.str();
}

std::string snapshot_relative_path(std::chrono::system_clock::time_point at) {
    const auto parts = local_parts(at);
    return "snapshots/" + parts.date + "/" + timestamp_name(at) + ".jpg";
}

std::string clip_directory_relative_path(std::chrono::system_clock::time_point at) {
    return "clips/" + local_parts(at).date;
}

std::string local_date_iso(std::chrono::system_clock::time_point at) {
    const auto compact = local_parts(at).date;
    return compact.substr(0, 4) + "-" + compact.substr(4, 2) + "-" + compact.substr(6, 2);
}

std::string system_timezone_name() {
    if (const char *tz = std::getenv("TZ")) {
        const std::string value = trim(tz);
        if (!value.empty() && value.find('/') != std::string::npos) return value;
    }
    std::ifstream timezone("/etc/timezone");
    std::string value;
    if (timezone && std::getline(timezone, value)) {
        value = trim(value);
        if (!value.empty()) return value;
    }
    std::error_code ec;
    const auto target = std::filesystem::read_symlink("/etc/localtime", ec).generic_string();
    const std::string marker = "/usr/share/zoneinfo/";
    const auto position = target.find(marker);
    if (!ec && position != std::string::npos) return target.substr(position + marker.size());
    return "Asia/Shanghai";
}

int utc_offset_minutes(std::chrono::system_clock::time_point at) {
    const std::time_t value = std::chrono::system_clock::to_time_t(at);
    std::tm local{};
    std::tm utc{};
#ifdef _WIN32
    localtime_s(&local, &value);
    gmtime_s(&utc, &value);
#else
    localtime_r(&value, &local);
    gmtime_r(&value, &utc);
#endif
    local.tm_isdst = -1;
    utc.tm_isdst = -1;
    const std::time_t local_epoch = std::mktime(&local);
    const std::time_t utc_as_local_epoch = std::mktime(&utc);
    return static_cast<int>(std::difftime(local_epoch, utc_as_local_epoch) / 60.0);
}

std::string clip_relative_path(std::chrono::system_clock::time_point at, uint64_t sequence) {
    const auto parts = local_parts(at);
    std::ostringstream name;
    name << parts.date << 'T' << parts.time << std::setw(3) << std::setfill('0') << parts.milliseconds
         << '-' << std::setw(7) << std::setfill('0') << sequence << ".mp4";
    return "clips/" + parts.date + "/" + name.str();
}

std::string recording_segment_relative_path(
    std::chrono::system_clock::time_point recording_started_at,
    uint32_t segment_index) {
    std::ostringstream name;
    name << timestamp_name(recording_started_at) << '-'
         << std::setw(6) << std::setfill('0') << segment_index << ".mp4";
    return clip_directory_relative_path(recording_started_at) + "/" + name.str();
}

bool is_canonical_clip_ref(const std::string &value) {
    static const std::regex pattern(R"(^clips/[0-9]{8}/[^/\\]+\.mp4$)");
    if (!std::regex_match(value, pattern)) return false;
    return std::filesystem::path(value).lexically_normal().generic_string() == value;
}

bool is_canonical_snapshot_path(const std::string &value) {
    static const std::regex pattern(R"(^snapshots/([0-9]{8})/([0-9]{8})-[0-9]{9}\.jpg$)");
    std::smatch matches;
    return std::regex_match(value, matches, pattern) && matches.size() == 3 && matches[1] == matches[2] &&
           std::filesystem::path(value).lexically_normal().generic_string() == value;
}
