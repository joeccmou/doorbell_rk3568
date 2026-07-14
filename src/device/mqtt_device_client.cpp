#include "device/mqtt_device_client.h"
#include "device/mqtt_session_handshake.h"

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

bool configure_mqtt_client(mosquitto *client,
						   const std::string &device_id,
						   const std::string &device_secret,
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
    MqttSessionHandshake handshake;
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

template <typename Predicate>
bool wait_for_mqtt_condition(mosquitto *client,
                             MqttDeviceClient::ConnectState *state,
                             Predicate predicate,
                             const char *phase,
                             int timeout_ms) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    while (std::chrono::steady_clock::now() < deadline) {
        if (state && state->handshake.failed()) {
            std::fprintf(stderr, "[mqtt] handshake rejected phase=%s\n", phase);
            return false;
        }
        if (predicate()) return true;

        const int rc = mosquitto_loop(client, 200, 1);
        if (rc != MOSQ_ERR_SUCCESS) {
            std::fprintf(stderr,
                         "[mqtt] loop while waiting acknowledgement failed phase=%s rc=%d error=%s\n",
                         phase,
                         rc,
                         mosquitto_strerror(rc));
            return false;
        }
    }

    std::fprintf(stderr, "[mqtt] acknowledgement timeout phase=%s timeout_ms=%d\n", phase, timeout_ms);
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

void MqttDeviceClient::start() {
    if (mqtt_thread_.joinable()) return;
    stop_.store(false);
	set_online(false);
    mqtt_thread_ = std::thread(&MqttDeviceClient::mqtt_loop, this);
}

bool MqttDeviceClient::wait_until_online(std::chrono::milliseconds timeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    std::unique_lock<std::mutex> lock(online_mtx_);
    while (!online_) {
        if (should_stop() || std::chrono::steady_clock::now() >= deadline) return false;
        online_cv_.wait_for(lock, std::chrono::milliseconds(200));
    }
    return true;
}

void MqttDeviceClient::stop(bool /*publish_offline*/) {
	if (!mqtt_thread_.joinable()) return;
	stop_.store(true);
	online_cv_.notify_all();
	{
		std::lock_guard<std::mutex> lock(mqtt_mtx_);
		if (mqtt_client_) mosquitto_disconnect(mqtt_client_);
	}
	mqtt_thread_.join();
}

bool MqttDeviceClient::publish_signal(const std::string &payload) {
    return publish_payload(signal_topic(), payload, 1, false);
}

bool MqttDeviceClient::publish_media_state(const std::string &payload) {
    return publish_payload(media_state_topic(), payload, 1, true);
}

bool MqttDeviceClient::publish_event(const std::string &payload) {
    return publish_payload(event_topic(), payload, 1, false);
}

bool MqttDeviceClient::publish_time_sync(const std::string &payload) {
    return publish_payload(time_sync_topic(), payload, 1, true);
}

bool MqttDeviceClient::publish_capabilities(const std::string &payload) {
    return publish_payload(capabilities_topic(), payload, 1, true);
}

bool MqttDeviceClient::publish_command_ack(const std::string &trace_id,
                                           const std::string &cmd_id,
                                           bool ok,
                                           const std::string &error_code,
                                           const std::string &data_json) {
    nlohmann::json ack;
    ack["trace_id"] = trace_id;
    ack["cmd_id"] = cmd_id;
    ack["device_id"] = device_id_;
    ack["ok"] = ok;
    ack["error_code"] = error_code.empty() ? nlohmann::json(nullptr) : nlohmann::json(error_code);
    if (!data_json.empty()) {
        try {
            ack["data"] = nlohmann::json::parse(data_json);
        } catch (...) {
            ack["data"] = nlohmann::json::object();
        }
    }
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

void MqttDeviceClient::mqtt_subscribe_callback(struct mosquitto *,
                                               void *userdata,
                                               int message_id,
                                               int granted_qos_count,
                                               const int *granted_qos) {
    auto *state = static_cast<ConnectState *>(userdata);
    if (!state) return;

    bool accepted = granted_qos_count > 0 && granted_qos != nullptr;
    for (int i = 0; accepted && i < granted_qos_count; ++i) {
        if (granted_qos[i] == 0x80) accepted = false;
    }
    state->handshake.acknowledge_subscription(message_id, accepted);
    if (!accepted) {
        std::fprintf(stderr, "[mqtt] subscription rejected message_id=%d\n", message_id);
    }
}

void MqttDeviceClient::mqtt_message_callback(struct mosquitto *, void *userdata, const struct mosquitto_message *msg) {
    auto *state = static_cast<ConnectState *>(userdata);
    if (!state || !state->owner || !msg) return;
    state->owner->handle_message(msg);
}

void MqttDeviceClient::mqtt_loop() {
    mosquitto_lib_init();

	const std::string ready = R"({"status":"ready"})";

    while (!should_stop()) {
        set_online(false);
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
        mosquitto_subscribe_callback_set(client, &MqttDeviceClient::mqtt_subscribe_callback);
        mosquitto_message_callback_set(client, &MqttDeviceClient::mqtt_message_callback);
        mosquitto_publish_callback_set(client, &MqttDeviceClient::mqtt_publish_callback);

		bool ok = configure_mqtt_client(client,
										device_id_,
										device_secret_,
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
		}
        if (ok) {
            int command_subscription_id = 0;
            int sub_rc = mosquitto_subscribe(client, &command_subscription_id, command_topic().c_str(), 1);
            if (sub_rc != MOSQ_ERR_SUCCESS) {
                std::fprintf(stderr,
                             "[mqtt] subscribe command failed topic=%s rc=%d error=%s\n",
                             command_topic().c_str(),
                             sub_rc,
                             mosquitto_strerror(sub_rc));
                ok = false;
            } else {
                connect_state.handshake.expect_subscription(command_subscription_id);
            }
            if (ok) {
                int signal_subscription_id = 0;
                sub_rc = mosquitto_subscribe(client, &signal_subscription_id, signal_topic().c_str(), 1);
                if (sub_rc != MOSQ_ERR_SUCCESS) {
                    std::fprintf(stderr,
                                 "[mqtt] subscribe signal failed topic=%s rc=%d error=%s\n",
                                 signal_topic().c_str(),
                                 sub_rc,
                                 mosquitto_strerror(sub_rc));
                    ok = false;
                } else {
                    connect_state.handshake.expect_subscription(signal_subscription_id);
                }
            }
            if (ok) {
                ok = wait_for_mqtt_condition(
                    client,
                    &connect_state,
                    [&connect_state] { return connect_state.handshake.subscriptions_ready(); },
                    "session_subscribe",
                    5000);
                if (ok) {
                    std::fprintf(stdout,
                                 "[mqtt] subscriptions acknowledged command=%s signal=%s\n",
                                 command_topic().c_str(),
                                 signal_topic().c_str());
                }
            }
            if (ok) {
				int ready_message_id = 0;
				int rc = mosquitto_publish(client,
									   &ready_message_id,
									   status_topic().c_str(),
									   static_cast<int>(ready.size()),
									   ready.data(),
									   1,
									   false);
				if (rc != MOSQ_ERR_SUCCESS) {
					std::fprintf(stderr,
								 "[mqtt] session publish ready failed topic=%s rc=%d error=%s\n",
                                 status_topic().c_str(),
                                 rc,
                                 mosquitto_strerror(rc));
                    ok = false;
                } else {
					connect_state.handshake.expect_ready_publish(ready_message_id);
					ok = wait_for_mqtt_condition(
                        client,
                        &connect_state,
						[&connect_state] { return connect_state.handshake.ready_publish_acknowledged(); },
						"session_ready_publish",
                        5000);
                }
            }
        }

        if (ok) {
            if (callbacks_.on_online) callbacks_.on_online();
            set_online(true);
            while (!should_stop()) {
                int rc = mosquitto_loop(client, 1000, 1);
                if (rc == MOSQ_ERR_SUCCESS) {
                    continue;
                }

				set_online(false);
                std::fprintf(stderr,
                             "[mqtt] session lost rc=%d error=%s; will rebuild client\n",
                             rc,
                             mosquitto_strerror(rc));
                break;
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
        set_online(false);

        if (!should_stop()) {
            std::fprintf(stdout, "[mqtt] reconnect scheduled after session loss\n");
            wait_for_stop_or(std::chrono::seconds(2));
        }
    }

    {
        std::lock_guard<std::mutex> lock(mqtt_mtx_);
        mqtt_client_ = nullptr;
    }
    set_online(false);
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
    int message_id = 0;
    int rc = MOSQ_ERR_NO_CONN;
    {
        std::lock_guard<std::mutex> lock(mqtt_mtx_);
        if (!mqtt_client_) return false;
        rc = mosquitto_publish(mqtt_client_,
                               &message_id,
                               topic.c_str(),
                               static_cast<int>(payload.size()),
                               payload.data(),
                               qos,
                               retained);
    }
    if (rc != MOSQ_ERR_SUCCESS) {
        std::fprintf(stderr, "[mqtt] publish failed topic=%s rc=%d error=%s\n", topic.c_str(), rc, mosquitto_strerror(rc));
        return false;
    }
    if (qos == 0) return true;
	if (mqtt_thread_.joinable() && std::this_thread::get_id() == mqtt_thread_.get_id()) {
		// 当前就在 mosquitto_loop 回调中，不能同步等待由同一线程处理的 PUBACK。
		std::lock_guard<std::mutex> delivery_lock(publish_mtx_);
		nonblocking_message_ids_.insert(message_id);
		return true;
	}

    std::unique_lock<std::mutex> delivery_lock(publish_mtx_);
    const bool acknowledged = publish_cv_.wait_for(delivery_lock, std::chrono::seconds(5), [this, message_id] {
        return published_message_ids_.find(message_id) != published_message_ids_.end() || stop_.load();
    });
    published_message_ids_.erase(message_id);
    if (!acknowledged || stop_.load()) {
        std::fprintf(stderr, "[mqtt] publish acknowledgement timeout topic=%s message_id=%d\n", topic.c_str(), message_id);
        return false;
    }
    return true;
}

void MqttDeviceClient::mqtt_publish_callback(struct mosquitto *, void *userdata, int message_id) {
    auto *state = static_cast<ConnectState *>(userdata);
    if (!state || !state->owner) return;
    if (state->handshake.acknowledge_publish(message_id)) return;
    {
        std::lock_guard<std::mutex> lock(state->owner->publish_mtx_);
		if (state->owner->nonblocking_message_ids_.erase(message_id) > 0) {
			return;
		}
        state->owner->published_message_ids_.insert(message_id);
    }
    state->owner->publish_cv_.notify_all();
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

void MqttDeviceClient::set_online(bool online) {
    {
        std::lock_guard<std::mutex> lock(online_mtx_);
        online_ = online;
    }
    online_cv_.notify_all();
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

std::string MqttDeviceClient::event_topic() const {
    return "doorbell/devices/" + device_id_ + "/event";
}

std::string MqttDeviceClient::time_sync_topic() const {
    return "doorbell/devices/" + device_id_ + "/time_sync";
}

std::string MqttDeviceClient::capabilities_topic() const {
    return "doorbell/devices/" + device_id_ + "/capabilities";
}
