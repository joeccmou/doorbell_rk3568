#include "events/event_store.h"

#include <sqlite3.h>

#include <cassert>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <string>

namespace {
void exec(sqlite3 *db, const char *sql) {
    char *error = nullptr;
    const int rc = sqlite3_exec(db, sql, nullptr, nullptr, &error);
    if (rc != SQLITE_OK) {
        sqlite3_free(error);
        assert(false);
    }
}

int scalar_int(sqlite3 *db, const char *sql) {
    sqlite3_stmt *statement = nullptr;
    assert(sqlite3_prepare_v2(db, sql, -1, &statement, nullptr) == SQLITE_OK);
    assert(sqlite3_step(statement) == SQLITE_ROW);
    const int value = sqlite3_column_int(statement, 0);
    sqlite3_finalize(statement);
    return value;
}

std::string scalar_text(sqlite3 *db, const char *sql) {
    sqlite3_stmt *statement = nullptr;
    assert(sqlite3_prepare_v2(db, sql, -1, &statement, nullptr) == SQLITE_OK);
    assert(sqlite3_step(statement) == SQLITE_ROW);
    const auto *value = sqlite3_column_text(statement, 0);
    const std::string result = value ? reinterpret_cast<const char *>(value) : "";
    sqlite3_finalize(statement);
    return result;
}
}

int main() {
#ifdef _WIN32
    _putenv_s("TZ", "Asia/Shanghai");
    _tzset();
#else
    setenv("TZ", "Asia/Shanghai", 1);
    tzset();
#endif

    const auto root = std::filesystem::temp_directory_path() / "doorbell_event_store_migration_test";
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root / "media/clips/20260711");
    const auto database_path = root / "doorbell.db";

    sqlite3 *legacy = nullptr;
    assert(sqlite3_open(database_path.string().c_str(), &legacy) == SQLITE_OK);
    exec(legacy, R"SQL(
CREATE TABLE event_meta(key TEXT PRIMARY KEY, value INTEGER NOT NULL);
INSERT INTO event_meta(key,value) VALUES('event_sequence', 8);
CREATE TABLE events(
 event_id TEXT PRIMARY KEY, device_id TEXT NOT NULL, event_type TEXT NOT NULL,
 at_ms INTEGER NOT NULL, snapshot_path TEXT NOT NULL, snapshot_url TEXT,
 clip_ref TEXT, extra_json TEXT NOT NULL DEFAULT '{}', reported INTEGER NOT NULL DEFAULT 0,
 created_at_ms INTEGER NOT NULL
);
INSERT INTO events VALUES(
 '20260711T010203456Z-0000008','RK3568-000001','person',1783731723456,
 'snapshots/20260711/20260711-090203456.jpg',NULL,
 'clips/20260711/20260711T090203456-0000008.mp4','{}',1,1783731723456
);
)SQL");
    sqlite3_close(legacy);

    EventStore store;
    std::string error;
    assert(store.open(database_path.string(), &error));
    store.close();

    sqlite3 *migrated = nullptr;
    assert(sqlite3_open(database_path.string().c_str(), &migrated) == SQLITE_OK);
    assert(scalar_int(migrated, "PRAGMA user_version") == 2);
    assert(scalar_text(migrated, "SELECT occurred_timezone FROM events") == "Asia/Shanghai");
    assert(scalar_int(migrated, "SELECT occurred_utc_offset_minutes FROM events") == 480);
    assert(scalar_text(migrated, "SELECT occurred_local_date FROM events") == "2026-07-11");
    assert(scalar_text(migrated, "SELECT event_id FROM events") == "20260711T010203456Z-0000008");
    assert(scalar_int(migrated, "SELECT reported FROM events") == 1);
    sqlite3_close(migrated);

    assert(store.open(database_path.string(), &error));
    const auto at = std::chrono::system_clock::time_point(std::chrono::milliseconds(1783731723456LL));
    auto first = store.allocate_clip_ref(at, (root / "media").string(), &error);
    assert(first && *first == "clips/20260711/20260711T090203456-0000001.mp4");
    std::filesystem::create_directories((root / "media" / *first).parent_path());
    std::FILE *file = std::fopen((root / "media" / *first).string().c_str(), "wb");
    assert(file != nullptr);
    std::fclose(file);
    auto second = store.allocate_clip_ref(at, (root / "media").string(), &error);
    assert(second && *second == "clips/20260711/20260711T090203456-0000002.mp4");
    const auto next_day = at + std::chrono::hours(24);
    auto next_day_first = store.allocate_clip_ref(next_day, (root / "media").string(), &error);
    assert(next_day_first && *next_day_first == "clips/20260712/20260712T090203456-0000001.mp4");
    store.close();

    assert(store.open(database_path.string(), &error));
    auto third = store.allocate_clip_ref(at, (root / "media").string(), &error);
    assert(third && *third == "clips/20260711/20260711T090203456-0000003.mp4");
    store.close();

    std::filesystem::remove_all(root);
    return 0;
}
