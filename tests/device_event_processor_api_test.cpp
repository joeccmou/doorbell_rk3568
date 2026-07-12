#include "events/device_event_processor.h"

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <type_traits>
#include <vector>

int main() {
    using BeginSignature = std::optional<EventRecord> (DeviceEventProcessor::*)(
        double,
        std::chrono::system_clock::time_point,
        std::string *);
    using SubmitSignature = void (DeviceEventProcessor::*)(
        EventRecord,
        std::vector<uint8_t>,
        uint32_t,
        uint32_t);
    using AllocateSignature = std::optional<std::string> (DeviceEventProcessor::*)(
        std::chrono::system_clock::time_point,
        std::string *);
    using BeginWithClipSignature = std::optional<EventRecord> (DeviceEventProcessor::*)(
        double,
        std::chrono::system_clock::time_point,
        const std::string &,
        std::string *);

    static_assert(std::is_same_v<
                  decltype(&DeviceEventProcessor::begin_person_event),
                  BeginSignature>);
    static_assert(std::is_same_v<
                  decltype(&DeviceEventProcessor::submit_person_snapshot),
                  SubmitSignature>);
    static_assert(std::is_same_v<
                  decltype(&DeviceEventProcessor::allocate_recording_clip),
                  AllocateSignature>);
    static_assert(std::is_same_v<
                  decltype(&DeviceEventProcessor::begin_person_event_with_clip),
                  BeginWithClipSignature>);
    return 0;
}
