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
        std::chrono::system_clock::time_point at,
        const std::string &snapshot_path,
        const std::string &clip_ref,
        double confidence,
        std::string *error = nullptr);

    std::optional<std::string> allocate_clip_ref(
        std::chrono::system_clock::time_point at,
        const std::string &media_root,
        std::string *error = nullptr);

    std::vector<EventRecord> pending_events(std::string *error = nullptr);
    bool set_snapshot_url(const std::string &event_id, const std::string &snapshot_url, std::string *error = nullptr);
    bool mark_reported(const std::string &event_id, std::string *error = nullptr);
    bool mark_unreportable(const std::string &event_id, std::string *error = nullptr);

private:
    bool initialize_schema(std::string *error);
    bool migrate_schema(std::string *error);

    sqlite3 *db_ = nullptr;
    std::mutex mutex_;
};
