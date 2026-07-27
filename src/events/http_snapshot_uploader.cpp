#include "events/http_snapshot_uploader.h"

#include "events/event_paths.h"

#include <curl/curl.h>
#include <nlohmann/json.hpp>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <mutex>
#include <utility>

namespace {
size_t append_response(char *data, size_t size, size_t count, void *user_data) {
    auto *response = static_cast<std::string *>(user_data);
    response->append(data, size * count);
    return size * count;
}

std::string summarize_response(std::string response) {
    for (char &ch : response) {
        if (ch == '\r' || ch == '\n' || ch == '\t') ch = ' ';
    }
    constexpr size_t kMaxResponseLength = 512;
    if (response.size() > kMaxResponseLength) {
        response.resize(kMaxResponseLength);
        response += "...";
    }
    return response;
}
}

HttpSnapshotUploader::HttpSnapshotUploader(std::string api_base_url,
                                           std::string device_id,
                                           std::string device_secret)
    : api_base_url_(std::move(api_base_url)),
      device_id_(std::move(device_id)),
      device_secret_(std::move(device_secret)) {
    while (!api_base_url_.empty() && api_base_url_.back() == '/') api_base_url_.pop_back();
}

bool HttpSnapshotUploader::upload(const std::string &jpeg_path,
                                  const std::string &device_relative_path,
                                  std::string *snapshot_url,
                                  std::string *error,
                                  long timeout_ms) const {
    if (api_base_url_.rfind("https://", 0) != 0 || !is_canonical_snapshot_path(device_relative_path)) {
        if (error) *error = "snapshot upload requires HTTPS";
        return false;
    }
	static std::once_flag curl_init_once;
	static CURLcode curl_init_result = CURLE_FAILED_INIT;
	std::call_once(curl_init_once, [] { curl_init_result = static_cast<CURLcode>(curl_global_init(CURL_GLOBAL_DEFAULT)); });
	if (curl_init_result != CURLE_OK) {
		if (error) *error = "initialize curl runtime failed";
		return false;
	}
    std::ifstream input(jpeg_path, std::ios::binary);
    if (!input) {
        if (error) *error = "snapshot file is not readable";
        return false;
    }
    std::ostringstream content_stream;
    content_stream << input.rdbuf();
    const std::string content = content_stream.str();
    if (content.empty()) {
        if (error) *error = "snapshot file is empty";
        return false;
    }

    CURL *curl = curl_easy_init();
    if (!curl) {
        if (error) *error = "initialize HTTP client failed";
        return false;
    }
    char *escaped_device = curl_easy_escape(curl, device_id_.c_str(), static_cast<int>(device_id_.size()));
	if (!escaped_device) {
		curl_easy_cleanup(curl);
		if (error) *error = "encode device id failed";
		return false;
	}
    const std::string url = api_base_url_ + "/devices/" + (escaped_device ? escaped_device : "") + "/snapshots";
    if (escaped_device) curl_free(escaped_device);
    std::string response;
    struct curl_slist *headers = nullptr;
    headers = curl_slist_append(headers, "Content-Type: image/jpeg");
    headers = curl_slist_append(headers, "Accept: application/json");
    const std::string snapshot_path_header = "X-Doorbell-Snapshot-Path: " + device_relative_path;
    headers = curl_slist_append(headers, snapshot_path_header.c_str());
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_USERNAME, device_id_.c_str());
    curl_easy_setopt(curl, CURLOPT_PASSWORD, device_secret_.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPAUTH, CURLAUTH_BASIC);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_POST, 1L);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, content.data());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE_LARGE, static_cast<curl_off_t>(content.size()));
    const long bounded_timeout_ms = timeout_ms > 0 ? timeout_ms : 20000L;
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, bounded_timeout_ms);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, bounded_timeout_ms);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 0L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, append_response);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
    const CURLcode result = curl_easy_perform(curl);
    long status = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    if (result != CURLE_OK || status < 200 || status >= 300) {
        if (error) {
            std::ostringstream message;
            if (result != CURLE_OK) {
                message << "snapshot upload transport failed: "
                        << curl_easy_strerror(result);
            } else {
                message << "snapshot server rejected upload: http_status="
                        << status;
            }
            const std::string response_summary = summarize_response(response);
            if (!response_summary.empty()) {
                message << " response=" << response_summary;
            }
            *error = message.str();
        }
        return false;
    }
    try {
        const auto body = nlohmann::json::parse(response);
        const std::string value = body.value("snapshot_url", "");
        const std::string expected_path = "/media/snapshots/" + device_id_ + "/";
        const auto path_position = value.find(expected_path);
        if (value.rfind("https://", 0) != 0 || path_position == std::string::npos) {
            if (error) *error = "snapshot response URL is invalid";
            return false;
        }
        if (snapshot_url) *snapshot_url = value;
        return true;
    } catch (const std::exception &) {
        if (error) *error = "snapshot response JSON is invalid";
        return false;
    }
}
