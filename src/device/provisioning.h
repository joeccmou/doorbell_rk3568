#pragma once

#include <atomic>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include <gpiod.h>

#include "device/live_webrtc_session.h"
#include "device/device_settings_store.h"
#include "utils/audio_capture_manager.h"

class LiveViewSession;
class MqttDeviceClient;
class SntpClient;
class TimeSyncService;
class TimezoneManager;
struct DeviceTimeSyncStatus;

class DoorbellProvisioning {
public:
    DoorbellProvisioning();
    ~DoorbellProvisioning();

    DoorbellProvisioning(const DoorbellProvisioning &) = delete;
    DoorbellProvisioning &operator=(const DoorbellProvisioning &) = delete;

    bool start();
    void stop();
    void set_live_frame_provider(LiveWebRtcSession::FrameProvider provider);
    void set_live_audio_manager(AudioCaptureManager *manager);
    const std::string &device_id() const { return identity_.device_id; }
    const std::string &device_secret() const { return identity_.device_secret; }
    bool publish_event(const std::string &payload);
    bool publish_ring_press(const std::string &payload);
    bool publish_command_ack(const std::string &trace_id,
                             const std::string &cmd_id,
                             bool ok,
                             const std::string &error_code,
                             const std::string &data_json);
    using EventReportAckHandler = std::function<void(const std::string &)>;
    void set_event_report_ack_handler(EventReportAckHandler handler);
    using EventMediaCommandHandler = std::function<bool(const std::string &)>;
    void set_event_media_command_handler(EventMediaCommandHandler handler);
    using RingButtonHandler = std::function<void()>;
    void set_ring_button_handler(RingButtonHandler handler);
    using PersonSettingsHandler = std::function<bool(bool, const std::string &)>;
    using ImageRotateHandler = std::function<bool(bool)>;
    void set_settings_apply_handlers(PersonSettingsHandler person_handler,
                                     ImageRotateHandler image_rotate_handler);
    DeviceSettingsValues current_settings() const;

private:
    enum class Stage {
        Idle,
        WaitingProvision,
        ConnectingWifi,
        ConnectingCloud,
        Online,
        WifiFailed,
        CloudFailed,
    };

    struct Identity {
        std::string device_id;
        std::string device_secret;
        std::string model;
        std::string firmware_version;
        std::string ap_ssid;
        std::string ap_pass;
    };

    struct WifiCredentials {
        std::string ssid;
        std::string password;
    };

    struct ProvisionStatus {
        bool wifi_ok = false;
        bool cloud_ok = false;
        std::string error_code;
    };

    struct GpioLine {
        gpiod_chip *chip = nullptr;
        gpiod_line *line = nullptr;

        void close();
    };

    bool load_identity();
    bool load_saved_wifi(WifiCredentials *out) const;
    bool save_wifi(const WifiCredentials &wifi) const;
    void clear_saved_wifi() const;

    bool start_access_point();
    void stop_access_point();
    bool connect_sta(const WifiCredentials &wifi);

    void start_http_server();
    void stop_http_server();
    void http_server_loop();
    void handle_http_client(int client_fd);
    void begin_provisioning(const WifiCredentials &wifi);
    void provisioning_worker(WifiCredentials wifi);

    void start_led();
    void stop_led();
    void led_loop();
    bool configure_led_class();
    void set_led(bool on);

    void start_button();
    void stop_button();
    void button_loop();
    void handle_long_press_reset();

    bool open_input_line(GpioLine *out, const std::string &chip_name, unsigned int offset);

    void set_stage(Stage stage, const char *error_code = nullptr);
    Stage stage() const;
    ProvisionStatus status_snapshot() const;
    std::string stage_name(Stage stage) const;

    bool wait_for_stop_or(std::chrono::milliseconds duration) const;
    void handle_mqtt_command(const std::string &payload);
    void publish_time_sync(const DeviceTimeSyncStatus &status);
    void publish_capabilities();
    bool apply_person_settings(bool enabled, const std::string &sensitivity);
    bool apply_image_rotate180(bool enabled);

    std::string identity_path_;
    std::string data_dir_;
    std::string wifi_path_;
    std::string ap_iface_;
    std::string sta_iface_;
    std::string mqtt_host_;
    std::string mqtt_ca_dir_;
    bool mqtt_tls_ = true;
    bool mqtt_tls_insecure_ = true;
    int mqtt_port_ = 8883;
    int http_port_ = 80;
    std::string led_name_;
    std::string led_path_;
    std::string led_trigger_path_;
    std::string led_brightness_path_;
    std::string button_chip_;
    unsigned int button_line_ = 9;
    bool button_active_low_ = true;

    Identity identity_;

    mutable std::mutex state_mtx_;
    Stage stage_ = Stage::Idle;
    ProvisionStatus status_;

    mutable std::mutex stop_mtx_;
    mutable std::condition_variable stop_cv_;
    std::atomic<bool> stop_{false};

    std::atomic<bool> http_stop_{false};
    std::atomic<int> http_fd_{-1};

    std::unique_ptr<MqttDeviceClient> mqtt_client_;
    std::unique_ptr<LiveViewSession> live_view_session_;
    std::atomic<bool> startup_media_state_published_{false};
    std::unique_ptr<SntpClient> sntp_client_;
    std::unique_ptr<TimeSyncService> time_sync_service_;
    std::unique_ptr<TimezoneManager> timezone_manager_;
    std::unique_ptr<DeviceSettingsStore> settings_store_;

    mutable std::mutex settings_handler_mtx_;
    PersonSettingsHandler person_settings_handler_;
    ImageRotateHandler image_rotate_handler_;
    std::atomic<bool> status_led_enabled_{true};
    mutable std::mutex event_handler_mtx_;
    EventReportAckHandler event_report_ack_handler_;
    EventMediaCommandHandler event_media_command_handler_;
    RingButtonHandler ring_button_handler_;

    GpioLine button_gpio_;

    std::thread http_thread_;
    std::thread led_thread_;
    std::thread button_thread_;
    std::thread provision_thread_;
};
