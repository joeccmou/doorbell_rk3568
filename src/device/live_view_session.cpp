#include "device/live_view_session.h"

#include <cstdio>
#include <ctime>
#include <utility>

namespace {

std::string iso_utc_now() {
    std::time_t now = std::time(nullptr);
    std::tm tm{};
#if defined(_WIN32)
    gmtime_s(&tm, &now);
#else
    gmtime_r(&now, &tm);
#endif
    char buf[32]{};
    std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tm);
    return std::string(buf);
}

std::pair<int, int> quality_dimensions(const std::string &quality) {
    if (quality == "360p") return {640, 360};
    if (quality == "1080p") return {1920, 1080};
    if (quality == "1440p") return {2560, 1440};
    return {1280, 720};
}

class LiveWebRtcBackend final : public LiveViewSession::RtcBackend {
public:
    LiveWebRtcBackend(LiveWebRtcSession::SignalPublisher signal_publisher,
                      LiveWebRtcSession::StatePublisher state_publisher)
        : session_(std::move(signal_publisher), std::move(state_publisher)) {}

    bool start(const LiveWebRtcSession::StartRequest &request, std::string *error_message) override {
        return session_.start(request, error_message);
    }

    void stop() override {
        session_.stop();
    }

    void handle_signal(const std::string &payload) override {
        session_.handle_signal(payload);
    }

    void set_frame_provider(LiveViewSession::FrameProvider provider) override {
        session_.set_frame_provider(std::move(provider));
    }

    void set_audio_manager(AudioCaptureManager *manager) override {
        session_.set_audio_manager(manager);
    }

    bool switch_quality(
        const std::string &trace_id,
        const std::string &quality,
        LiveWebRtcSession::QualitySwitchCallback callback,
        std::string *error_code) override {
        return session_.switch_quality(
            trace_id, quality, std::move(callback), error_code);
    }

private:
    LiveWebRtcSession session_;
};

} // namespace

LiveViewSession::LiveViewSession(std::string device_id, Publishers publishers)
    : LiveViewSession(std::move(device_id), std::move(publishers), nullptr) {}

LiveViewSession::LiveViewSession(std::string device_id, Publishers publishers, std::unique_ptr<RtcBackend> backend)
    : device_id_(std::move(device_id)),
      publishers_(std::move(publishers)),
      backend_(std::move(backend)) {
    if (!backend_) {
        backend_ = std::make_unique<LiveWebRtcBackend>(
            [this](const std::string &payload) {
                if (publishers_.signal_publisher) publishers_.signal_publisher(payload);
            },
            [this](const std::string &trace_id, const std::string &call_id, const std::string &media_state) {
                publish_media_state(trace_id, call_id, media_state);
            });
    }
}

LiveViewSession::~LiveViewSession() = default;

void LiveViewSession::set_frame_provider(FrameProvider provider) {
    if (backend_) {
        backend_->set_frame_provider(std::move(provider));
    }
}

void LiveViewSession::set_audio_manager(AudioCaptureManager *manager) {
    if (backend_) {
        backend_->set_audio_manager(manager);
    }
}

bool LiveViewSession::handle_command(const std::string &payload) {
    try {
        auto in = nlohmann::json::parse(payload);
        const std::string action = in.value("action", "");
        const std::string trace_id = in.value("trace_id", "");
        const std::string cmd_id = in.value("cmd_id", "");
        const std::string call_id = in.value("call_id", "");

        if (action == "start_live" || action == "answer_call") {
            current_mode_ = action == "answer_call" ? "call" : "live_view";
            {
                std::lock_guard<std::mutex> lock(session_mtx_);
                current_call_id_ = call_id;
            }
            publish_command_ack(trace_id, cmd_id, true);
            publish_media_state(trace_id, call_id, "connecting");

            std::string error_message;
            LiveWebRtcSession::StartRequest request;
            if (!parse_start_request(device_id_, in, &request, &error_message)) {
                publish_media_state(
                    trace_id,
                    call_id,
                    "idle",
                    "INVALID_ICE_SERVERS",
                    error_message);
                std::fprintf(
                    stderr,
                    "[mqtt] media start rejected call_id=%s error=%s\n",
                    call_id.c_str(),
                    error_message.c_str());
                return true;
            }
            if (!backend_ || !backend_->start(request, &error_message)) {
                publish_media_state(trace_id, call_id, "idle", "MEDIA_PIPELINE_FAILED", error_message);
                std::fprintf(stderr, "[mqtt] start_live failed call_id=%s error=%s\n", call_id.c_str(), error_message.c_str());
                return true;
            }
            std::fprintf(stdout,
                         "[mqtt] accepted media start action=%s call_id=%s quality=%s\n",
                         action.c_str(),
                         call_id.c_str(),
                         request.quality.c_str());
            return true;
        }

        if (action == "set_live_quality") {
            std::string current_call_id;
            {
                std::lock_guard<std::mutex> lock(session_mtx_);
                current_call_id = current_call_id_;
            }
            const std::string quality = in.value("quality", "");
            if (call_id.empty() || call_id != current_call_id) {
                publish_command_ack(
                    trace_id, cmd_id, false, "CALL_NOT_ACTIVE");
                return true;
            }
            std::string error_code;
            const bool accepted = backend_ && backend_->switch_quality(
                trace_id,
                quality,
                [this, trace_id, cmd_id, call_id](
                    bool ok,
                    const std::string &applied_quality,
                    const std::string &previous_quality,
                    const std::string &switch_error) {
                    const auto [width, height] =
                        quality_dimensions(applied_quality);
                    nlohmann::json data = {
                        {"call_id", call_id},
                        {"state", ok ? "applied" : "failed"},
                        {"quality", applied_quality},
                        {"previous_quality", previous_quality},
                        {"width", width},
                        {"height", height},
                    };
                    publish_command_ack(
                        trace_id,
                        cmd_id,
                        ok,
                        ok ? "" : switch_error,
                        data.dump());
                },
                &error_code);
            if (!accepted) {
                publish_command_ack(
                    trace_id,
                    cmd_id,
                    false,
                    error_code.empty()
                        ? "LIVE_QUALITY_SWITCH_FAILED"
                        : error_code);
            }
            return true;
        }

        if (action == "hangup") {
            publish_command_ack(trace_id, cmd_id, true);
            if (backend_) backend_->stop();
            {
                std::lock_guard<std::mutex> lock(session_mtx_);
                current_call_id_.clear();
            }
            publish_media_state(trace_id, call_id, "idle");
            std::fprintf(stdout, "[mqtt] accepted hangup call_id=%s\n", call_id.c_str());
            return true;
        }

        std::fprintf(stdout, "[mqtt] ignore unsupported command action=%s\n", action.c_str());
        return false;
    } catch (const std::exception &e) {
        std::fprintf(stderr, "[mqtt] invalid command payload error=%s\n", e.what());
        return false;
    }
}

void LiveViewSession::handle_signal(const std::string &payload) {
    if (backend_) {
        backend_->handle_signal(payload);
    }
}

bool LiveViewSession::publish_startup_media_state() {
    return publish_media_state("", "", "idle");
}

void LiveViewSession::stop() {
    if (backend_) {
        backend_->stop();
    }
    std::lock_guard<std::mutex> lock(session_mtx_);
    current_call_id_.clear();
}

void LiveViewSession::publish_command_ack(const std::string &trace_id,
                                          const std::string &cmd_id,
                                          bool ok,
                                          const std::string &error_code,
                                          const std::string &data_json) {
    if (!data_json.empty() && publishers_.command_ack_data_publisher) {
        publishers_.command_ack_data_publisher(
            trace_id, cmd_id, ok, error_code, data_json);
        return;
    }
    if (publishers_.command_ack_publisher) {
        publishers_.command_ack_publisher(trace_id, cmd_id, ok, error_code);
    }
}

bool LiveViewSession::publish_media_state(const std::string &trace_id,
                                          const std::string &call_id,
                                          const std::string &media_state,
                                          const std::string &error_code,
                                          const std::string &error_message) {
    if (!publishers_.media_state_publisher) return false;

    nlohmann::json media;
    media["trace_id"] = trace_id;
    media["device_id"] = device_id_;
    const bool has_error = !error_code.empty();
    media["media_state"] = media_state;
    media["occupant_user_id"] = nullptr;
    media["mode"] = (media_state == "idle" && !has_error)
        ? nlohmann::json(nullptr)
        : nlohmann::json(current_mode_);
    media["call_id"] = (media_state == "idle" && !has_error) ? nlohmann::json(nullptr) : nlohmann::json(call_id);
    media["error_code"] = has_error ? nlohmann::json(error_code) : nlohmann::json(nullptr);
    media["error_message"] = has_error ? nlohmann::json(error_message) : nlohmann::json(nullptr);
    media["updated_at"] = iso_utc_now();
    return publishers_.media_state_publisher(media.dump());
}

bool LiveViewSession::parse_start_request(
    const std::string &device_id,
    const nlohmann::json &command,
    LiveWebRtcSession::StartRequest *request,
    std::string *error_message) {
    if (!request) {
        if (error_message) *error_message = "ICE request output is null";
        return false;
    }
    request->trace_id = command.value("trace_id", "");
    request->call_id = command.value("call_id", "");
    request->device_id = device_id;
    request->quality = command.value("quality", "1080p");

    if (!command.contains("ice_servers") || !command["ice_servers"].is_array() ||
        command["ice_servers"].empty()) {
        if (error_message) *error_message = "ice_servers must be a non-empty array";
        return false;
    }

    bool has_supported_url = false;
    for (const auto &item : command["ice_servers"]) {
        if (!item.is_object() || !item.contains("urls") ||
            !item["urls"].is_array() || item["urls"].empty()) {
            if (error_message) *error_message = "ICE urls must be a non-empty string array";
            return false;
        }

        LiveWebRtcSession::IceServer server;
        for (const auto &url_value : item["urls"]) {
            if (!url_value.is_string()) {
                if (error_message) *error_message = "ICE urls must contain strings only";
                return false;
            }
            const std::string url = url_value.get<std::string>();
            const bool is_stun =
                url.rfind("stun:", 0) == 0 || url.rfind("stun://", 0) == 0;
            const bool is_turn =
                url.rfind("turn:", 0) == 0 || url.rfind("turn://", 0) == 0;
            if (url.empty() || (!is_stun && !is_turn)) {
                if (error_message) *error_message = "unsupported ICE URL scheme";
                return false;
            }
            server.urls.push_back(url);
            has_supported_url = true;
        }
        if (item.contains("username") && !item["username"].is_string()) {
            if (error_message) *error_message = "ICE username must be a string";
            return false;
        }
        if (item.contains("credential") && !item["credential"].is_string()) {
            if (error_message) *error_message = "ICE credential must be a string";
            return false;
        }
        server.username = item.value("username", "");
        server.credential = item.value("credential", "");

        for (const auto &url : server.urls) {
            const bool is_turn =
                url.rfind("turn:", 0) == 0 || url.rfind("turn://", 0) == 0;
            if (is_turn && (server.username.empty() || server.credential.empty())) {
                if (error_message) {
                    *error_message = "TURN URL requires username and credential";
                }
                return false;
            }
        }
        request->ice_servers.push_back(std::move(server));
    }

    if (!has_supported_url) {
        if (error_message) *error_message = "no supported ICE URL";
        return false;
    }
    return true;
}
