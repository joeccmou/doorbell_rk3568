#pragma once

#include <chrono>
#include <cstdint>
#include <string>

std::string format_event_id(std::chrono::system_clock::time_point at, uint64_t sequence);
std::string snapshot_relative_path(std::chrono::system_clock::time_point at);
std::string clip_directory_relative_path(std::chrono::system_clock::time_point at);
std::string local_date_iso(std::chrono::system_clock::time_point at);
std::string system_timezone_name();
int utc_offset_minutes(std::chrono::system_clock::time_point at);
std::string clip_relative_path(std::chrono::system_clock::time_point at, uint64_t sequence);
std::string recording_segment_relative_path(
    std::chrono::system_clock::time_point recording_started_at,
    uint32_t segment_index);
bool is_canonical_clip_ref(const std::string &value);
bool is_canonical_snapshot_path(const std::string &value);
