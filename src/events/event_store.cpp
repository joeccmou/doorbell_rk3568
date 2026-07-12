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
    static const char *sql = R"SQL(
PRAGMA journal_mode=WAL;
PRAGMA synchronous=NORMAL;
CREATE TABLE IF NOT EXISTS event_meta (
    key TEXT PRIMARY KEY,
    value INTEGER NOT NULL
);
INSERT OR IGNORE INTO event_meta(key, value) VALUES('event_sequence', 0);
CREATE TABLE IF NOT EXISTS events (
    event_id TEXT PRIMARY KEY,
    device_id TEXT NOT NULL,
    event_type TEXT NOT NULL,
    at_ms INTEGER NOT NULL,
    snapshot_path TEXT NOT NULL,
    snapshot_url TEXT,
    clip_ref TEXT,
    extra_json TEXT NOT NULL DEFAULT '{}',
    reported INTEGER NOT NULL DEFAULT 0,
    created_at_ms INTEGER NOT NULL
);
CREATE INDEX IF NOT EXISTS idx_events_pending ON events(reported, at_ms);
)SQL";
    return exec_sql(db_, sql, error) && migrate_schema(error);
}

bool EventStore::migrate_schema(std::string *error) {
    int version = 0;
    if (!scalar_int(db_, "PRAGMA user_version", &version, error)) return false;
    if (version == 2) return true;
    if (version != 0) {
        if (error) *error = "unsupported event database schema version: " + std::to_string(version);
        return false;
    }
    static const char *migration = R"SQL(
BEGIN IMMEDIATE;
ALTER TABLE events ADD COLUMN occurred_timezone TEXT;
ALTER TABLE events ADD COLUMN occurred_utc_offset_minutes INTEGER;
ALTER TABLE events ADD COLUMN occurred_local_date TEXT;
UPDATE events
SET occurred_timezone='Asia/Shanghai',
    occurred_utc_offset_minutes=480,
    occurred_local_date=strftime('%Y-%m-%d', at_ms / 1000.0, 'unixepoch', '+480 minutes')
WHERE occurred_timezone IS NULL
   OR occurred_utc_offset_minutes IS NULL
   OR occurred_local_date IS NULL;
CREATE TABLE clip_daily_sequences (
    local_date TEXT PRIMARY KEY,
    last_sequence INTEGER NOT NULL
);
CREATE INDEX IF NOT EXISTS idx_events_local_date
    ON events(occurred_local_date, at_ms DESC, event_id DESC);
PRAGMA user_version=2;
COMMIT;
)SQL";
    if (exec_sql(db_, migration, error)) return true;
    sqlite3_exec(db_, "ROLLBACK", nullptr, nullptr, nullptr);
    return false;
}

std::optional<EventRecord> EventStore::create_person_event(
    const std::string &device_id,
    std::chrono::system_clock::time_point at,
    const std::string &snapshot_path,
    const std::string &clip_ref,
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
    event.type = "person";
    event.at_ms = at_ms;
    event.occurred_timezone = system_timezone_name();
    event.occurred_utc_offset_minutes = utc_offset_minutes(at);
    event.occurred_local_date = local_date_iso(at);
    event.snapshot_path = snapshot_path;
    event.clip_ref = clip_ref;
    std::ostringstream extra;
    extra << "{\"confidence\":" << std::setprecision(15) << confidence << '}';
    event.extra_json = extra.str();

    sqlite3_stmt *insert = nullptr;
    const char *sql = R"SQL(
INSERT INTO events(event_id, device_id, event_type, at_ms, occurred_timezone, occurred_utc_offset_minutes,
                   occurred_local_date, snapshot_path, snapshot_url, clip_ref, extra_json, reported, created_at_ms)
VALUES(?, ?, ?, ?, ?, ?, ?, ?, NULL, NULLIF(?, ''), ?, 0, ?)
)SQL";
    if (sqlite3_prepare_v2(db_, sql, -1, &insert, nullptr) != SQLITE_OK) {
        set_error(error, db_, "prepare event insert failed");
        rollback();
        return std::nullopt;
    }
    sqlite3_bind_text(insert, 1, event.event_id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(insert, 2, event.device_id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(insert, 3, event.type.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(insert, 4, at_ms);
    sqlite3_bind_text(insert, 5, event.occurred_timezone.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(insert, 6, event.occurred_utc_offset_minutes);
    sqlite3_bind_text(insert, 7, event.occurred_local_date.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(insert, 8, event.snapshot_path.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(insert, 9, event.clip_ref.c_str(), -1, SQLITE_TRANSIENT);
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
       occurred_local_date, snapshot_path, snapshot_url, clip_ref, extra_json, reported
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
        event.snapshot_url = column_text(statement, 8);
        event.clip_ref = column_text(statement, 9);
        event.extra_json = column_text(statement, 10);
        event.reported = sqlite3_column_int(statement, 11) != 0;
        events.push_back(std::move(event));
    }
    sqlite3_finalize(statement);
    return events;
}

bool EventStore::set_snapshot_url(const std::string &event_id, const std::string &snapshot_url, std::string *error) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!db_) return false;
    sqlite3_stmt *statement = nullptr;
    if (sqlite3_prepare_v2(db_, "UPDATE events SET snapshot_url=? WHERE event_id=?", -1, &statement, nullptr) != SQLITE_OK) {
        set_error(error, db_, "prepare snapshot update failed");
        return false;
    }
    sqlite3_bind_text(statement, 1, snapshot_url.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(statement, 2, event_id.c_str(), -1, SQLITE_TRANSIENT);
    const bool ok = sqlite3_step(statement) == SQLITE_DONE;
    if (!ok) set_error(error, db_, "update snapshot url failed");
    sqlite3_finalize(statement);
    return ok;
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
