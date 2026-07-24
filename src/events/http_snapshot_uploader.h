#pragma once

#include <string>

class HttpSnapshotUploader {
public:
    HttpSnapshotUploader(std::string api_base_url, std::string device_id, std::string device_secret);

    bool upload(const std::string &jpeg_path,
                const std::string &device_relative_path,
                std::string *snapshot_url,
                std::string *error = nullptr,
                long timeout_ms = 20000) const;

private:
    std::string api_base_url_;
    std::string device_id_;
    std::string device_secret_;
};
