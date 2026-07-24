#pragma once

#include <functional>
#include <memory>
#include <string>

#include <nlohmann/json.hpp>

#include "device/live_webrtc_session.h"
#include "utils/audio_capture_manager.h"

class LiveViewSession {
public:
    using FrameProvider = LiveWebRtcSession::FrameProvider;

    class RtcBackend {
    public:
        virtual ~RtcBackend() = default;
        virtual bool start(const LiveWebRtcSession::StartRequest &request, std::string *error_message) = 0;
        virtual void stop() = 0;
        virtual void handle_signal(const std::string &payload) = 0;
        virtual void set_frame_provider(FrameProvider provider) = 0;
        virtual void set_audio_manager(AudioCaptureManager *manager) = 0;
    };

    struct Publishers {
        std::function<void(const std::string &trace_id,
                           const std::string &cmd_id,
                           bool ok,
                           const std::string &error_code)> command_ack_publisher;
        std::function<void(const std::string &payload)> signal_publisher;
        std::function<void(const std::string &payload)> media_state_publisher;
    };

    LiveViewSession(std::string device_id, Publishers publishers);
    LiveViewSession(std::string device_id, Publishers publishers, std::unique_ptr<RtcBackend> backend);
    ~LiveViewSession();

    LiveViewSession(const LiveViewSession &) = delete;
    LiveViewSession &operator=(const LiveViewSession &) = delete;

    void set_frame_provider(FrameProvider provider);
    void set_audio_manager(AudioCaptureManager *manager);
    bool handle_command(const std::string &payload);
    void handle_signal(const std::string &payload);
    void stop();

private:
    void publish_command_ack(const std::string &trace_id,
                             const std::string &cmd_id,
                             bool ok,
                             const std::string &error_code = "");
    void publish_media_state(const std::string &trace_id,
                             const std::string &call_id,
                             const std::string &media_state,
                             const std::string &error_code = "",
                             const std::string &error_message = "");
    static LiveWebRtcSession::StartRequest parse_start_request(const std::string &device_id,
                                                               const nlohmann::json &command);

    std::string device_id_;
    std::string current_mode_ = "live_view";
    Publishers publishers_;
    std::unique_ptr<RtcBackend> backend_;
};
