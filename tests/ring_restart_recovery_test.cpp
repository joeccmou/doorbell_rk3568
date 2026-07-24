#include "events/event_store.h"

#include <cassert>
#include <chrono>
#include <filesystem>
#include <string>

int main() {
    const auto root =
        std::filesystem::temp_directory_path() / "doorbell_ring_restart_recovery_test";
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root);
    const auto database_path = root / "doorbell.db";
    const auto first_at =
        std::chrono::system_clock::time_point(std::chrono::milliseconds(1784807678371LL));

    EventStore store;
    std::string error;
    assert(store.open(database_path.string(), &error));
    const std::string first_recording_id = "RK3568-000001-1784807678371";
    assert(store.begin_recording(first_recording_id, first_at, &error));
    const auto first_press = store.record_ring_press(
        "RK3568-000001",
        first_recording_id,
        first_at,
        "snapshots/20260723/first.jpg",
        &error);
    assert(first_press && first_press->initial);
    assert(first_press->ring_state == "ringing");
    assert(store.update_ring_state(first_press->event.event_id, "accepted", &error));
    const auto accepted_press = store.record_ring_press(
        "RK3568-000001",
        first_recording_id,
        first_at + std::chrono::seconds(1),
        "snapshots/20260723/repeated.jpg",
        &error);
    assert(accepted_press && !accepted_press->initial);
    assert(accepted_press->ring_state == "accepted");
    store.close();

    assert(store.open(database_path.string(), &error));
    std::string recovered_event_id;
    assert(store.recover_stale_accepted_ring(&recovered_event_id, &error));
    assert(recovered_event_id == first_press->event.event_id);
    const auto second_at = first_at + std::chrono::hours(4);
    const std::string second_recording_id = "RK3568-000001-1784822078371";
    assert(store.begin_recording(second_recording_id, second_at, &error));
    const auto second_press = store.record_ring_press(
        "RK3568-000001",
        second_recording_id,
        second_at,
        "snapshots/20260723/second.jpg",
        &error);
    assert(second_press);
    assert(second_press->initial);
    assert(second_press->event.event_id != first_press->event.event_id);
    assert(second_press->event.recording_id == second_recording_id);
    store.close();

    std::filesystem::remove_all(root);
    return 0;
}
