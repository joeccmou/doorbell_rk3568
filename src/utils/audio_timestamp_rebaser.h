#pragma once

#include <cstdint>

class AudioTimestampRebaser {
public:
  uint64_t rebase(uint64_t source_pts_ns, uint64_t duration_ns) {
    constexpr uint64_t kDefaultFrameDurationNs = 20ULL * 1000ULL * 1000ULL;
    const uint64_t step_ns =
        duration_ns == 0 ? kDefaultFrameDurationNs : duration_ns;
    if (!initialized_) {
      initialized_ = true;
      first_source_pts_ns_ = source_pts_ns;
      last_output_pts_ns_ = 0;
      return 0;
    }

    uint64_t output_pts_ns = source_pts_ns >= first_source_pts_ns_
                                 ? source_pts_ns - first_source_pts_ns_
                                 : 0;
    if (output_pts_ns <= last_output_pts_ns_) {
      output_pts_ns = last_output_pts_ns_ + step_ns;
    }
    last_output_pts_ns_ = output_pts_ns;
    return output_pts_ns;
  }

  void reset() {
    initialized_ = false;
    first_source_pts_ns_ = 0;
    last_output_pts_ns_ = 0;
  }

private:
  bool initialized_ = false;
  uint64_t first_source_pts_ns_ = 0;
  uint64_t last_output_pts_ns_ = 0;
};