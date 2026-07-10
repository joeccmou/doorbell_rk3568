#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <mutex>

struct AudioLevelSnapshot {
  uint64_t sample_count = 0;
  double rms = 0.0;
  double rms_dbfs = -120.0;
  uint32_t peak = 0;
};

enum class AudioPlaybackPathState {
  kIdle,
  kBeforeSink,
  kAtSink,
};

inline AudioPlaybackPathState
audio_playback_path_state(const AudioLevelSnapshot &input,
                          const AudioLevelSnapshot &sink) {
  if (sink.sample_count > 0)
    return AudioPlaybackPathState::kAtSink;
  if (input.sample_count > 0)
    return AudioPlaybackPathState::kBeforeSink;
  return AudioPlaybackPathState::kIdle;
}

inline const char *
audio_playback_path_state_name(AudioPlaybackPathState state) {
  switch (state) {
  case AudioPlaybackPathState::kAtSink:
    return "at_sink";
  case AudioPlaybackPathState::kBeforeSink:
    return "before_sink";
  case AudioPlaybackPathState::kIdle:
    return "idle";
  }
  return "unknown";
}

class PcmS16LevelMeter {
public:
  void add(const uint8_t *data, size_t size) {
    if (!data || size < 2)
      return;

    std::lock_guard<std::mutex> lock(mtx_);
    const size_t sample_count = size / 2;
    for (size_t i = 0; i < sample_count; ++i) {
      const uint16_t raw =
          static_cast<uint16_t>(data[i * 2]) |
          (static_cast<uint16_t>(data[i * 2 + 1]) << 8);
      const int32_t sample = static_cast<int16_t>(raw);
      const int64_t wide_sample = sample;
      const uint32_t magnitude =
          sample < 0 ? static_cast<uint32_t>(-sample)
                     : static_cast<uint32_t>(sample);
      sum_squares_ += static_cast<uint64_t>(wide_sample * wide_sample);
      peak_ = std::max(peak_, magnitude);
    }
    sample_count_ += sample_count;
  }

  AudioLevelSnapshot snapshot_and_reset() {
    std::lock_guard<std::mutex> lock(mtx_);
    AudioLevelSnapshot snapshot;
    snapshot.sample_count = sample_count_;
    snapshot.peak = peak_;
    if (sample_count_ > 0) {
      snapshot.rms =
          std::sqrt(static_cast<double>(sum_squares_) / sample_count_);
      if (snapshot.rms > 0.0) {
        snapshot.rms_dbfs =
            std::max(-120.0, 20.0 * std::log10(snapshot.rms / 32768.0));
      }
    }
    sample_count_ = 0;
    sum_squares_ = 0;
    peak_ = 0;
    return snapshot;
  }

private:
  std::mutex mtx_;
  uint64_t sample_count_ = 0;
  uint64_t sum_squares_ = 0;
  uint32_t peak_ = 0;
};

inline int64_t audio_timestamp_delta_ms(uint64_t capture_pts_ns,
                                        uint64_t playback_pts_ns) {
  if (capture_pts_ns == 0 || playback_pts_ns == 0)
    return 0;
  if (capture_pts_ns >= playback_pts_ns) {
    return static_cast<int64_t>((capture_pts_ns - playback_pts_ns) / 1'000'000);
  }
  return -static_cast<int64_t>(
      (playback_pts_ns - capture_pts_ns) / 1'000'000);
}
