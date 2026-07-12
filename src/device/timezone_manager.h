#pragma once

#include <string>

struct TimezonePaths {
    std::string zoneinfo_root = "/usr/share/zoneinfo";
    std::string localtime_path = "/etc/localtime";
    std::string timezone_path = "/etc/timezone";
};

struct TimezoneApplyResult {
    bool ok = false;
    std::string timezone;
    std::string error_code;
};

class TimezoneManager {
public:
    explicit TimezoneManager(TimezonePaths paths = {});

    TimezoneApplyResult apply(const std::string &timezone) const;

private:
    TimezonePaths paths_;
};
