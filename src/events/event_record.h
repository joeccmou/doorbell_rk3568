#pragma once

#include <cstdint>
#include <string>

struct EventRecord {
    std::string event_id;
    std::string device_id;
    std::string type;
    int64_t at_ms = 0;
    std::string occurred_timezone;
    int occurred_utc_offset_minutes = 0;
    std::string occurred_local_date;
    std::string snapshot_path;
    std::string snapshot_url;
    std::string clip_ref;
    std::string extra_json;
    bool reported = false;
};
