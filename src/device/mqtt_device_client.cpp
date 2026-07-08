#include "device/mqtt_device_client.h"

#include <cstdio>
#include <ctime>
#include <utility>

#include <mosquitto.h>
#include <nlohmann/json.hpp>

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

bool flush_mqtt_loop(mosquitto *client, const char *phase, int duration_ms) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(duration_ms);
    while (std::chrono::steady_clock::now() < deadline) {
        int rc = mosquitto_loop(client, 200, 1);
        if (rc != MOSQ_ERR_SUCCESS) {
            std::fprintf(stderr,
                         "[mqtt] loop flush failed phase=%s rc=%d error=%s\n",
                         phase,
                         rc,
                         mosquitto_strerror(rc));
            return false;
        }
    }
    return true;
}

bool configure_mqtt_client(mosquitto *client,
                           const std::string &device_id,
                           const std::string &device_secret,
                           const std::string &topic,
                           const std::string &offline_payload,
                           bool tls,
                           bool tls_insecure,
                           const std::string &ca_dir,
                           const char *phase) {
    int rc = mosquitto_username_pw_set(client, device_id.c_str(), device_secret.c_str());
    if (rc != MOSQ_ERR_SUCCESS) {
        std::fprintf(stderr,
                     "[mqtt] username setup failed phase=%s rc=%d error=%s\n",
                     phase,
                     rc,
                     mosquitto_strerror(rc));
        return false;
    }

    rc = mosquitto_will_set(client,
                            topic.c_str(),
                            static_cast<int>(offline_payload.size()),
                            offline_payload.data(),
                            1,
                            true);
    if (rc != MOSQ_ERR_SUCCESS) {
        std::fprintf(stderr,
                     "[mqtt] will setup failed phase=%s rc=%d error=%s\n",
                     phase,
                     rc,
                     mosquitto_strerror(rc));
        return false;
    }

    if (tls) {
        rc = mosquitto_tls_set(client, nullptr, ca_dir.c_str(), nullptr, nullptr, nullptr);
        if (rc != MOSQ_ERR_SUCCESS) {
            std::fprintf(stderr,
                         "[mqtt] tls setup failed phase=%s ca_dir=%s rc=%d error=%s\n",
                         phase,
                         ca_dir.c_str(),
                         rc,
                         mosquitto_strerror(rc));
            return false;
        }
        mosquitto_tls_insecure_set(client, tls_insecure);
    }
    return true;
}

} // namespace

struct MqttDeviceClient::ConnectState {
    bool completed = false;
    int rc = MOSQ_ERR_SUCCESS;
    const char *label = "";
    MqttDeviceClient *owner = nullptr;
};

namespace {

bool wait_for_mqtt_connack(mosquitto *client, MqttDeviceClient::ConnectState *state, int timeout_ms) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    while (std::chrono::steady_clock::now() < deadline) {
        if (state && state->completed) {
            return state->rc == MOSQ_ERR_SUCCESS;
        }

        int rc = mosquitto_loop(client, 200, 1);
        if (rc != MOSQ_ERR_SUCCESS) {
            std::fprintf(stderr,
                         "[mqtt] loop while waiting connack failed phase=%s rc=%d error=%s\n",
                         state && state->label ? state->label : "unknown",
                         rc,
                         mosquitto_strerror(rc));
            return false;
        }
    }

    std::fprintf(stderr,
                 "[mqtt] connack timeout phase=%s timeout_ms=%d\n",
                 state && state->label ? state->label : "unknown",
                 timeout_ms);
    return false;
}

} // namespace

MqttDeviceClient::MqttDeviceClient(std::string device_id,
                                   std::string device_secret,
                                   std::string host,
                                   int port,
                                   std::string ca_dir,
                                   bool tls,
                                   bool tls_insecure,
                                   Callbacks callbacks)
    : device_id_(std::move(device_id)),
      device_secret_(std::move(device_secret)),
      host_(std::move(host)),
      port_(port),
      ca_dir_(std::move(ca_dir)),
      tls_(tls),
      tls_insecure_(tls_insecure),
      callbacks_(std::move(callbacks)) {}

MqttDeviceClient::~MqttDeviceClient() {
    stop(false);
}

bool MqttDeviceClient::run_online_probe() {
    mosquitto_lib_init();
    ConnectState connect_state;
    connect_state.label = "probe";
    mosquitto *client = mosquitto_new((device_id_ + "-probe").c_str(), true, &connect_state);
    if (!client) {
        std::fprintf(stderr, "[mqtt] create probe client failed\n");
        mosquitto_lib_cleanup();
        return false;
    }

    const std::string online = R"({"status":"online"})";
    const std::string offline = R"({"status":"offline"})";
    mosquitto_connect_callback_set(client, &MqttDeviceClient::mqtt_connect_callback);
    bool ok = configure_mqtt_client(client,
                                    device_id_,
                                    device_secret_,
                                    status_topic(),
                                    offline,
                                    tls_,
                                    tls_insecure_,
                                    ca_dir_,
                                    "probe");

    if (ok) {
        std::fprintf(stdout,
                     "[mqtt] probe connecting host=%s port=%d tls=%d tls_insecure=%d topic=%s\n",
                     host_.c_str(),
                     port_,
                     tls_ ? 1 : 0,
                     tls_insecure_ ? 1 : 0,
                     status_topic().c_str());
        int rc = mosquitto_connect(client, host_.c_str(), port_, 60);
        if (rc != MOSQ_ERR_SUCCESS) {
            std::fprintf(stderr,
                         "[mqtt] probe connect call failed host=%s port=%d rc=%d error=%s\n",
                         host_.c_str(),
                         port_,
                         rc,
                         mosquitto_strerror(rc));
            ok = false;
        }
    }

    if (ok) {
        ok = wait_for_mqtt_connack(client, &connect_state, 8000);
    }
    if (ok) {
        int mid = 0;
        int rc = mosquitto_publish(client,
                                   &mid,
                                   status_topic().c_str(),
                                   static_cast<int>(online.size()),
                                   online.data(),
                                   1,
                                   true);
        if (rc != MOSQ_ERR_SUCCESS) {
            std::fprintf(stderr,
                         "[mqtt] probe publish online failed topic=%s rc=%d error=%s\n",
                         status_topic().c_str(),
                         rc,
                         mosquitto_strerror(rc));
            ok = false;
        } else {
            ok = flush_mqtt_loop(client, "probe_publish", 2000);
        }
    }

    mosquitto_disconnect(client);
    mosquitto_destroy(client);
    mosquitto_lib_cleanup();
    return ok;
}

void MqttDeviceClient::start() {
    if (mqtt_thread_.joinable()) return;
    stop_.store(false);
    publish_offline_.store(false);
    mqtt_thread_ = std::thread(&MqttDeviceClient::mqtt_loop, this);
}

void MqttDeviceClient::stop(bool publish_offline) {
    if (!mqtt_thread_.joinable()) return;
    publish_offline_.store(publish_offline);
    stop_.store(true);
    if (!publish_offline) {
        std::lock_guard<std::mutex> lock(mqtt_mtx_);
        if (mqtt_client_) {
            mosquitto_disconnect(mqtt_client_);
        }
    }
    mqtt_thread_.join();
    publish_offline_.store(false);
}

bool MqttDeviceClient::publish_signal(const std::string &payload) {
    return publish_payload(signal_topic(), payload, 1, false);
}

bool MqttDeviceClient::publish_media_state(const std::string &payload) {
    return publish_payload(media_state_topic(), payload, 1, true);
}

bool MqttDeviceClient::publish_command_ack(const std::string &trace_id,
                                           const std::string &cmd_id,
                                           bool ok,
                                           const std::string &error_code) {
    nlohmann::json ack;
    ack["trace_id"] = trace_id;
    ack["cmd_id"] = cmd_id;
    ack["device_id"] = device_id_;
    ack["ok"] = ok;
    ack["error_code"] = error_code.empty() ? nlohmann::json(nullptr) : nlohmann::json(error_code);
    ack["ts"] = iso_utc_now();
    return publish_payload(command_ack_topic(), ack.dump(), 1, false);
}

void MqttDeviceClient::mqtt_connect_callback(struct mosquitto *, void *userdata, int rc) {
    auto *state = static_cast<ConnectState *>(userdata);
    const char *label = state && state->label ? state->label : "unknown";
    if (state) {
        state->completed = true;
        state->rc = rc;
    }
    if (rc == MOSQ_ERR_SUCCESS) {
        std::fprintf(stdout, "[mqtt] connect accepted phase=%s\n", label);
    } else {
        std::fprintf(stderr,
                     "[mqtt] connect rejected phase=%s rc=%d error=%s\n",
                     label,
                     rc,
                     mosquitto_strerror(rc));
    }
}

void MqttDeviceClient::mqtt_message_callback(struct mosquitto *, void *userdata, const struct mosquitto_message *msg) {
    auto *state = static_cast<ConnectState *>(userdata);
    if (!state || !state->owner || !msg) return;
    state->owner->handle_message(msg);
}

void MqttDeviceClient::mqtt_loop() {
    mosquitto_lib_init();

    const std::string online = R"({"status":"online"})";
    const std::string offline = R"({"status":"offline"})";

    while (!should_stop()) {
        if (callbacks_.on_connecting) callbacks_.on_connecting();

        ConnectState connect_state;
        connect_state.label = "session";
        connect_state.owner = this;
        mosquitto *client = mosquitto_new(device_id_.c_str(), true, &connect_state);
        if (client) mosquitto_threaded_set(client, true);
        if (!client) {
            std::fprintf(stderr, "[mqtt] create session client failed\n");
            if (wait_for_stop_or(std::chrono::seconds(2))) break;
            continue;
        }

        {
            std::lock_guard<std::mutex> lock(mqtt_mtx_);
            mqtt_client_ = client;
        }

        mosquitto_connect_callback_set(client, &MqttDeviceClient::mqtt_connect_callback);
        mosquitto_message_callback_set(client, &MqttDeviceClient::mqtt_message_callback);

        bool connected = false;
        bool ok = configure_mqtt_client(client,
                                        device_id_,
                                        device_secret_,
                                        status_topic(),
                                        offline,
                                        tls_,
                                        tls_insecure_,
                                        ca_dir_,
                                        "session");

        if (ok) {
            std::fprintf(stdout,
                         "[mqtt] session connecting host=%s port=%d tls=%d tls_insecure=%d topic=%s\n",
                         host_.c_str(),
                         port_,
                         tls_ ? 1 : 0,
                         tls_insecure_ ? 1 : 0,
                         status_topic().c_str());
            int rc = mosquitto_connect(client, host_.c_str(), port_, 60);
            if (rc != MOSQ_ERR_SUCCESS) {
                std::fprintf(stderr,
                             "[mqtt] session connect call failed host=%s port=%d rc=%d error=%s\n",
                             host_.c_str(),
                             port_,
                             rc,
                             mosquitto_strerror(rc));
                ok = false;
            }
        }

        if (ok) {
            ok = wait_for_mqtt_connack(client, &connect_state, 8000);
            connected = ok;
        }
        if (ok) {
            int rc = mosquitto_publish(client,
                                       nullptr,
                                       status_topic().c_str(),
                                       static_cast<int>(online.size()),
                                       online.data(),
                                       1,
                                       true);
            if (rc != MOSQ_ERR_SUCCESS) {
                std::fprintf(stderr,
                             "[mqtt] session publish online failed topic=%s rc=%d error=%s\n",
                             status_topic().c_str(),
                             rc,
                             mosquitto_strerror(rc));
                ok = false;
            } else {
                ok = flush_mqtt_loop(client, "session_publish", 2000);
                int sub_rc = mosquitto_subscribe(client, nullptr, command_topic().c_str(), 1);
                if (sub_rc != MOSQ_ERR_SUCCESS) {
                    std::fprintf(stderr,
                                 "[mqtt] subscribe command failed topic=%s rc=%d error=%s\n",
                                 command_topic().c_str(),
                                 sub_rc,
                                 mosquitto_strerror(sub_rc));
                    ok = false;
                } else {
                    std::fprintf(stdout, "[mqtt] subscribed command topic=%s\n", command_topic().c_str());
                }
                if (ok) {
                    sub_rc = mosquitto_subscribe(client, nullptr, signal_topic().c_str(), 1);
                    if (sub_rc != MOSQ_ERR_SUCCESS) {
                        std::fprintf(stderr,
                                     "[mqtt] subscribe signal failed topic=%s rc=%d error=%s\n",
                                     signal_topic().c_str(),
                                     sub_rc,
                                     mosquitto_strerror(sub_rc));
                        ok = false;
                    } else {
                        std::fprintf(stdout, "[mqtt] subscribed signal topic=%s\n", signal_topic().c_str());
                    }
                }
            }
        }

        if (ok) {
            if (callbacks_.on_online) callbacks_.on_online();
            while (!should_stop()) {
                int rc = mosquitto_loop(client, 1000, 1);
                if (rc == MOSQ_ERR_SUCCESS) {
                    continue;
                }

                connected = false;
                std::fprintf(stderr,
                             "[mqtt] session lost rc=%d error=%s; will rebuild client\n",
                             rc,
                             mosquitto_strerror(rc));
                break;
            }
        }

        if ((should_stop() || publish_offline_.load()) && connected) {
            int rc = mosquitto_publish(client,
                                       nullptr,
                                       status_topic().c_str(),
                                       static_cast<int>(offline.size()),
                                       offline.data(),
                                       1,
                                       true);
            if (rc != MOSQ_ERR_SUCCESS) {
                std::fprintf(stderr,
                             "[mqtt] session publish offline failed topic=%s rc=%d error=%s\n",
                             status_topic().c_str(),
                             rc,
                             mosquitto_strerror(rc));
            } else if (flush_mqtt_loop(client, "session_offline", 1000)) {
                std::fprintf(stdout, "[mqtt] session published offline topic=%s\n", status_topic().c_str());
            }
        }

        mosquitto_disconnect(client);
        {
            std::lock_guard<std::mutex> lock(mqtt_mtx_);
            if (mqtt_client_ == client) {
                mqtt_client_ = nullptr;
            }
        }
        mosquitto_destroy(client);

        if (!should_stop()) {
            std::fprintf(stdout, "[mqtt] reconnect scheduled after session loss\n");
            wait_for_stop_or(std::chrono::seconds(2));
        }
    }

    {
        std::lock_guard<std::mutex> lock(mqtt_mtx_);
        mqtt_client_ = nullptr;
    }
    mosquitto_lib_cleanup();
}

void MqttDeviceClient::handle_message(const struct mosquitto_message *msg) {
    if (!msg || !msg->topic || !msg->payload) return;
    const std::string topic(msg->topic);
    const std::string body(static_cast<const char *>(msg->payload), static_cast<size_t>(msg->payloadlen));

    if (topic == signal_topic()) {
        if (callbacks_.signal_handler) callbacks_.signal_handler(body);
        return;
    }
    if (topic == command_topic()) {
        if (callbacks_.command_handler) callbacks_.command_handler(body);
    }
}

bool MqttDeviceClient::publish_payload(const std::string &topic, const std::string &payload, int qos, bool retained) {
    std::lock_guard<std::mutex> lock(mqtt_mtx_);
    if (!mqtt_client_) return false;
    int rc = mosquitto_publish(mqtt_client_,
                               nullptr,
                               topic.c_str(),
                               static_cast<int>(payload.size()),
                               payload.data(),
                               qos,
                               retained);
    if (rc != MOSQ_ERR_SUCCESS) {
        std::fprintf(stderr, "[mqtt] publish failed topic=%s rc=%d error=%s\n", topic.c_str(), rc, mosquitto_strerror(rc));
        return false;
    }
    return true;
}

bool MqttDeviceClient::wait_for_stop_or(std::chrono::milliseconds duration) const {
    if (callbacks_.wait_for_stop) {
        return callbacks_.wait_for_stop(duration);
    }
    std::this_thread::sleep_for(duration);
    return should_stop();
}

bool MqttDeviceClient::should_stop() const {
    if (stop_.load()) return true;
    if (callbacks_.should_stop) return callbacks_.should_stop();
    return false;
}

std::string MqttDeviceClient::status_topic() const {
    return "doorbell/devices/" + device_id_ + "/status";
}

std::string MqttDeviceClient::command_topic() const {
    return "doorbell/devices/" + device_id_ + "/command";
}

std::string MqttDeviceClient::command_ack_topic() const {
    return "doorbell/devices/" + device_id_ + "/command_ack";
}

std::string MqttDeviceClient::signal_topic() const {
    return "doorbell/devices/" + device_id_ + "/signal";
}

std::string MqttDeviceClient::media_state_topic() const {
    return "doorbell/devices/" + device_id_ + "/media_state";
}
