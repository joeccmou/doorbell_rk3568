#pragma once

#include <cstdint>
#include <string>

struct EventRecord {
    std::string event_id;
    std::string device_id;
    std::string recording_id;
    std::string type;
    int64_t at_ms = 0;
    std::string occurred_timezone;
    int occurred_utc_offset_minutes = 0;
    std::string occurred_local_date;
    std::string snapshot_path;
    std::string snapshot_url;
    std::string extra_json;
    int reported = 0;
};

struct RecordingRecord {
    std::string recording_id;
    std::string status;
    int64_t started_at_ms = 0;
    int64_t ended_at_ms = 0;
    std::string recorded_timezone;
    int started_utc_offset_minutes = 0;
    std::string started_local_date;
    int segment_count = 0;
    int64_t total_media_duration_ms = 0;
};

struct RecordingSegmentRecord {
    std::string segment_id;
    std::string recording_id;
    int segment_index = 0;
    std::string clip_ref;
    int64_t started_at_ms = 0;
    int64_t ended_at_ms = 0;
    int64_t media_duration_ms = 0;
    int64_t size_bytes = 0;
    std::string sha256;
};

struct RingPressRecord {
    EventRecord event;
    std::string ring_state;
    int press_seq = 1;
    int64_t pressed_at_ms = 0;
    bool initial = false;
};
