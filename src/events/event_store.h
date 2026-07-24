#pragma once

#include "events/event_record.h"

#include <chrono>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

struct sqlite3;

class EventStore {
public:
    EventStore() = default;
    ~EventStore();

    EventStore(const EventStore &) = delete;
    EventStore &operator=(const EventStore &) = delete;

    bool open(const std::string &database_path, std::string *error = nullptr);
    void close();

    std::optional<EventRecord> create_person_event(
        const std::string &device_id,
        const std::string &recording_id,
        std::chrono::system_clock::time_point at,
        const std::string &snapshot_path,
        double confidence,
        std::string *error = nullptr);

    std::optional<RingPressRecord> record_ring_press(
        const std::string &device_id,
        const std::string &recording_id,
        std::chrono::system_clock::time_point at,
        const std::string &snapshot_path,
        std::string *error = nullptr);
    std::optional<std::string> expire_open_ring(
        std::chrono::system_clock::time_point at,
        std::string *error = nullptr);
    // 进程重启后媒体管线已经不存在，本地 accepted 事件必须先收敛，避免新按铃复用旧事件。
    bool recover_stale_accepted_ring(
        std::string *event_id,
        std::string *error = nullptr);
    std::vector<RingPressRecord> pending_ring_presses(std::string *error = nullptr);
    bool mark_ring_press_reported(
        const std::string &event_id,
        int press_seq,
        std::string *error = nullptr);
    bool update_ring_state(
        const std::string &event_id,
        const std::string &state,
        std::string *error = nullptr);

    bool begin_recording(
        const std::string &recording_id,
        std::chrono::system_clock::time_point started_at,
        std::string *error = nullptr);

    bool append_recording_segment(
        const std::string &recording_id,
        const std::string &segment_id,
        int segment_index,
        const std::string &clip_ref,
        std::chrono::system_clock::time_point started_at,
        std::chrono::system_clock::time_point ended_at,
        int64_t media_duration_ms,
        int64_t size_bytes,
        const std::string &sha256,
        std::string *error = nullptr);

    bool finalize_recording(
        const std::string &recording_id,
        std::chrono::system_clock::time_point ended_at,
        const std::string &status,
        std::string *error = nullptr);

    bool complete_recording(
        const std::string &recording_id,
        const std::string &segment_id,
        int segment_index,
        const std::string &clip_ref,
        std::chrono::system_clock::time_point started_at,
        std::chrono::system_clock::time_point ended_at,
        int64_t media_duration_ms,
        int64_t size_bytes,
        const std::string &sha256,
        const std::string &status,
        std::string *error = nullptr);

    std::optional<std::string> allocate_clip_ref(
        std::chrono::system_clock::time_point at,
        const std::string &media_root,
        std::string *error = nullptr);

    std::vector<EventRecord> pending_events(std::string *error = nullptr);
    std::optional<EventRecord> find_event(const std::string &event_id, std::string *error = nullptr);
    std::optional<RecordingRecord> find_recording(
        const std::string &recording_id,
        std::string *error = nullptr);
    std::vector<RecordingSegmentRecord> recording_segments(
        const std::string &recording_id,
        std::string *error = nullptr);
    bool update_segment_sha256(
        const std::string &segment_id,
        const std::string &sha256,
        std::string *error = nullptr);
    std::optional<int> report_state(const std::string &event_id, std::string *error = nullptr);
    bool mark_reported(const std::string &event_id, std::string *error = nullptr);
    bool mark_unreportable(const std::string &event_id, std::string *error = nullptr);

private:
    bool initialize_schema(std::string *error);
    bool migrate_schema(std::string *error);

    sqlite3 *db_ = nullptr;
    std::mutex mutex_;
};
