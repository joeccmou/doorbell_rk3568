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

bool LiveViewSession::handle_command(const std::string &payload) {
    try {
        auto in = nlohmann::json::parse(payload);
        const std::string action = in.value("action", "");
        const std::string trace_id = in.value("trace_id", "");
        const std::string cmd_id = in.value("cmd_id", "");
        const std::string call_id = in.value("call_id", "");

        if (action == "start_live") {
            publish_command_ack(trace_id, cmd_id, true);
            publish_media_state(trace_id, call_id, "connecting");

            LiveWebRtcSession::StartRequest request = parse_start_request(device_id_, in);
            std::string error_message;
            if (!backend_ || !backend_->start(request, &error_message)) {
                publish_media_state(trace_id, call_id, "idle", "MEDIA_PIPELINE_FAILED", error_message);
                std::fprintf(stderr, "[mqtt] start_live failed call_id=%s error=%s\n", call_id.c_str(), error_message.c_str());
                return true;
            }
            std::fprintf(stdout, "[mqtt] accepted start_live call_id=%s quality=%s\n", call_id.c_str(), request.quality.c_str());
            return true;
        }

        if (action == "hangup") {
            publish_command_ack(trace_id, cmd_id, true);
            if (backend_) backend_->stop();
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

void LiveViewSession::stop() {
    if (backend_) {
        backend_->stop();
    }
}

void LiveViewSession::publish_command_ack(const std::string &trace_id,
                                          const std::string &cmd_id,
                                          bool ok,
                                          const std::string &error_code) {
    if (publishers_.command_ack_publisher) {
        publishers_.command_ack_publisher(trace_id, cmd_id, ok, error_code);
    }
}

void LiveViewSession::publish_media_state(const std::string &trace_id,
                                          const std::string &call_id,
                                          const std::string &media_state,
                                          const std::string &error_code,
                                          const std::string &error_message) {
    if (!publishers_.media_state_publisher) return;

    nlohmann::json media;
    media["trace_id"] = trace_id;
    media["device_id"] = device_id_;
    const bool has_error = !error_code.empty();
    media["media_state"] = media_state;
    media["occupant_user_id"] = nullptr;
    media["mode"] = (media_state == "idle" && !has_error) ? nlohmann::json(nullptr) : nlohmann::json("live_view");
    media["call_id"] = (media_state == "idle" && !has_error) ? nlohmann::json(nullptr) : nlohmann::json(call_id);
    media["error_code"] = has_error ? nlohmann::json(error_code) : nlohmann::json(nullptr);
    media["error_message"] = has_error ? nlohmann::json(error_message) : nlohmann::json(nullptr);
    media["updated_at"] = iso_utc_now();
    publishers_.media_state_publisher(media.dump());
}

LiveWebRtcSession::StartRequest LiveViewSession::parse_start_request(const std::string &device_id,
                                                                     const nlohmann::json &command) {
    LiveWebRtcSession::StartRequest request;
    request.trace_id = command.value("trace_id", "");
    request.call_id = command.value("call_id", "");
    request.device_id = device_id;
    request.quality = command.value("quality", "720p");
    if (command.contains("ice_servers") && command["ice_servers"].is_array()) {
        for (const auto &item : command["ice_servers"]) {
            LiveWebRtcSession::IceServer server;
            if (item.contains("urls")) {
                if (item["urls"].is_array()) {
                    for (const auto &url : item["urls"]) {
                        if (url.is_string()) server.urls.push_back(url.get<std::string>());
                    }
                } else if (item["urls"].is_string()) {
                    server.urls.push_back(item["urls"].get<std::string>());
                }
            }
            server.username = item.value("username", "");
            server.credential = item.value("credential", "");
            request.ice_servers.push_back(std::move(server));
        }
    }
    return request;
}
