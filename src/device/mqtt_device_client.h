#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_set>

struct mosquitto;
struct mosquitto_message;

class MqttDeviceClient {
public:
    struct Callbacks {
        std::function<void(const std::string &payload)> command_handler;
        std::function<void(const std::string &payload)> signal_handler;
        std::function<bool(std::chrono::milliseconds duration)> wait_for_stop;
        std::function<bool()> should_stop;
        std::function<void()> on_connecting;
        std::function<void()> on_online;
    };

    struct ConnectState;

    MqttDeviceClient(std::string device_id,
                     std::string device_secret,
                     std::string host,
                     int port,
                     std::string ca_dir,
                     bool tls,
                     bool tls_insecure,
                     Callbacks callbacks);
    ~MqttDeviceClient();

    MqttDeviceClient(const MqttDeviceClient &) = delete;
    MqttDeviceClient &operator=(const MqttDeviceClient &) = delete;

    void start();
    bool wait_until_online(std::chrono::milliseconds timeout);
    void stop(bool publish_offline);

    bool publish_signal(const std::string &payload);
    bool publish_media_state(const std::string &payload);
    bool publish_event(const std::string &payload);
    bool publish_time_sync(const std::string &payload);
    bool publish_capabilities(const std::string &payload);
    bool publish_command_ack(const std::string &trace_id,
                             const std::string &cmd_id,
                             bool ok,
                             const std::string &error_code = "",
                             const std::string &data_json = "");

private:
    static void mqtt_connect_callback(struct mosquitto *client, void *userdata, int rc);
    static void mqtt_subscribe_callback(struct mosquitto *client,
                                        void *userdata,
                                        int message_id,
                                        int granted_qos_count,
                                        const int *granted_qos);
    static void mqtt_message_callback(struct mosquitto *client, void *userdata, const struct mosquitto_message *msg);
    static void mqtt_publish_callback(struct mosquitto *client, void *userdata, int message_id);

    void mqtt_loop();
    void handle_message(const struct mosquitto_message *msg);
    bool publish_payload(const std::string &topic, const std::string &payload, int qos, bool retained);
    bool wait_for_stop_or(std::chrono::milliseconds duration) const;
    bool should_stop() const;
    void set_online(bool online);

    std::string status_topic() const;
    std::string command_topic() const;
    std::string command_ack_topic() const;
    std::string signal_topic() const;
    std::string media_state_topic() const;
    std::string event_topic() const;
    std::string time_sync_topic() const;
    std::string capabilities_topic() const;

    std::string device_id_;
    std::string device_secret_;
    std::string host_;
    int port_ = 8883;
    std::string ca_dir_;
    bool tls_ = true;
    bool tls_insecure_ = true;
    Callbacks callbacks_;

    std::atomic<bool> stop_{false};
    std::mutex online_mtx_;
    std::condition_variable online_cv_;
    bool online_ = false;
    std::mutex mqtt_mtx_;
    mosquitto *mqtt_client_ = nullptr;
    std::thread mqtt_thread_;
    std::mutex publish_mtx_;
    std::condition_variable publish_cv_;
    std::unordered_set<int> published_message_ids_;
    std::unordered_set<int> nonblocking_message_ids_;
};
