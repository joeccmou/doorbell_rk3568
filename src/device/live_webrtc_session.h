#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <gst/gst.h>
#include <nlohmann/json.hpp>

class LiveWebRtcSession {
public:
    struct VideoFrame {
        const uint8_t *data = nullptr;
        size_t size = 0;
        int dmabuf_fd = -1;
        uint32_t width = 0;
        uint32_t height = 0;
        uint32_t pixfmt = 0;
        uint32_t stride_y = 0;
        uint32_t stride_uv = 0;
        uint64_t seq = 0;
        uint64_t ts_ns = 0;
    };

    struct IceServer {
        std::vector<std::string> urls;
        std::string username;
        std::string credential;
    };

    struct StartRequest {
        std::string trace_id;
        std::string call_id;
        std::string device_id;
        std::string quality;
        std::vector<IceServer> ice_servers;
    };

    using SignalPublisher = std::function<void(const std::string &payload)>;
    using StatePublisher = std::function<void(const std::string &trace_id,
                                              const std::string &call_id,
                                              const std::string &media_state)>;
    using FrameProvider = std::function<bool(VideoFrame &frame)>;

    LiveWebRtcSession(SignalPublisher signal_publisher, StatePublisher state_publisher);
    ~LiveWebRtcSession();

    LiveWebRtcSession(const LiveWebRtcSession &) = delete;
    LiveWebRtcSession &operator=(const LiveWebRtcSession &) = delete;

    bool start(const StartRequest &request, std::string *error_message);
    void stop();
    void handle_signal(const std::string &payload);
    void set_frame_provider(FrameProvider provider);

    void on_negotiation_needed();
    void on_offer_created(GstPromise *promise);
    void on_ice_candidate(unsigned int mline_index, const char *candidate);
    void on_ice_connection_state_changed();
    void on_bus_message(GstMessage *message);
    GstPadProbeReturn on_rtp_probe(GstPadProbeInfo *info);

private:
    struct QualityProfile {
        int width;
        int height;
        int bitrate;
    };

    static QualityProfile profile_for_quality(const std::string &quality);
    static std::string stun_server_for(const std::vector<IceServer> &ice_servers);

    void publish_signal(const std::string &signal_type,
                        const std::string *sdp,
                        const nlohmann::json *candidate);
    void publish_state(const std::string &media_state);
    void request_offer(const char *reason);
    void frame_push_loop();
    void handle_answer(const nlohmann::json &signal);
    void handle_candidate(const nlohmann::json &signal);

    SignalPublisher signal_publisher_;
    StatePublisher state_publisher_;

    mutable std::mutex mtx_;
    StartRequest current_;
    GstElement *pipeline_ = nullptr;
    GstElement *webrtc_ = nullptr;
    GstElement *appsrc_ = nullptr;
    GstPad *webrtc_sink_pad_ = nullptr;
    GMainLoop *main_loop_ = nullptr;
    FrameProvider frame_provider_;
    guint bus_watch_id_ = 0;
    gulong rtp_probe_id_ = 0;
    guint64 rtp_buffer_count_ = 0;
    guint64 rtp_bytes_ = 0;
    uint32_t appsrc_width_ = 0;
    uint32_t appsrc_height_ = 0;
    uint32_t appsrc_pixfmt_ = 0;
    size_t appsrc_frame_size_ = 0;
    std::thread loop_thread_;
    std::thread frame_thread_;
    std::atomic<bool> frame_stop_{false};
    bool active_published_ = false;
    bool offer_requested_ = false;
};