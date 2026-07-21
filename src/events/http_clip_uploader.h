#pragma once

#include <string>

class HttpClipUploader {
public:
    HttpClipUploader(std::string api_base_url, std::string device_id, std::string device_secret);

    bool upload(const std::string &clip_path,
                const std::string &clip_ref,
                const std::string &upload_id,
                const std::string &sha256,
                std::string *clip_url,
                std::string *error = nullptr) const;

private:
    std::string api_base_url_;
    std::string device_id_;
    std::string device_secret_;
};
