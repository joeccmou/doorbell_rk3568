#pragma once

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <string>
#include <thread>

#include <gpiod.h>

class DoorbellProvisioning {
public:
    DoorbellProvisioning();
    ~DoorbellProvisioning();

    DoorbellProvisioning(const DoorbellProvisioning &) = delete;
    DoorbellProvisioning &operator=(const DoorbellProvisioning &) = delete;

    bool start();
    void stop();

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
    bool run_mqtt_online_probe();

    void start_http_server();
    void stop_http_server();
    void http_server_loop();
    void handle_http_client(int client_fd);
    void begin_provisioning(const WifiCredentials &wifi);
    void provisioning_worker(WifiCredentials wifi);

    void start_mqtt_session();
    void stop_mqtt_session(bool publish_offline);
    void mqtt_loop();

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
    std::atomic<bool> mqtt_stop_{false};
    std::atomic<bool> mqtt_publish_offline_{false};

    std::mutex mqtt_mtx_;
    void *mqtt_client_ = nullptr;

    GpioLine button_gpio_;

    std::thread http_thread_;
    std::thread led_thread_;
    std::thread button_thread_;
    std::thread provision_thread_;
    std::thread mqtt_thread_;
};
