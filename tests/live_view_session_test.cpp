#include "device/live_view_session.h"

#include <cassert>
#include <memory>
#include <string>
#include <utility>
#include <vector>

class FakeRtcBackend final : public LiveViewSession::RtcBackend {
public:
    bool start(const LiveWebRtcSession::StartRequest &request, std::string *error_message) override {
        start_called = true;
        last_request = request;
        if (fail_start) {
            if (error_message) *error_message = fail_message;
            return false;
        }
        return true;
    }

    void stop() override {
        stop_called = true;
    }

    void handle_signal(const std::string &payload) override {
        signals.push_back(payload);
    }

    void set_frame_provider(LiveViewSession::FrameProvider provider) override {
        frame_provider_set = static_cast<bool>(provider);
    }

    void set_audio_manager(AudioCaptureManager *manager) override {
        audio_manager = manager;
    }

    bool fail_start = false;
    std::string fail_message = "pipeline failed";
    bool start_called = false;
    bool stop_called = false;
    bool frame_provider_set = false;
    AudioCaptureManager *audio_manager = nullptr;
    LiveWebRtcSession::StartRequest last_request;
    std::vector<std::string> signals;
};

struct PublishCapture {
    std::vector<nlohmann::json> acks;
    std::vector<nlohmann::json> media_states;
    std::vector<std::string> signals;
};

LiveViewSession make_session(PublishCapture *capture, std::unique_ptr<FakeRtcBackend> backend, FakeRtcBackend **backend_ptr) {
    *backend_ptr = backend.get();
    LiveViewSession::Publishers publishers;
    publishers.command_ack_publisher = [capture](const std::string &trace_id,
                                                 const std::string &cmd_id,
                                                 bool ok,
                                                 const std::string &error_code) {
        nlohmann::json ack;
        ack["trace_id"] = trace_id;
        ack["cmd_id"] = cmd_id;
        ack["ok"] = ok;
        ack["error_code"] = error_code.empty() ? nlohmann::json(nullptr) : nlohmann::json(error_code);
        capture->acks.push_back(std::move(ack));
    };
    publishers.signal_publisher = [capture](const std::string &payload) {
        capture->signals.push_back(payload);
    };
    publishers.media_state_publisher = [capture](const std::string &payload) {
        capture->media_states.push_back(nlohmann::json::parse(payload));
        return true;
    };
    return LiveViewSession("device-1", std::move(publishers), std::move(backend));
}

void test_start_live_routes_request() {
    PublishCapture capture;
    FakeRtcBackend *backend = nullptr;
    auto session = make_session(&capture, std::make_unique<FakeRtcBackend>(), &backend);

    const bool handled = session.handle_command(R"({
        "action":"start_live",
        "trace_id":"trace-1",
        "cmd_id":"cmd-1",
        "call_id":"call-1",
        "quality":"1080p",
        "ice_servers":[{"urls":["stun:one","turn:two"],"username":"user","credential":"pass"}]
    })");

    assert(handled);
    assert(backend != nullptr);
    assert(backend->start_called);
    assert(backend->last_request.device_id == "device-1");
    assert(backend->last_request.call_id == "call-1");
    assert(backend->last_request.quality == "1080p");
    assert(backend->last_request.ice_servers.size() == 1);
    assert(backend->last_request.ice_servers[0].urls.size() == 2);
    assert(capture.acks.size() == 1);
    assert(capture.acks[0]["cmd_id"] == "cmd-1");
    assert(capture.acks[0]["ok"] == true);
    assert(capture.media_states.size() == 1);
    assert(capture.media_states[0]["media_state"] == "connecting");
    assert(capture.media_states[0]["call_id"] == "call-1");
    assert(capture.media_states[0]["mode"] == "live_view");
}

void test_start_live_failure_reports_idle_error() {
    PublishCapture capture;
    auto fake = std::make_unique<FakeRtcBackend>();
    fake->fail_start = true;
    fake->fail_message = "encoder unavailable";
    FakeRtcBackend *backend = nullptr;
    auto session = make_session(&capture, std::move(fake), &backend);

    const bool handled = session.handle_command(R"({
        "action":"start_live",
        "trace_id":"trace-2",
        "cmd_id":"cmd-2",
        "call_id":"call-2"
    })");

    assert(handled);
    assert(backend != nullptr);
    assert(backend->start_called);
    assert(capture.acks.size() == 1);
    assert(capture.media_states.size() == 2);
    assert(capture.media_states[1]["media_state"] == "idle");
    assert(capture.media_states[1]["error_code"] == "MEDIA_PIPELINE_FAILED");
    assert(capture.media_states[1]["error_message"] == "encoder unavailable");
}

void test_hangup_stops_backend() {
    PublishCapture capture;
    FakeRtcBackend *backend = nullptr;
    auto session = make_session(&capture, std::make_unique<FakeRtcBackend>(), &backend);

    const bool handled = session.handle_command(R"({
        "action":"hangup",
        "trace_id":"trace-3",
        "cmd_id":"cmd-3",
        "call_id":"call-3"
    })");

    assert(handled);
    assert(backend != nullptr);
    assert(backend->stop_called);
    assert(capture.acks.size() == 1);
    assert(capture.media_states.size() == 1);
    assert(capture.media_states[0]["media_state"] == "idle");
    assert(capture.media_states[0]["call_id"].is_null());
}

void test_audio_manager_delegates_to_backend() {
    PublishCapture capture;
    FakeRtcBackend *backend = nullptr;
    auto session = make_session(&capture, std::make_unique<FakeRtcBackend>(), &backend);
    AudioCaptureManager manager;

    session.set_audio_manager(&manager);

    assert(backend != nullptr);
    assert(backend->audio_manager == &manager);
}
void test_signal_delegates_to_backend() {
    PublishCapture capture;
    FakeRtcBackend *backend = nullptr;
    auto session = make_session(&capture, std::make_unique<FakeRtcBackend>(), &backend);

    session.handle_signal("candidate-payload");

    assert(backend != nullptr);
    assert(backend->signals.size() == 1);
    assert(backend->signals[0] == "candidate-payload");
}

int main() {
    test_start_live_routes_request();
    test_start_live_failure_reports_idle_error();
    test_hangup_stops_backend();
    test_signal_delegates_to_backend();
    test_audio_manager_delegates_to_backend();
    return 0;
}
