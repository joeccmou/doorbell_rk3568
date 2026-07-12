#include "device/provisioning.h"
#include "device/live_view_session.h"
#include "device/mqtt_device_client.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cctype>
#include <cstdio>
#include <ctime>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>
#include <utility>

#include <nlohmann/json.hpp>

namespace {

constexpr const char *kDefaultDataDir = "/userdata/doorbell";
constexpr const char *kDefaultIdentityPath = "/userdata/doorbell/identity.json";
constexpr const char *kWifiConfigName = "wifi.json";
constexpr const char *kApAddress = "192.168.4.1";
constexpr int kHttpBacklog = 8;


std::string iso_utc_now() {
    std::time_t now = std::time(nullptr);
    std::tm tm{};
    gmtime_r(&now, &tm);
    char buf[32]{};
    std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tm);
    return std::string(buf);
}
std::string env_or(const char *name, const char *fallback) {
    const char *value = std::getenv(name);
    return (value && value[0] != '\0') ? std::string(value) : std::string(fallback);
}

int env_int_or(const char *name, int fallback) {
    const char *value = std::getenv(name);
    if (!value || value[0] == '\0') return fallback;
    char *end = nullptr;
    long parsed = std::strtol(value, &end, 10);
    return (end && *end == '\0') ? static_cast<int>(parsed) : fallback;
}

unsigned int env_uint_or(const char *name, unsigned int fallback) {
    int value = env_int_or(name, static_cast<int>(fallback));
    return value >= 0 ? static_cast<unsigned int>(value) : fallback;
}

bool env_bool_or(const char *name, bool fallback) {
    const char *value = std::getenv(name);
    if (!value || value[0] == '\0') return fallback;
    std::string v(value);
    std::transform(v.begin(), v.end(), v.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    if (v == "1" || v == "true" || v == "yes" || v == "on") return true;
    if (v == "0" || v == "false" || v == "no" || v == "off") return false;
    return fallback;
}

bool ensure_dir(const std::string &path) {
    std::error_code ec;
    std::filesystem::create_directories(path, ec);
    return !ec;
}

std::string read_file(const std::string &path) {
    std::ifstream in(path);
    if (!in) return "";
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

bool write_file(const std::string &path, const std::string &content, std::string *error = nullptr) {
    int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    if (fd < 0) {
        if (error) *error = std::strerror(errno);
        return false;
    }

    const char *data = content.data();
    size_t remaining = content.size();
    while (remaining > 0) {
        ssize_t n = ::write(fd, data, remaining);
        if (n < 0) {
            if (errno == EINTR) continue;
            if (error) *error = std::strerror(errno);
            ::close(fd);
            return false;
        }
        data += n;
        remaining -= static_cast<size_t>(n);
    }

    if (::close(fd) != 0) {
        if (error) *error = std::strerror(errno);
        return false;
    }
    return true;
}

std::string shell_quote(const std::string &value) {
    std::string out = "'";
    for (char c : value) {
        if (c == '\'') {
            out += "'\\''";
        } else {
            out += c;
        }
    }
    out += "'";
    return out;
}

int run_command(const std::string &cmd, bool log_failure = true) {
    std::fprintf(stdout, "[provision] run: %s\n", cmd.c_str());
    int rc = std::system(cmd.c_str());
    if (rc != 0 && log_failure) {
        std::fprintf(stderr, "[provision] command failed rc=%d cmd=%s\n", rc, cmd.c_str());
    }
    return rc;
}

std::string command_path(const std::string &name) {
    const std::array<std::string, 4> dirs = {"/usr/sbin/", "/sbin/", "/usr/bin/", "/bin/"};
    for (const auto &dir : dirs) {
        const std::string path = dir + name;
        if (::access(path.c_str(), X_OK) == 0) {
            return path;
        }
    }
    return name;
}

bool command_available(const char *name) {
    const std::string path = command_path(name);
    if (path != name) return true;
    return run_command("command -v " + shell_quote(name) + " >/dev/null 2>&1", false) == 0;
}

std::string run_command_capture(const std::string &cmd, int *exit_code = nullptr) {
    if (exit_code) *exit_code = -1;
    std::array<char, 256> buf{};
    std::string out;
    FILE *pipe = ::popen(cmd.c_str(), "r");
    if (!pipe) {
        return out;
    }
    while (std::fgets(buf.data(), static_cast<int>(buf.size()), pipe)) {
        out += buf.data();
    }
    const int rc = ::pclose(pipe);
    if (exit_code) *exit_code = rc;
    return out;
}

std::string lower_ascii(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return value;
}

std::string trim_ascii(const std::string &value) {
    size_t begin = 0;
    while (begin < value.size() && std::isspace(static_cast<unsigned char>(value[begin]))) {
        ++begin;
    }
    size_t end = value.size();
    while (end > begin && std::isspace(static_cast<unsigned char>(value[end - 1]))) {
        --end;
    }
    return value.substr(begin, end - begin);
}

struct HttpRequestLine {
    std::string method;
    std::string path;
    std::string version;
};

struct HttpHeaders {
    bool has_content_length = false;
    size_t content_length = 0;
    bool chunked = false;
};

HttpRequestLine parse_request_line(const std::string &line) {
    HttpRequestLine out;
    std::istringstream in(line);
    in >> out.method >> out.path >> out.version;
    const size_t query = out.path.find('?');
    if (query != std::string::npos) {
        out.path.resize(query);
    }
    return out;
}

HttpHeaders parse_headers(const std::string &request, size_t line_end, size_t header_end) {
    HttpHeaders out;
    size_t pos = line_end + 2;
    while (pos < header_end) {
        const size_t next = request.find("\r\n", pos);
        const size_t end = next == std::string::npos || next > header_end ? header_end : next;
        const std::string line = request.substr(pos, end - pos);
        const size_t colon = line.find(':');
        if (colon != std::string::npos) {
            const std::string name = lower_ascii(trim_ascii(line.substr(0, colon)));
            const std::string value = trim_ascii(line.substr(colon + 1));
            if (name == "content-length") {
                char *parse_end = nullptr;
                unsigned long parsed = std::strtoul(value.c_str(), &parse_end, 10);
                if (parse_end != value.c_str()) {
                    out.has_content_length = true;
                    out.content_length = static_cast<size_t>(parsed);
                }
            } else if (name == "transfer-encoding") {
                out.chunked = lower_ascii(value).find("chunked") != std::string::npos;
            }
        }
        if (next == std::string::npos || next >= header_end) break;
        pos = next + 2;
    }
    return out;
}

bool write_all(int fd, const char *data, size_t len) {
    size_t sent = 0;
    while (sent < len) {
        ssize_t n = ::send(fd, data + sent, len - sent, MSG_NOSIGNAL);
        if (n < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        sent += static_cast<size_t>(n);
    }
    return true;
}

std::string http_response(int status, const char *reason, const nlohmann::json &body) {
    const std::string payload = body.dump();
    std::ostringstream out;
    out << "HTTP/1.1 " << status << " " << reason << "\r\n";
    out << "Content-Type: application/json\r\n";
    out << "Content-Length: " << payload.size() << "\r\n";
    out << "Connection: close\r\n\r\n";
    out << payload;
    return out.str();
}

std::string wpa_escape(const std::string &value) {
    std::string out = "\"";
    for (char c : value) {
        if (c == '\\' || c == '"') out += '\\';
        out += c;
    }
    out += '"';
    return out;
}

struct WpaStatus {
    bool available = false;
    bool completed = false;
    std::string ssid;
    std::string ip_address;
};

std::string key_value_from_lines(const std::string &text, const std::string &key) {
    std::istringstream in(text);
    std::string line;
    const std::string prefix = key + "=";
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.rfind(prefix, 0) == 0) {
            return line.substr(prefix.size());
        }
    }
    return "";
}

WpaStatus read_wpa_status(const std::string &iface) {
    WpaStatus status;
    if (!command_available("wpa_cli")) {
        return status;
    }

    int rc = -1;
    const std::string cmd = shell_quote(command_path("wpa_cli")) + " -i " + shell_quote(iface) + " status 2>/dev/null";
    const std::string out = run_command_capture(cmd, &rc);
    if (rc != 0 || out.empty()) {
        return status;
    }

    status.available = true;
    status.ssid = key_value_from_lines(out, "ssid");
    status.ip_address = key_value_from_lines(out, "ip_address");
    status.completed = key_value_from_lines(out, "wpa_state") == "COMPLETED";
    return status;
}

bool wait_for_wpa_connected(const std::string &iface, const std::string &ssid) {
    bool dhcp_requested = false;
    for (int i = 0; i < 45; ++i) {
        WpaStatus status = read_wpa_status(iface);
        if (status.available && status.completed && status.ssid == ssid) {
            if (!status.ip_address.empty()) {
                std::fprintf(stdout,
                             "[provision] STA connected iface=%s ssid=%s ip=%s\n",
                             iface.c_str(), ssid.c_str(), status.ip_address.c_str());
                return true;
            }
            if (!dhcp_requested && command_available("udhcpc")) {
                dhcp_requested = true;
                run_command(shell_quote(command_path("udhcpc")) + " -i " + shell_quote(iface) + " -q -t 5 -n");
            }
        }
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }
    return false;
}

bool apply_wpa_cli_network(const std::string &iface, const std::string &ssid, const std::string &password) {
    if (!command_available("wpa_cli")) {
        return false;
    }

    const std::string script_path = "/tmp/doorbell_wpa_cli_update.sh";
    std::ostringstream script;
    script << "#!/bin/sh\n";
    script << "set -eu\n";
    script << "WPA=" << shell_quote(command_path("wpa_cli")) << "\n";
    script << "IFACE=" << shell_quote(iface) << "\n";
    script << "id=\"$($WPA -i \"$IFACE\" add_network | tail -n 1)\"\n";
    script << "case \"$id\" in ''|FAIL*) exit 1;; esac\n";
    script << "$WPA -i \"$IFACE\" set_network \"$id\" ssid " << shell_quote(wpa_escape(ssid)) << " >/dev/null\n";
    if (password.empty()) {
        script << "$WPA -i \"$IFACE\" set_network \"$id\" key_mgmt NONE >/dev/null\n";
    } else {
        script << "$WPA -i \"$IFACE\" set_network \"$id\" psk " << shell_quote(wpa_escape(password)) << " >/dev/null\n";
    }
    script << "$WPA -i \"$IFACE\" enable_network \"$id\" >/dev/null\n";
    script << "$WPA -i \"$IFACE\" select_network \"$id\" >/dev/null\n";

    if (!write_file(script_path, script.str())) {
        return false;
    }
    const int rc = run_command("/bin/sh " + shell_quote(script_path));
    std::error_code ec;
    std::filesystem::remove(script_path, ec);
    return rc == 0;
}

} // namespace

void DoorbellProvisioning::GpioLine::close() {
    if (line) {
        gpiod_line_release(line);
        line = nullptr;
    }
    if (chip) {
        gpiod_chip_close(chip);
        chip = nullptr;
    }
}

DoorbellProvisioning::DoorbellProvisioning()
    : identity_path_(env_or("DOORBELL_IDENTITY_PATH", kDefaultIdentityPath)),
      data_dir_(env_or("DOORBELL_DATA_DIR", kDefaultDataDir)),
      wifi_path_(data_dir_ + "/" + kWifiConfigName),
      ap_iface_(env_or("DOORBELL_AP_IFACE", "wlan0")),
      sta_iface_(env_or("DOORBELL_STA_IFACE", "p2p0")),
      mqtt_host_(env_or("DOORBELL_MQTT_HOST", "smartdoorbell.site")),
      mqtt_ca_dir_(env_or("DOORBELL_MQTT_CA_DIR", "/etc/ssl/certs")),
      mqtt_tls_(env_bool_or("DOORBELL_MQTT_TLS", true)),
      mqtt_tls_insecure_(env_bool_or("DOORBELL_MQTT_TLS_INSECURE", true)),
      mqtt_port_(env_int_or("DOORBELL_MQTT_PORT", 8883)),
      http_port_(env_int_or("DOORBELL_PROVISION_HTTP_PORT", 80)),
      led_name_(env_or("DOORBELL_LED_NAME", "work-led")),
      led_path_(env_or("DOORBELL_LED_PATH", "")),
      button_chip_(env_or("DOORBELL_BUTTON_GPIOCHIP", "gpiochip3")),
      button_line_(env_uint_or("DOORBELL_BUTTON_LINE", 9)),
      button_active_low_(env_bool_or("DOORBELL_BUTTON_ACTIVE_LOW", true)) {
    if (led_path_.empty()) {
        led_path_ = "/sys/class/leds/" + led_name_;
    }
    led_trigger_path_ = led_path_ + "/trigger";
    led_brightness_path_ = led_path_ + "/brightness";
}

DoorbellProvisioning::~DoorbellProvisioning() {
    stop();
}

bool DoorbellProvisioning::publish_event(const std::string &payload) {
    return mqtt_client_ && mqtt_client_->publish_event(payload);
}

bool DoorbellProvisioning::start() {
    if (!ensure_dir(data_dir_)) {
        std::fprintf(stderr, "[provision] failed to create data dir: %s\n", data_dir_.c_str());
        return false;
    }
    if (!load_identity()) {
        std::fprintf(stderr, "[provision] identity missing or invalid: %s\n", identity_path_.c_str());
        return false;
    }

    LiveViewSession::Publishers live_publishers;
    live_publishers.command_ack_publisher = [this](const std::string &trace_id,
                                                   const std::string &cmd_id,
                                                   bool ok,
                                                   const std::string &error_code) {
        if (mqtt_client_) {
            mqtt_client_->publish_command_ack(trace_id, cmd_id, ok, error_code);
        }
    };
    live_publishers.signal_publisher = [this](const std::string &payload) {
        if (mqtt_client_) {
            mqtt_client_->publish_signal(payload);
        }
    };
    live_publishers.media_state_publisher = [this](const std::string &payload) {
        if (mqtt_client_) {
            mqtt_client_->publish_media_state(payload);
        }
    };
    live_view_session_ = std::make_unique<LiveViewSession>(identity_.device_id, std::move(live_publishers));

    MqttDeviceClient::Callbacks mqtt_callbacks;
    mqtt_callbacks.command_handler = [this](const std::string &payload) {
        if (live_view_session_) {
            live_view_session_->handle_command(payload);
        }
    };
    mqtt_callbacks.signal_handler = [this](const std::string &payload) {
        if (live_view_session_) {
            live_view_session_->handle_signal(payload);
        }
    };
    mqtt_callbacks.wait_for_stop = [this](std::chrono::milliseconds duration) {
        return wait_for_stop_or(duration);
    };
    mqtt_callbacks.should_stop = [this]() {
        return stop_.load();
    };
    mqtt_callbacks.on_connecting = [this]() {
        set_stage(Stage::ConnectingCloud);
    };
    mqtt_callbacks.on_online = [this]() {
        set_stage(Stage::Online);
    };
    mqtt_client_ = std::make_unique<MqttDeviceClient>(identity_.device_id,
                                                      identity_.device_secret,
                                                      mqtt_host_,
                                                      mqtt_port_,
                                                      mqtt_ca_dir_,
                                                      mqtt_tls_,
                                                      mqtt_tls_insecure_,
                                                      std::move(mqtt_callbacks));

    stop_.store(false);
    start_led();
    start_button();
    start_http_server();

    WifiCredentials saved;
    if (load_saved_wifi(&saved)) {
        begin_provisioning(saved);
    } else {
        set_stage(Stage::WaitingProvision);
        start_access_point();
    }
    return true;
}

void DoorbellProvisioning::stop() {
    bool was_stopped = stop_.exchange(true);
    if (was_stopped) return;
    stop_cv_.notify_all();

    stop_http_server();
    if (live_view_session_) live_view_session_->stop();
    if (provision_thread_.joinable()) provision_thread_.join();
    if (mqtt_client_) mqtt_client_->stop(true);
    stop_button();
    stop_led();
    stop_access_point();
}

bool DoorbellProvisioning::load_identity() {
    try {
        auto j = nlohmann::json::parse(read_file(identity_path_));
        identity_.device_id = j.value("device_id", "");
        identity_.device_secret = j.value("device_secret", "");
        identity_.model = j.value("model", "");
        identity_.ap_ssid = j.value("ap_ssid", "");
        identity_.ap_pass = j.value("ap_pass", "");
    } catch (const std::exception &e) {
        std::fprintf(stderr, "[provision] parse identity failed: %s\n", e.what());
        return false;
    }

    return !identity_.device_id.empty() &&
           !identity_.device_secret.empty() &&
           !identity_.ap_ssid.empty() &&
           identity_.ap_pass.size() >= 8;
}

bool DoorbellProvisioning::load_saved_wifi(WifiCredentials *out) const {
    try {
        auto j = nlohmann::json::parse(read_file(wifi_path_));
        WifiCredentials wifi;
        wifi.ssid = j.value("ssid", "");
        wifi.password = j.value("password", "");
        if (wifi.ssid.empty()) return false;
        if (out) *out = std::move(wifi);
        return true;
    } catch (...) {
        return false;
    }
}

bool DoorbellProvisioning::save_wifi(const WifiCredentials &wifi) const {
    nlohmann::json j;
    j["ssid"] = wifi.ssid;
    j["password"] = wifi.password;
    return write_file(wifi_path_, j.dump(2) + "\n");
}

void DoorbellProvisioning::clear_saved_wifi() const {
    std::error_code ec;
    std::filesystem::remove(wifi_path_, ec);
}

bool DoorbellProvisioning::start_access_point() {
    const std::string hostapd_conf = data_dir_ + "/doorbell_hostapd.conf";
    const std::string dnsmasq_conf = data_dir_ + "/doorbell_dnsmasq.conf";

    if (::geteuid() != 0) {
        std::fprintf(stderr,
                     "[provision] SoftAP requires root privileges or CAP_NET_ADMIN; "
                     "run with sudo/systemd service privileges\n");
    }

    bool missing_dependency = false;
    const char *required_commands[] = {"ip", "hostapd", "dnsmasq"};
    for (const char *cmd : required_commands) {
        if (!command_available(cmd)) {
            std::fprintf(stderr, "[provision] missing SoftAP dependency: %s\n", cmd);
            missing_dependency = true;
        }
    }
    if (missing_dependency) {
        set_stage(Stage::WifiFailed, "SOFTAP_DEPENDENCY_MISSING");
        return false;
    }

    std::ostringstream hostapd;
    hostapd << "interface=" << ap_iface_ << "\n";
    hostapd << "driver=nl80211\n";
    hostapd << "ssid=" << identity_.ap_ssid << "\n";
    hostapd << "hw_mode=g\n";
    hostapd << "channel=6\n";
    hostapd << "auth_algs=1\n";
    hostapd << "wpa=2\n";
    hostapd << "wpa_passphrase=" << identity_.ap_pass << "\n";
    hostapd << "wpa_key_mgmt=WPA-PSK\n";
    hostapd << "rsn_pairwise=CCMP\n";
    std::string write_error;
    if (!write_file(hostapd_conf, hostapd.str(), &write_error)) {
        std::fprintf(stderr, "[provision] failed to write hostapd config: %s error=%s\n",
                     hostapd_conf.c_str(), write_error.c_str());
        set_stage(Stage::WifiFailed, "SOFTAP_CONFIG_FAILED");
        return false;
    }

    std::ostringstream dnsmasq;
    dnsmasq << "interface=" << ap_iface_ << "\n";
    dnsmasq << "bind-interfaces\n";
    dnsmasq << "dhcp-range=192.168.4.10,192.168.4.80,255.255.255.0,12h\n";
    dnsmasq << "dhcp-option=3," << kApAddress << "\n";
    dnsmasq << "dhcp-option=6," << kApAddress << "\n";
    if (!write_file(dnsmasq_conf, dnsmasq.str(), &write_error)) {
        std::fprintf(stderr, "[provision] failed to write dnsmasq config: %s error=%s\n",
                     dnsmasq_conf.c_str(), write_error.c_str());
        set_stage(Stage::WifiFailed, "SOFTAP_CONFIG_FAILED");
        return false;
    }

    run_command("killall hostapd >/dev/null 2>&1", false);
    run_command("killall dnsmasq >/dev/null 2>&1", false);

    bool ok = true;
    ok = run_command(shell_quote(command_path("ip")) + " link set " + shell_quote(ap_iface_) + " up") == 0 && ok;
    ok = run_command(shell_quote(command_path("ip")) + " addr flush dev " + shell_quote(ap_iface_)) == 0 && ok;
    ok = run_command(shell_quote(command_path("ip")) + " addr add 192.168.4.1/24 dev " + shell_quote(ap_iface_)) == 0 && ok;
    ok = run_command(shell_quote(command_path("hostapd")) + " -B " + shell_quote(hostapd_conf)) == 0 && ok;
    ok = run_command(shell_quote(command_path("dnsmasq")) + " -C " + shell_quote(dnsmasq_conf)) == 0 && ok;
    if (!ok) {
        set_stage(Stage::WifiFailed, "SOFTAP_START_FAILED");
        std::fprintf(stderr, "[provision] SoftAP failed ssid=%s iface=%s\n",
                     identity_.ap_ssid.c_str(), ap_iface_.c_str());
        return false;
    }

    std::fprintf(stdout, "[provision] SoftAP started ssid=%s iface=%s\n",
                 identity_.ap_ssid.c_str(), ap_iface_.c_str());
    return true;
}

void DoorbellProvisioning::stop_access_point() {
    run_command("killall hostapd >/dev/null 2>&1", false);
    run_command("killall dnsmasq >/dev/null 2>&1", false);
}

bool DoorbellProvisioning::connect_sta(const WifiCredentials &wifi) {
    WpaStatus current = read_wpa_status(sta_iface_);
    if (current.available) {
        std::fprintf(stdout,
                     "[provision] current STA iface=%s ssid=%s completed=%d ip=%s\n",
                     sta_iface_.c_str(),
                     current.ssid.empty() ? "none" : current.ssid.c_str(),
                     current.completed ? 1 : 0,
                     current.ip_address.empty() ? "none" : current.ip_address.c_str());
        if (current.completed && current.ssid == wifi.ssid) {
            if (!current.ip_address.empty()) {
                return true;
            }
            if (command_available("udhcpc")) {
                run_command(shell_quote(command_path("udhcpc")) + " -i " + shell_quote(sta_iface_) + " -q -t 5 -n");
            }
            return wait_for_wpa_connected(sta_iface_, wifi.ssid);
        }

        std::fprintf(stdout, "[provision] switching STA iface=%s to requested ssid=%s\n",
                     sta_iface_.c_str(), wifi.ssid.c_str());
        if (!apply_wpa_cli_network(sta_iface_, wifi.ssid, wifi.password)) {
            std::fprintf(stderr, "[provision] wpa_cli network switch failed iface=%s ssid=%s\n",
                         sta_iface_.c_str(), wifi.ssid.c_str());
            return false;
        }
        return wait_for_wpa_connected(sta_iface_, wifi.ssid);
    }

    const std::string wpa_conf = "/tmp/doorbell_wpa_supplicant.conf";
    std::ostringstream conf;
    conf << "ctrl_interface=/var/run/wpa_supplicant\n";
    conf << "update_config=0\n";
    conf << "network={\n";
    conf << "    ssid=" << wpa_escape(wifi.ssid) << "\n";
    if (wifi.password.empty()) {
        conf << "    key_mgmt=NONE\n";
    } else {
        conf << "    psk=" << wpa_escape(wifi.password) << "\n";
        conf << "    key_mgmt=WPA-PSK\n";
    }
    conf << "}\n";
    if (!write_file(wpa_conf, conf.str())) return false;

    if (!command_available("ip") || !command_available("wpa_supplicant")) {
        std::fprintf(stderr, "[provision] missing STA dependency: ip or wpa_supplicant\n");
        return false;
    }

    run_command(shell_quote(command_path("ip")) + " link set " + shell_quote(sta_iface_) + " up");
    run_command(shell_quote(command_path("ip")) + " addr flush dev " + shell_quote(sta_iface_));
    if (run_command(shell_quote(command_path("wpa_supplicant")) + " -B -i " + shell_quote(sta_iface_) + " -c " + shell_quote(wpa_conf)) != 0) {
        return false;
    }

    return wait_for_wpa_connected(sta_iface_, wifi.ssid);
}

void DoorbellProvisioning::start_http_server() {
    http_stop_.store(false);
    http_thread_ = std::thread(&DoorbellProvisioning::http_server_loop, this);
}

void DoorbellProvisioning::stop_http_server() {
    http_stop_.store(true);
    int fd = http_fd_.exchange(-1);
    if (fd >= 0) ::shutdown(fd, SHUT_RDWR);
    if (fd >= 0) ::close(fd);
    if (http_thread_.joinable()) http_thread_.join();
}

void DoorbellProvisioning::http_server_loop() {
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        std::perror("[provision] socket");
        return;
    }

    int yes = 1;
    ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(static_cast<uint16_t>(http_port_));
    if (::bind(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) < 0) {
        std::perror("[provision] bind http");
        if (errno == EACCES && http_port_ < 1024 && ::geteuid() != 0) {
            std::fprintf(stderr,
                         "[provision] HTTP port %d requires root privileges; "
                         "run with sudo or set DOORBELL_PROVISION_HTTP_PORT>=1024\n",
                         http_port_);
        }
        ::close(fd);
        return;
    }
    if (::listen(fd, kHttpBacklog) < 0) {
        std::perror("[provision] listen http");
        ::close(fd);
        return;
    }

    http_fd_.store(fd);
    std::fprintf(stdout, "[provision] HTTP server listening on 0.0.0.0:%d\n", http_port_);
    while (!http_stop_.load() && !stop_.load()) {
        sockaddr_in peer{};
        socklen_t peer_len = sizeof(peer);
        int client = ::accept(fd, reinterpret_cast<sockaddr *>(&peer), &peer_len);
        if (client < 0) {
            if (errno == EINTR) continue;
            if (http_stop_.load() || stop_.load()) break;
            std::perror("[provision] accept");
            continue;
        }
        handle_http_client(client);
        ::close(client);
    }
}

void DoorbellProvisioning::handle_http_client(int client_fd) {
    std::string request;
    char buf[1024];
    while (request.find("\r\n\r\n") == std::string::npos && request.size() < 8192) {
        ssize_t n = ::recv(client_fd, buf, sizeof(buf), 0);
        if (n <= 0) return;
        request.append(buf, static_cast<size_t>(n));
    }

    const size_t line_end = request.find("\r\n");
    const std::string first = line_end == std::string::npos ? request : request.substr(0, line_end);
    const size_t header_end = request.find("\r\n\r\n");
    const HttpRequestLine req = parse_request_line(first);
    const HttpHeaders headers = header_end == std::string::npos
        ? HttpHeaders{}
        : parse_headers(request, line_end, header_end);
    std::string body = header_end == std::string::npos ? "" : request.substr(header_end + 4);

    std::string response;
    if (headers.chunked) {
        response = http_response(400, "Bad Request",
                                 {{"code", "UNSUPPORTED_TRANSFER_ENCODING"},
                                  {"message", "chunked transfer is not supported"}});
        write_all(client_fd, response.data(), response.size());
        return;
    }
    if (headers.has_content_length && headers.content_length > 4096) {
        response = http_response(413, "Payload Too Large",
                                 {{"code", "PAYLOAD_TOO_LARGE"}, {"message", "payload too large"}});
        write_all(client_fd, response.data(), response.size());
        return;
    }
    while (headers.has_content_length && body.size() < headers.content_length) {
        ssize_t n = ::recv(client_fd, buf, sizeof(buf), 0);
        if (n <= 0) break;
        body.append(buf, static_cast<size_t>(n));
    }
    if (headers.has_content_length && body.size() > headers.content_length) {
        body.resize(headers.content_length);
    }

    std::fprintf(stdout,
                 "[provision] http %s %s content_length=%zu body_size=%zu\n",
                 req.method.c_str(),
                 req.path.c_str(),
                 headers.has_content_length ? headers.content_length : 0,
                 body.size());

    if (req.method == "GET" && req.path == "/provision/status") {
        ProvisionStatus s = status_snapshot();
        nlohmann::json out;
        out["wifi_ok"] = s.wifi_ok;
        out["cloud_ok"] = s.cloud_ok;
        out["error_code"] = s.error_code.empty() ? nlohmann::json(nullptr) : nlohmann::json(s.error_code);
        out["stage"] = stage_name(stage());
        response = http_response(200, "OK", out);
    } else if (req.method == "POST" && req.path == "/provision") {
        if (headers.has_content_length && body.size() < headers.content_length) {
            response = http_response(400, "Bad Request",
                                     {{"code", "INCOMPLETE_BODY"}, {"message", "incomplete request body"}});
            write_all(client_fd, response.data(), response.size());
            return;
        }
        try {
            auto in = nlohmann::json::parse(body);
            WifiCredentials wifi;
            wifi.ssid = in.value("ssid", "");
            wifi.password = in.value("password", "");
            if (wifi.ssid.empty()) {
                response = http_response(400, "Bad Request", {{"code", "INVALID_BODY"}, {"message", "ssid required"}});
            } else {
                begin_provisioning(wifi);
                response = http_response(200, "OK", {{"accepted", true}});
            }
        } catch (const std::exception &e) {
            std::fprintf(stderr,
                         "[provision] invalid json body_size=%zu error=%s\n",
                         body.size(),
                         e.what());
            response = http_response(400, "Bad Request", {{"code", "INVALID_BODY"}, {"message", "invalid json"}});
        }
    } else {
        response = http_response(404, "Not Found", {{"code", "NOT_FOUND"}, {"message", "not found"}});
    }

    write_all(client_fd, response.data(), response.size());
}

void DoorbellProvisioning::begin_provisioning(const WifiCredentials &wifi) {
    if (provision_thread_.joinable()) provision_thread_.join();
    provision_thread_ = std::thread(&DoorbellProvisioning::provisioning_worker, this, wifi);
}

void DoorbellProvisioning::provisioning_worker(WifiCredentials wifi) {
    if (live_view_session_) live_view_session_->stop();
    if (mqtt_client_) mqtt_client_->stop(false);
    set_stage(Stage::ConnectingWifi);
    if (!connect_sta(wifi)) {
        set_stage(Stage::WifiFailed, "WIFI_TIMEOUT");
        start_access_point();
        return;
    }

    save_wifi(wifi);
    set_stage(Stage::ConnectingCloud);
    if (!mqtt_client_ || !mqtt_client_->run_online_probe()) {
        set_stage(Stage::CloudFailed, "CLOUD_CONNECT_FAILED");
        start_access_point();
        return;
    }

    set_stage(Stage::Online);
    if (mqtt_client_) mqtt_client_->start();
}

void DoorbellProvisioning::set_live_frame_provider(LiveWebRtcSession::FrameProvider provider) {
    if (live_view_session_) {
        live_view_session_->set_frame_provider(std::move(provider));
    }
}

void DoorbellProvisioning::set_live_audio_manager(AudioCaptureManager *manager) {
    if (live_view_session_) {
        live_view_session_->set_audio_manager(manager);
    }
}

void DoorbellProvisioning::start_led() {
    if (!configure_led_class()) {
        std::fprintf(stderr, "[led] LED class unavailable or not controllable path=%s\n", led_path_.c_str());
    }
    led_thread_ = std::thread(&DoorbellProvisioning::led_loop, this);
}

void DoorbellProvisioning::stop_led() {
    if (led_thread_.joinable()) led_thread_.join();
    set_led(false);
}

void DoorbellProvisioning::led_loop() {
    while (!stop_.load()) {
        Stage s = stage();
        if (s == Stage::Online) {
            set_led(true);
            wait_for_stop_or(std::chrono::milliseconds(500));
        } else if (s == Stage::WaitingProvision) {
            set_led(true);
            if (wait_for_stop_or(std::chrono::milliseconds(500))) break;
            set_led(false);
            wait_for_stop_or(std::chrono::milliseconds(500));
        } else if (s == Stage::ConnectingWifi || s == Stage::WifiFailed) {
            set_led(true);
            if (wait_for_stop_or(std::chrono::milliseconds(150))) break;
            set_led(false);
            wait_for_stop_or(std::chrono::milliseconds(150));
        } else if (s == Stage::ConnectingCloud || s == Stage::CloudFailed) {
            set_led(true);
            if (wait_for_stop_or(std::chrono::milliseconds(120))) break;
            set_led(false);
            if (wait_for_stop_or(std::chrono::milliseconds(120))) break;
            set_led(true);
            if (wait_for_stop_or(std::chrono::milliseconds(120))) break;
            set_led(false);
            wait_for_stop_or(std::chrono::milliseconds(1640));
        } else {
            set_led(false);
            wait_for_stop_or(std::chrono::milliseconds(500));
        }
    }
}

bool DoorbellProvisioning::configure_led_class() {
    if (led_path_.empty()) return false;

    if (!std::filesystem::exists(led_brightness_path_)) {
        return false;
    }

    if (std::filesystem::exists(led_trigger_path_)) {
        if (!write_file(led_trigger_path_, "none\n")) {
            std::fprintf(stderr, "[led] failed to disable kernel trigger: %s\n", led_trigger_path_.c_str());
            return false;
        }
    }

    return write_file(led_brightness_path_, "0\n");
}

void DoorbellProvisioning::set_led(bool on) {
    if (!led_brightness_path_.empty()) {
        write_file(led_brightness_path_, on ? "1\n" : "0\n");
    }
}

void DoorbellProvisioning::start_button() {
    if (!open_input_line(&button_gpio_, button_chip_, button_line_)) {
        std::fprintf(stderr, "[gpio] button line unavailable chip=%s line=%u\n", button_chip_.c_str(), button_line_);
    }
    button_thread_ = std::thread(&DoorbellProvisioning::button_loop, this);
}

void DoorbellProvisioning::stop_button() {
    if (button_thread_.joinable()) button_thread_.join();
    button_gpio_.close();
}

void DoorbellProvisioning::button_loop() {
    bool was_pressed = false;
    auto press_start = std::chrono::steady_clock::now();
    bool long_handled = false;

    while (!stop_.load()) {
        int value = button_gpio_.line ? gpiod_line_get_value(button_gpio_.line) : -1;
        bool pressed = value >= 0 && (button_active_low_ ? value == 0 : value == 1);
        if (pressed && !was_pressed) {
            press_start = std::chrono::steady_clock::now();
            long_handled = false;
        }
        if (pressed && !long_handled) {
            auto held = std::chrono::steady_clock::now() - press_start;
            if (held >= std::chrono::seconds(5)) {
                handle_long_press_reset();
                long_handled = true;
            }
        }
        if (!pressed && was_pressed && !long_handled) {
            std::fprintf(stdout, "[button] short press detected; ring event is not part of provisioning MVP\n");
        }
        was_pressed = pressed;
        wait_for_stop_or(std::chrono::milliseconds(100));
    }
}

void DoorbellProvisioning::handle_long_press_reset() {
    std::fprintf(stdout, "[button] long press reset: clear wifi and enter provisioning\n");
    clear_saved_wifi();
    if (live_view_session_) live_view_session_->stop();
    if (mqtt_client_) mqtt_client_->stop(true);
    set_stage(Stage::WaitingProvision);
    if (command_available("wpa_cli")) {
        const std::string disconnect_cmd =
            shell_quote(command_path("wpa_cli")) + " -i " + shell_quote(sta_iface_) + " disconnect >/dev/null 2>&1";
        if (command_available("timeout")) {
            run_command(shell_quote(command_path("timeout")) + " 3 /bin/sh -c " + shell_quote(disconnect_cmd), false);
        } else {
            std::fprintf(stderr, "[provision] timeout unavailable, skip wpa_cli disconnect during reset\n");
        }
    }
    start_access_point();
}

bool DoorbellProvisioning::open_input_line(GpioLine *out,
                                           const std::string &chip_name,
                                           unsigned int offset) {
    if (!out) return false;
    out->close();
    out->chip = gpiod_chip_open_by_name(chip_name.c_str());
    if (!out->chip) return false;
    out->line = gpiod_chip_get_line(out->chip, offset);
    if (!out->line) {
        out->close();
        return false;
    }
    if (gpiod_line_request_input(out->line, "doorbell") != 0) {
        out->close();
        return false;
    }
    return true;
}

void DoorbellProvisioning::set_stage(Stage stage, const char *error_code) {
    std::lock_guard<std::mutex> lock(state_mtx_);
    stage_ = stage;
    status_.wifi_ok = stage == Stage::ConnectingCloud || stage == Stage::Online || stage == Stage::CloudFailed;
    status_.cloud_ok = stage == Stage::Online;
    status_.error_code = error_code ? error_code : "";
    std::fprintf(stdout, "[provision] stage=%s wifi_ok=%d cloud_ok=%d error=%s\n",
                 stage_name(stage_).c_str(),
                 status_.wifi_ok ? 1 : 0,
                 status_.cloud_ok ? 1 : 0,
                 status_.error_code.empty() ? "none" : status_.error_code.c_str());
}

DoorbellProvisioning::Stage DoorbellProvisioning::stage() const {
    std::lock_guard<std::mutex> lock(state_mtx_);
    return stage_;
}

DoorbellProvisioning::ProvisionStatus DoorbellProvisioning::status_snapshot() const {
    std::lock_guard<std::mutex> lock(state_mtx_);
    return status_;
}

std::string DoorbellProvisioning::stage_name(Stage stage) const {
    switch (stage) {
    case Stage::Idle: return "idle";
    case Stage::WaitingProvision: return "waiting_provision";
    case Stage::ConnectingWifi: return "connecting_wifi";
    case Stage::ConnectingCloud: return "connecting_cloud";
    case Stage::Online: return "online";
    case Stage::WifiFailed: return "wifi_failed";
    case Stage::CloudFailed: return "cloud_failed";
    }
    return "unknown";
}

bool DoorbellProvisioning::wait_for_stop_or(std::chrono::milliseconds duration) const {
    std::unique_lock<std::mutex> lock(stop_mtx_);
    return stop_cv_.wait_for(lock, duration, [this] { return stop_.load(); });
}
