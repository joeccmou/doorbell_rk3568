#pragma once

#include <cstdint>
#include <string>
#include <vector>

class SnapshotService {
public:
    bool save_rgb_jpeg(const std::string &output_path,
                       const std::vector<uint8_t> &rgb,
                       uint32_t width,
                       uint32_t height,
                       std::string *error = nullptr) const;
};
