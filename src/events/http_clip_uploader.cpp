#include "events/http_clip_uploader.h"

#include "events/event_paths.h"

#include <curl/curl.h>
#include <nlohmann/json.hpp>

#include <cstdio>
#include <filesystem>
#include <mutex>
#include <utility>

namespace {
size_t append_clip_response(char *data, size_t size, size_t count, void *user_data) {
    auto *response = static_cast<std::string *>(user_data);
    response->append(data, size * count);
    return size * count;
}
}

HttpClipUploader::HttpClipUploader(std::string api_base_url,
                                   std::string device_id,
                                   std::string device_secret)
    : api_base_url_(std::move(api_base_url)),
      device_id_(std::move(device_id)),
      device_secret_(std::move(device_secret)) {
    while (!api_base_url_.empty() && api_base_url_.back() == '/') api_base_url_.pop_back();
}

bool HttpClipUploader::upload(const std::string &clip_path,
                              const std::string &clip_ref,
                              const std::string &upload_id,
                              const std::string &sha256,
                              std::string *clip_url,
                              std::string *error) const {
    if (api_base_url_.rfind("https://", 0) != 0 ||
        !is_canonical_clip_ref(clip_ref) ||
        sha256.size() != 64) {
        if (error) *error = "clip upload requires HTTPS and canonical clip_ref";
        return false;
    }
    std::FILE *input = std::fopen(clip_path.c_str(), "rb");
    if (!input) {
        if (error) *error = "clip file is not readable";
        return false;
    }
    std::error_code ec;
    const auto size = std::filesystem::file_size(clip_path, ec);
    if (ec || size == 0) {
        std::fclose(input);
        if (error) *error = "clip file is empty";
        return false;
    }
    static std::once_flag curl_init_once;
    static CURLcode curl_init_result = CURLE_FAILED_INIT;
    std::call_once(curl_init_once, [] {
        curl_init_result = static_cast<CURLcode>(curl_global_init(CURL_GLOBAL_DEFAULT));
    });
    CURL *curl = curl_init_result == CURLE_OK ? curl_easy_init() : nullptr;
    if (!curl) {
        std::fclose(input);
        if (error) *error = "initialize HTTP client failed";
        return false;
    }
    char *escaped_device = curl_easy_escape(
        curl, device_id_.c_str(), static_cast<int>(device_id_.size()));
    const std::string url =
        api_base_url_ + "/devices/" + (escaped_device ? escaped_device : "") + "/clips";
    if (escaped_device) curl_free(escaped_device);
    struct curl_slist *headers = nullptr;
    headers = curl_slist_append(headers, "Content-Type: video/mp4");
    headers = curl_slist_append(headers, "Accept: application/json");
    headers = curl_slist_append(headers, "Expect: 100-continue");
    const std::string path_header = "X-Doorbell-Clip-Path: " + clip_ref;
    const std::string upload_header = "X-Doorbell-Upload-Id: " + upload_id;
    const std::string sha256_header = "X-Doorbell-Clip-SHA256: " + sha256;
    headers = curl_slist_append(headers, path_header.c_str());
    headers = curl_slist_append(headers, upload_header.c_str());
    headers = curl_slist_append(headers, sha256_header.c_str());
    std::string response;
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_USERNAME, device_id_.c_str());
    curl_easy_setopt(curl, CURLOPT_PASSWORD, device_secret_.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPAUTH, CURLAUTH_BASIC);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_POST, 1L);
    curl_easy_setopt(curl, CURLOPT_READDATA, input);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE_LARGE, static_cast<curl_off_t>(size));
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, 5000L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, 120000L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, append_clip_response);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
    const CURLcode result = curl_easy_perform(curl);
    long status = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    std::fclose(input);
    if (result != CURLE_OK || status < 200 || status >= 300) {
        if (error) {
            *error = result != CURLE_OK ? curl_easy_strerror(result) : "clip server rejected upload";
        }
        return false;
    }
    try {
        const auto body = nlohmann::json::parse(response);
        const std::string value = body.value("clip_url", "");
        if (value.rfind("https://", 0) != 0) {
            if (error) *error = "clip response URL is invalid";
            return false;
        }
        if (clip_url) *clip_url = value;
        return true;
    } catch (const std::exception &) {
        if (error) *error = "clip response JSON is invalid";
        return false;
    }
}
