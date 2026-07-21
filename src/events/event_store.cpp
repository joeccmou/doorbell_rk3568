#include "events/event_store.h"

#include "events/event_paths.h"

#include <sqlite3.h>

#include <filesystem>
#include <iomanip>
#include <sstream>
#include <utility>

namespace {
void set_error(std::string *out, sqlite3 *db, const char *fallback) {
    if (!out) return;
    *out = db ? sqlite3_errmsg(db) : fallback;
}

bool exec_sql(sqlite3 *db, const char *sql, std::string *error) {
    char *message = nullptr;
    const int rc = sqlite3_exec(db, sql, nullptr, nullptr, &message);
    if (rc == SQLITE_OK) return true;
    if (error) *error = message ? message : sqlite3_errmsg(db);
    sqlite3_free(message);
    return false;
}

std::string column_text(sqlite3_stmt *statement, int index) {
    const auto *value = sqlite3_column_text(statement, index);
    return value ? reinterpret_cast<const char *>(value) : std::string();
}

bool scalar_int(sqlite3 *db, const char *sql, int *value, std::string *error) {
    sqlite3_stmt *statement = nullptr;
    if (sqlite3_prepare_v2(db, sql, -1, &statement, nullptr) != SQLITE_OK ||
        sqlite3_step(statement) != SQLITE_ROW) {
        set_error(error, db, "read scalar failed");
        sqlite3_finalize(statement);
        return false;
    }
    *value = sqlite3_column_int(statement, 0);
    sqlite3_finalize(statement);
    return true;
}
}

EventStore::~EventStore() {
    close();
}

bool EventStore::open(const std::string &database_path, std::string *error) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (db_) return true;
    std::error_code ec;
    std::filesystem::create_directories(std::filesystem::path(database_path).parent_path(), ec);
    if (ec) {
        if (error) *error = ec.message();
        return false;
    }
    if (sqlite3_open_v2(database_path.c_str(), &db_, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX, nullptr) != SQLITE_OK) {
        set_error(error, db_, "sqlite open failed");
        if (db_) sqlite3_close(db_);
        db_ = nullptr;
        return false;
    }
    sqlite3_busy_timeout(db_, 3000);
    if (!initialize_schema(error)) {
        sqlite3_close(db_);
        db_ = nullptr;
        return false;
    }
    return true;
}

void EventStore::close() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!db_) return;
    sqlite3_close(db_);
    db_ = nullptr;
}

bool EventStore::initialize_schema(std::string *error) {
    int version = 0;
    if (!scalar_int(db_, "PRAGMA user_version", &version, error)) return false;
    if (version != 0 && version != 3 && version != 4) {
        if (error) {
            *error = "unsupported event database schema version: " + std::to_string(version) +
                     "; stop service and remove doorbell.db, doorbell.db-wal and doorbell.db-shm";
        }
        return false;
    }
    static const char *sql = R"SQL(
PRAGMA journal_mode=WAL;
PRAGMA synchronous=NORMAL;
PRAGMA foreign_keys=ON;
CREATE TABLE IF NOT EXISTS event_meta (
    key TEXT PRIMARY KEY,
    value INTEGER NOT NULL
);
INSERT OR IGNORE INTO event_meta(key, value) VALUES('event_sequence', 0);
CREATE TABLE IF NOT EXISTS events (
    event_id TEXT PRIMARY KEY,
    device_id TEXT NOT NULL,
    recording_id TEXT NOT NULL,
    event_type TEXT NOT NULL,
    at_ms INTEGER NOT NULL,
    occurred_timezone TEXT NOT NULL,
    occurred_utc_offset_minutes INTEGER NOT NULL,
    occurred_local_date TEXT NOT NULL,
    snapshot_path TEXT NOT NULL,
    extra_json TEXT NOT NULL DEFAULT '{}',
    reported INTEGER NOT NULL DEFAULT 0,
    created_at_ms INTEGER NOT NULL,
    FOREIGN KEY(recording_id) REFERENCES recordings(recording_id)
);
CREATE INDEX IF NOT EXISTS idx_events_pending ON events(reported, at_ms);
CREATE TABLE IF NOT EXISTS recordings (
    recording_id TEXT PRIMARY KEY,
    status TEXT NOT NULL,
    started_at_ms INTEGER NOT NULL,
    ended_at_ms INTEGER,
    recorded_timezone TEXT NOT NULL,
    started_utc_offset_minutes INTEGER NOT NULL,
    started_local_date TEXT NOT NULL,
    segment_count INTEGER NOT NULL DEFAULT 0,
    total_media_duration_ms INTEGER NOT NULL DEFAULT 0,
    created_at_ms INTEGER NOT NULL
);
CREATE TABLE IF NOT EXISTS recording_segments (
    segment_id TEXT PRIMARY KEY,
    recording_id TEXT NOT NULL,
    segment_index INTEGER NOT NULL,
    clip_ref TEXT NOT NULL,
    started_at_ms INTEGER NOT NULL,
    ended_at_ms INTEGER NOT NULL,
    media_duration_ms INTEGER NOT NULL,
    size_bytes INTEGER NOT NULL,
    sha256 TEXT NOT NULL,
    UNIQUE(recording_id, segment_index),
    FOREIGN KEY(recording_id) REFERENCES recordings(recording_id) ON DELETE CASCADE
);
CREATE TABLE IF NOT EXISTS clip_daily_sequences (
    local_date TEXT PRIMARY KEY,
    last_sequence INTEGER NOT NULL
);
CREATE INDEX IF NOT EXISTS idx_events_local_date
    ON events(occurred_local_date, at_ms DESC, event_id DESC);
)SQL";
    if (!exec_sql(db_, sql, error) || !migrate_schema(error)) return false;
    // 上次异常断电留下的 recording 不再伪装成“仍在录像”；保留事件并明确无可播放分片。
    return exec_sql(
        db_,
        "UPDATE recordings SET status='failed', ended_at_ms=COALESCE(ended_at_ms,started_at_ms) "
        "WHERE status='recording'",
        error);
}

bool EventStore::migrate_schema(std::string *error) {
    int version = 0;
    if (!scalar_int(db_, "PRAGMA user_version", &version, error)) return false;
    if (version == 0) {
        return exec_sql(db_, "PRAGMA user_version=4", error);
    }
    if (version == 3) {
        return exec_sql(
            db_,
            "ALTER TABLE recording_segments "
            "ADD COLUMN sha256 TEXT NOT NULL DEFAULT '';"
            "PRAGMA user_version=4;",
            error);
    }
    return version == 4;
}

std::optional<EventRecord> EventStore::create_person_event(
    const std::string &device_id,
    const std::string &recording_id,
    std::chrono::system_clock::time_point at,
    const std::string &snapshot_path,
    double confidence,
    std::string *error) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!db_) {
        if (error) *error = "event database is not open";
        return std::nullopt;
    }
    if (!exec_sql(db_, "BEGIN IMMEDIATE", error)) return std::nullopt;
    bool committed = false;
    auto rollback = [&] {
        if (!committed) sqlite3_exec(db_, "ROLLBACK", nullptr, nullptr, nullptr);
    };

    if (!exec_sql(db_, "UPDATE event_meta SET value=value+1 WHERE key='event_sequence'", error)) {
        rollback();
        return std::nullopt;
    }
    sqlite3_stmt *sequence_statement = nullptr;
    if (sqlite3_prepare_v2(db_, "SELECT value FROM event_meta WHERE key='event_sequence'", -1, &sequence_statement, nullptr) != SQLITE_OK ||
        sqlite3_step(sequence_statement) != SQLITE_ROW) {
        set_error(error, db_, "read event sequence failed");
        sqlite3_finalize(sequence_statement);
        rollback();
        return std::nullopt;
    }
    const uint64_t sequence = static_cast<uint64_t>(sqlite3_column_int64(sequence_statement, 0));
    sqlite3_finalize(sequence_statement);

    const int64_t at_ms = std::chrono::duration_cast<std::chrono::milliseconds>(at.time_since_epoch()).count();
    EventRecord event;
    event.event_id = format_event_id(at, sequence);
    event.device_id = device_id;
    event.recording_id = recording_id;
    event.type = "person";
    event.at_ms = at_ms;
    event.occurred_timezone = system_timezone_name();
    event.occurred_utc_offset_minutes = utc_offset_minutes(at);
    event.occurred_local_date = local_date_iso(at);
    event.snapshot_path = snapshot_path;
    std::ostringstream extra;
    extra << "{\"confidence\":" << std::setprecision(15) << confidence << '}';
    event.extra_json = extra.str();

    sqlite3_stmt *insert = nullptr;
    const char *sql = R"SQL(
INSERT INTO events(event_id, device_id, recording_id, event_type, at_ms, occurred_timezone,
                   occurred_utc_offset_minutes, occurred_local_date, snapshot_path, extra_json,
                   reported, created_at_ms)
VALUES(?, ?, ?, ?, ?, ?, ?, ?, ?, ?, 0, ?)
)SQL";
    if (sqlite3_prepare_v2(db_, sql, -1, &insert, nullptr) != SQLITE_OK) {
        set_error(error, db_, "prepare event insert failed");
        rollback();
        return std::nullopt;
    }
    sqlite3_bind_text(insert, 1, event.event_id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(insert, 2, event.device_id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(insert, 3, event.recording_id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(insert, 4, event.type.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(insert, 5, at_ms);
    sqlite3_bind_text(insert, 6, event.occurred_timezone.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(insert, 7, event.occurred_utc_offset_minutes);
    sqlite3_bind_text(insert, 8, event.occurred_local_date.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(insert, 9, event.snapshot_path.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(insert, 10, event.extra_json.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(insert, 11, at_ms);
    const int insert_rc = sqlite3_step(insert);
    sqlite3_finalize(insert);
    if (insert_rc != SQLITE_DONE || !exec_sql(db_, "COMMIT", error)) {
        set_error(error, db_, "insert event failed");
        rollback();
        return std::nullopt;
    }
    committed = true;
    return event;
}

bool EventStore::begin_recording(
    const std::string &recording_id,
    std::chrono::system_clock::time_point started_at,
    std::string *error) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!db_) {
        if (error) *error = "event database is not open";
        return false;
    }
    const int64_t started_at_ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(started_at.time_since_epoch()).count();
    sqlite3_stmt *statement = nullptr;
    const char *sql = R"SQL(
INSERT INTO recordings(
    recording_id, status, started_at_ms, ended_at_ms, recorded_timezone,
    started_utc_offset_minutes, started_local_date, segment_count,
    total_media_duration_ms, created_at_ms)
VALUES(?, 'recording', ?, NULL, ?, ?, ?, 0, 0, ?)
)SQL";
    if (sqlite3_prepare_v2(db_, sql, -1, &statement, nullptr) != SQLITE_OK) {
        set_error(error, db_, "prepare recording insert failed");
        return false;
    }
    sqlite3_bind_text(statement, 1, recording_id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(statement, 2, started_at_ms);
    const std::string timezone = system_timezone_name();
    const std::string local_date = local_date_iso(started_at);
    sqlite3_bind_text(statement, 3, timezone.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(statement, 4, utc_offset_minutes(started_at));
    sqlite3_bind_text(statement, 5, local_date.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(statement, 6, started_at_ms);
    const bool ok = sqlite3_step(statement) == SQLITE_DONE;
    if (!ok) set_error(error, db_, "insert recording failed");
    sqlite3_finalize(statement);
    return ok;
}

bool EventStore::complete_recording(
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
    std::string *error) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!db_ || !exec_sql(db_, "BEGIN IMMEDIATE", error)) return false;
    bool committed = false;
    auto rollback = [&] {
        if (!committed) sqlite3_exec(db_, "ROLLBACK", nullptr, nullptr, nullptr);
    };
    const int64_t started_at_ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(started_at.time_since_epoch()).count();
    const int64_t ended_at_ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(ended_at.time_since_epoch()).count();
    sqlite3_stmt *segment = nullptr;
    const char *segment_sql = R"SQL(
INSERT INTO recording_segments(
    segment_id, recording_id, segment_index, clip_ref, started_at_ms, ended_at_ms,
    media_duration_ms, size_bytes, sha256)
VALUES(?, ?, ?, ?, ?, ?, ?, ?, ?)
)SQL";
    if (sqlite3_prepare_v2(db_, segment_sql, -1, &segment, nullptr) != SQLITE_OK) {
        set_error(error, db_, "prepare recording segment insert failed");
        rollback();
        return false;
    }
    sqlite3_bind_text(segment, 1, segment_id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(segment, 2, recording_id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(segment, 3, segment_index);
    sqlite3_bind_text(segment, 4, clip_ref.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(segment, 5, started_at_ms);
    sqlite3_bind_int64(segment, 6, ended_at_ms);
    sqlite3_bind_int64(segment, 7, media_duration_ms);
    sqlite3_bind_int64(segment, 8, size_bytes);
    sqlite3_bind_text(segment, 9, sha256.c_str(), -1, SQLITE_TRANSIENT);
    const bool segment_ok = sqlite3_step(segment) == SQLITE_DONE;
    sqlite3_finalize(segment);
    if (!segment_ok) {
        set_error(error, db_, "insert recording segment failed");
        rollback();
        return false;
    }

    sqlite3_stmt *recording = nullptr;
    const char *recording_sql = R"SQL(
UPDATE recordings
SET status=?, ended_at_ms=?, segment_count=segment_count+1,
    total_media_duration_ms=total_media_duration_ms+?
WHERE recording_id=? AND status='recording'
)SQL";
    if (sqlite3_prepare_v2(db_, recording_sql, -1, &recording, nullptr) != SQLITE_OK) {
        set_error(error, db_, "prepare recording completion failed");
        rollback();
        return false;
    }
    sqlite3_bind_text(recording, 1, status.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(recording, 2, ended_at_ms);
    sqlite3_bind_int64(recording, 3, media_duration_ms);
    sqlite3_bind_text(recording, 4, recording_id.c_str(), -1, SQLITE_TRANSIENT);
    const bool recording_ok = sqlite3_step(recording) == SQLITE_DONE && sqlite3_changes(db_) == 1;
    sqlite3_finalize(recording);
    if (!recording_ok || !exec_sql(db_, "COMMIT", error)) {
        if (recording_ok) set_error(error, db_, "commit recording completion failed");
        rollback();
        return false;
    }
    committed = true;
    return true;
}

bool EventStore::append_recording_segment(
    const std::string &recording_id,
    const std::string &segment_id,
    int segment_index,
    const std::string &clip_ref,
    std::chrono::system_clock::time_point started_at,
    std::chrono::system_clock::time_point ended_at,
    int64_t media_duration_ms,
    int64_t size_bytes,
    const std::string &sha256,
    std::string *error) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!db_ || !exec_sql(db_, "BEGIN IMMEDIATE", error)) return false;
    bool committed = false;
    auto rollback = [&] {
        if (!committed) sqlite3_exec(db_, "ROLLBACK", nullptr, nullptr, nullptr);
    };
    const int64_t started_at_ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(started_at.time_since_epoch()).count();
    const int64_t ended_at_ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(ended_at.time_since_epoch()).count();

    sqlite3_stmt *segment = nullptr;
    const char *segment_sql = R"SQL(
INSERT INTO recording_segments(
    segment_id, recording_id, segment_index, clip_ref, started_at_ms, ended_at_ms,
    media_duration_ms, size_bytes, sha256)
VALUES(?, ?, ?, ?, ?, ?, ?, ?, ?)
)SQL";
    if (sqlite3_prepare_v2(db_, segment_sql, -1, &segment, nullptr) != SQLITE_OK) {
        set_error(error, db_, "prepare recording segment insert failed");
        rollback();
        return false;
    }
    sqlite3_bind_text(segment, 1, segment_id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(segment, 2, recording_id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(segment, 3, segment_index);
    sqlite3_bind_text(segment, 4, clip_ref.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(segment, 5, started_at_ms);
    sqlite3_bind_int64(segment, 6, ended_at_ms);
    sqlite3_bind_int64(segment, 7, media_duration_ms);
    sqlite3_bind_int64(segment, 8, size_bytes);
    sqlite3_bind_text(segment, 9, sha256.c_str(), -1, SQLITE_TRANSIENT);
    const bool segment_ok = sqlite3_step(segment) == SQLITE_DONE;
    sqlite3_finalize(segment);
    if (!segment_ok) {
        set_error(error, db_, "insert recording segment failed");
        rollback();
        return false;
    }

    sqlite3_stmt *recording = nullptr;
    const char *recording_sql = R"SQL(
UPDATE recordings
SET segment_count=segment_count+1,
    total_media_duration_ms=total_media_duration_ms+?
WHERE recording_id=? AND status='recording'
)SQL";
    if (sqlite3_prepare_v2(db_, recording_sql, -1, &recording, nullptr) != SQLITE_OK) {
        set_error(error, db_, "prepare recording segment accounting failed");
        rollback();
        return false;
    }
    sqlite3_bind_int64(recording, 1, media_duration_ms);
    sqlite3_bind_text(recording, 2, recording_id.c_str(), -1, SQLITE_TRANSIENT);
    const bool recording_ok = sqlite3_step(recording) == SQLITE_DONE && sqlite3_changes(db_) == 1;
    sqlite3_finalize(recording);
    if (!recording_ok || !exec_sql(db_, "COMMIT", error)) {
        if (recording_ok) set_error(error, db_, "commit recording segment failed");
        rollback();
        return false;
    }
    committed = true;
    return true;
}

bool EventStore::finalize_recording(
    const std::string &recording_id,
    std::chrono::system_clock::time_point ended_at,
    const std::string &status,
    std::string *error) {
    if (status != "finalized" && status != "interrupted" && status != "failed") {
        if (error) *error = "invalid terminal recording status";
        return false;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    if (!db_) {
        if (error) *error = "event database is not open";
        return false;
    }
    const int64_t ended_at_ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(ended_at.time_since_epoch()).count();
    sqlite3_stmt *recording = nullptr;
    const char *sql = R"SQL(
UPDATE recordings SET status=?, ended_at_ms=?
WHERE recording_id=? AND status='recording'
)SQL";
    if (sqlite3_prepare_v2(db_, sql, -1, &recording, nullptr) != SQLITE_OK) {
        set_error(error, db_, "prepare recording finalization failed");
        return false;
    }
    sqlite3_bind_text(recording, 1, status.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(recording, 2, ended_at_ms);
    sqlite3_bind_text(recording, 3, recording_id.c_str(), -1, SQLITE_TRANSIENT);
    const bool ok = sqlite3_step(recording) == SQLITE_DONE && sqlite3_changes(db_) == 1;
    if (!ok) set_error(error, db_, "finalize recording failed");
    sqlite3_finalize(recording);
    return ok;
}

std::optional<std::string> EventStore::allocate_clip_ref(
    std::chrono::system_clock::time_point at,
    const std::string &media_root,
    std::string *error) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!db_) {
        if (error) *error = "event database is not open";
        return std::nullopt;
    }
    if (!exec_sql(db_, "BEGIN IMMEDIATE", error)) return std::nullopt;
    bool committed = false;
    auto rollback = [&] {
        if (!committed) sqlite3_exec(db_, "ROLLBACK", nullptr, nullptr, nullptr);
    };
    const std::string date = clip_directory_relative_path(at).substr(6);
    sqlite3_stmt *insert = nullptr;
    if (sqlite3_prepare_v2(db_,
            "INSERT OR IGNORE INTO clip_daily_sequences(local_date,last_sequence) VALUES(?,0)",
            -1, &insert, nullptr) != SQLITE_OK) {
        set_error(error, db_, "prepare clip sequence insert failed");
        rollback();
        return std::nullopt;
    }
    sqlite3_bind_text(insert, 1, date.c_str(), -1, SQLITE_TRANSIENT);
    const bool inserted = sqlite3_step(insert) == SQLITE_DONE;
    sqlite3_finalize(insert);
    if (!inserted) {
        set_error(error, db_, "initialize clip sequence failed");
        rollback();
        return std::nullopt;
    }

    for (;;) {
        sqlite3_stmt *update = nullptr;
        if (sqlite3_prepare_v2(db_,
                "UPDATE clip_daily_sequences SET last_sequence=last_sequence+1 WHERE local_date=?",
                -1, &update, nullptr) != SQLITE_OK) {
            set_error(error, db_, "prepare clip sequence update failed");
            rollback();
            return std::nullopt;
        }
        sqlite3_bind_text(update, 1, date.c_str(), -1, SQLITE_TRANSIENT);
        const bool updated = sqlite3_step(update) == SQLITE_DONE;
        sqlite3_finalize(update);
        if (!updated) {
            set_error(error, db_, "increment clip sequence failed");
            rollback();
            return std::nullopt;
        }
        sqlite3_stmt *select = nullptr;
        if (sqlite3_prepare_v2(db_,
                "SELECT last_sequence FROM clip_daily_sequences WHERE local_date=?",
                -1, &select, nullptr) != SQLITE_OK) {
            set_error(error, db_, "prepare clip sequence read failed");
            rollback();
            return std::nullopt;
        }
        sqlite3_bind_text(select, 1, date.c_str(), -1, SQLITE_TRANSIENT);
        if (sqlite3_step(select) != SQLITE_ROW) {
            set_error(error, db_, "read clip sequence failed");
            sqlite3_finalize(select);
            rollback();
            return std::nullopt;
        }
        const uint64_t sequence = static_cast<uint64_t>(sqlite3_column_int64(select, 0));
        sqlite3_finalize(select);
        const std::string clip_ref = clip_relative_path(at, sequence);
        if (!std::filesystem::exists(std::filesystem::path(media_root) / clip_ref)) {
            if (!exec_sql(db_, "COMMIT", error)) {
                rollback();
                return std::nullopt;
            }
            committed = true;
            return clip_ref;
        }
    }
}

std::vector<EventRecord> EventStore::pending_events(std::string *error) {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<EventRecord> events;
    if (!db_) return events;
    sqlite3_stmt *statement = nullptr;
    const char *sql = R"SQL(
SELECT event_id, device_id, event_type, at_ms, occurred_timezone, occurred_utc_offset_minutes,
       occurred_local_date, snapshot_path, recording_id, extra_json, reported
FROM events WHERE reported=0 ORDER BY at_ms ASC
)SQL";
    if (sqlite3_prepare_v2(db_, sql, -1, &statement, nullptr) != SQLITE_OK) {
        set_error(error, db_, "prepare pending event query failed");
        return events;
    }
    while (sqlite3_step(statement) == SQLITE_ROW) {
        EventRecord event;
        event.event_id = column_text(statement, 0);
        event.device_id = column_text(statement, 1);
        event.type = column_text(statement, 2);
        event.at_ms = sqlite3_column_int64(statement, 3);
        event.occurred_timezone = column_text(statement, 4);
        event.occurred_utc_offset_minutes = sqlite3_column_int(statement, 5);
        event.occurred_local_date = column_text(statement, 6);
        event.snapshot_path = column_text(statement, 7);
        event.recording_id = column_text(statement, 8);
        event.extra_json = column_text(statement, 9);
        event.reported = sqlite3_column_int(statement, 10);
        events.push_back(std::move(event));
    }
    sqlite3_finalize(statement);
    return events;
}

std::optional<EventRecord> EventStore::find_event(const std::string &event_id, std::string *error) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!db_) return std::nullopt;
    sqlite3_stmt *statement = nullptr;
    const char *sql = R"SQL(
SELECT event_id, device_id, event_type, at_ms, occurred_timezone, occurred_utc_offset_minutes,
       occurred_local_date, snapshot_path, recording_id, extra_json, reported
FROM events WHERE event_id=?
)SQL";
    if (sqlite3_prepare_v2(db_, sql, -1, &statement, nullptr) != SQLITE_OK) {
        set_error(error, db_, "prepare event lookup failed");
        return std::nullopt;
    }
    sqlite3_bind_text(statement, 1, event_id.c_str(), -1, SQLITE_TRANSIENT);
    if (sqlite3_step(statement) != SQLITE_ROW) {
        sqlite3_finalize(statement);
        if (error) *error = "event not found";
        return std::nullopt;
    }
    EventRecord event;
    event.event_id = column_text(statement, 0);
    event.device_id = column_text(statement, 1);
    event.type = column_text(statement, 2);
    event.at_ms = sqlite3_column_int64(statement, 3);
    event.occurred_timezone = column_text(statement, 4);
    event.occurred_utc_offset_minutes = sqlite3_column_int(statement, 5);
    event.occurred_local_date = column_text(statement, 6);
    event.snapshot_path = column_text(statement, 7);
    event.recording_id = column_text(statement, 8);
    event.extra_json = column_text(statement, 9);
    event.reported = sqlite3_column_int(statement, 10);
    sqlite3_finalize(statement);
    return event;
}

std::optional<RecordingRecord> EventStore::find_recording(
    const std::string &recording_id,
    std::string *error) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!db_) return std::nullopt;
    sqlite3_stmt *statement = nullptr;
    const char *sql = R"SQL(
SELECT recording_id, status, started_at_ms, COALESCE(ended_at_ms,0),
       recorded_timezone, started_utc_offset_minutes, started_local_date,
       segment_count, total_media_duration_ms
FROM recordings WHERE recording_id=?
)SQL";
    if (sqlite3_prepare_v2(db_, sql, -1, &statement, nullptr) != SQLITE_OK) {
        set_error(error, db_, "prepare recording lookup failed");
        return std::nullopt;
    }
    sqlite3_bind_text(statement, 1, recording_id.c_str(), -1, SQLITE_TRANSIENT);
    if (sqlite3_step(statement) != SQLITE_ROW) {
        sqlite3_finalize(statement);
        if (error) *error = "recording not found";
        return std::nullopt;
    }
    RecordingRecord recording;
    recording.recording_id = column_text(statement, 0);
    recording.status = column_text(statement, 1);
    recording.started_at_ms = sqlite3_column_int64(statement, 2);
    recording.ended_at_ms = sqlite3_column_int64(statement, 3);
    recording.recorded_timezone = column_text(statement, 4);
    recording.started_utc_offset_minutes = sqlite3_column_int(statement, 5);
    recording.started_local_date = column_text(statement, 6);
    recording.segment_count = sqlite3_column_int(statement, 7);
    recording.total_media_duration_ms = sqlite3_column_int64(statement, 8);
    sqlite3_finalize(statement);
    return recording;
}

std::vector<RecordingSegmentRecord> EventStore::recording_segments(
    const std::string &recording_id,
    std::string *error) {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<RecordingSegmentRecord> segments;
    if (!db_) return segments;
    sqlite3_stmt *statement = nullptr;
    const char *sql = R"SQL(
SELECT segment_id, recording_id, segment_index, clip_ref, started_at_ms,
       ended_at_ms, media_duration_ms, size_bytes, sha256
FROM recording_segments WHERE recording_id=? ORDER BY segment_index
)SQL";
    if (sqlite3_prepare_v2(db_, sql, -1, &statement, nullptr) != SQLITE_OK) {
        set_error(error, db_, "prepare recording segments query failed");
        return segments;
    }
    sqlite3_bind_text(statement, 1, recording_id.c_str(), -1, SQLITE_TRANSIENT);
    while (sqlite3_step(statement) == SQLITE_ROW) {
        RecordingSegmentRecord segment;
        segment.segment_id = column_text(statement, 0);
        segment.recording_id = column_text(statement, 1);
        segment.segment_index = sqlite3_column_int(statement, 2);
        segment.clip_ref = column_text(statement, 3);
        segment.started_at_ms = sqlite3_column_int64(statement, 4);
        segment.ended_at_ms = sqlite3_column_int64(statement, 5);
        segment.media_duration_ms = sqlite3_column_int64(statement, 6);
        segment.size_bytes = sqlite3_column_int64(statement, 7);
        segment.sha256 = column_text(statement, 8);
        segments.push_back(std::move(segment));
    }
    sqlite3_finalize(statement);
    return segments;
}

bool EventStore::update_segment_sha256(
    const std::string &segment_id,
    const std::string &sha256,
    std::string *error) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!db_) {
        if (error) *error = "event database is not open";
        return false;
    }
    sqlite3_stmt *statement = nullptr;
    const char *sql =
        "UPDATE recording_segments SET sha256=? "
        "WHERE segment_id=? AND (sha256='' OR sha256=?)";
    if (sqlite3_prepare_v2(db_, sql, -1, &statement, nullptr) != SQLITE_OK) {
        set_error(error, db_, "prepare segment sha256 update failed");
        return false;
    }
    sqlite3_bind_text(statement, 1, sha256.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(statement, 2, segment_id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(statement, 3, sha256.c_str(), -1, SQLITE_TRANSIENT);
    const bool ok =
        sqlite3_step(statement) == SQLITE_DONE && sqlite3_changes(db_) == 1;
    if (!ok) set_error(error, db_, "segment sha256 conflict");
    sqlite3_finalize(statement);
    return ok;
}

std::optional<int> EventStore::report_state(const std::string &event_id, std::string *error) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!db_) return std::nullopt;
    sqlite3_stmt *statement = nullptr;
    if (sqlite3_prepare_v2(db_, "SELECT reported FROM events WHERE event_id=?", -1, &statement, nullptr) != SQLITE_OK) {
        set_error(error, db_, "prepare report state query failed");
        return std::nullopt;
    }
    sqlite3_bind_text(statement, 1, event_id.c_str(), -1, SQLITE_TRANSIENT);
    const int rc = sqlite3_step(statement);
    if (rc != SQLITE_ROW) {
        set_error(error, db_, "event report state not found");
        sqlite3_finalize(statement);
        return std::nullopt;
    }
    const int state = sqlite3_column_int(statement, 0);
    sqlite3_finalize(statement);
    return state;
}

bool EventStore::mark_reported(const std::string &event_id, std::string *error) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!db_) return false;
    sqlite3_stmt *statement = nullptr;
    if (sqlite3_prepare_v2(db_, "UPDATE events SET reported=1 WHERE event_id=?", -1, &statement, nullptr) != SQLITE_OK) {
        set_error(error, db_, "prepare reported update failed");
        return false;
    }
    sqlite3_bind_text(statement, 1, event_id.c_str(), -1, SQLITE_TRANSIENT);
    const bool ok = sqlite3_step(statement) == SQLITE_DONE;
    if (!ok) set_error(error, db_, "mark event reported failed");
    sqlite3_finalize(statement);
    return ok;
}

bool EventStore::mark_unreportable(const std::string &event_id, std::string *error) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!db_) return false;
    sqlite3_stmt *statement = nullptr;
    if (sqlite3_prepare_v2(db_, "UPDATE events SET reported=-1 WHERE event_id=?", -1, &statement, nullptr) != SQLITE_OK) {
        set_error(error, db_, "prepare unreportable update failed");
        return false;
    }
    sqlite3_bind_text(statement, 1, event_id.c_str(), -1, SQLITE_TRANSIENT);
    const bool ok = sqlite3_step(statement) == SQLITE_DONE;
    if (!ok) set_error(error, db_, "mark event unreportable failed");
    sqlite3_finalize(statement);
    return ok;
}
