#include "events/event_store.h"

#include <sqlite3.h>

#include <cassert>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <string>

namespace {
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

    const auto root = std::filesystem::temp_directory_path() / "doorbell_event_store_v3_test";
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root / "media/clips/20260711");
    const auto database_path = root / "doorbell.db";
    const auto at =
        std::chrono::system_clock::time_point(std::chrono::milliseconds(1783731723456LL));

    EventStore store;
    std::string error;
    assert(store.open(database_path.string(), &error));
    const std::string recording_id = "RK3568-000001-1783731723456";
    assert(store.begin_recording(recording_id, at, &error));
    auto event = store.create_person_event(
        "RK3568-000001",
        recording_id,
        at,
        "snapshots/20260711/20260711-090203456.jpg",
        0.91,
        &error);
    assert(event && event->recording_id == recording_id);
    assert(store.report_state(event->event_id, &error) == 0);
    assert(store.mark_reported(event->event_id, &error));
    assert(store.report_state(event->event_id, &error) == 1);

    auto clip = store.allocate_clip_ref(at, (root / "media").string(), &error);
    assert(clip && *clip == "clips/20260711/20260711T090203456-0000001.mp4");
    assert(store.complete_recording(
        recording_id,
        recording_id + "-000001",
        1,
        *clip,
        at,
        at + std::chrono::seconds(30),
        30000,
        1024,
        "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef",
        "finalized",
        &error));
    store.close();

    sqlite3 *db = nullptr;
    assert(sqlite3_open(database_path.string().c_str(), &db) == SQLITE_OK);
    assert(scalar_int(db, "PRAGMA user_version") == 4);
    assert(scalar_text(db, "SELECT recording_id FROM events") == recording_id);
    assert(scalar_text(db, "SELECT status FROM recordings") == "finalized");
    assert(scalar_int(db, "SELECT COUNT(*) FROM recording_segments") == 1);
    sqlite3_close(db);

    assert(store.open(database_path.string(), &error));
    store.close();

    std::filesystem::remove_all(root);
    return 0;
}
