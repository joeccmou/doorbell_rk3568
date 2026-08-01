#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <condition_variable>
#include <deque>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <gst/gst.h>

#include "utils/audio_signal_metrics.h"
#include "utils/audio_timestamp_rebaser.h"

struct AudioFrame {
  const uint8_t *data = nullptr;
  size_t size = 0;
  uint64_t pts_ns = 0;
  uint64_t duration_ns = 0;
  uint64_t seq = 0;
};

class AudioFrameDispatcher {
public:
  using Consumer = std::function<void(const AudioFrame &frame)>;

  size_t register_consumer(Consumer consumer);
  void unregister_consumer(size_t consumer_id);
  void dispatch(const AudioFrame &frame);
  size_t consumer_count() const;

private:
  mutable std::mutex mtx_;
  std::map<size_t, Consumer> consumers_;
  size_t next_id_ = 1;
};

class AudioCaptureManager {
public:
  AudioCaptureManager() = default;
  ~AudioCaptureManager();

  AudioCaptureManager(const AudioCaptureManager &) = delete;
  AudioCaptureManager &operator=(const AudioCaptureManager &) = delete;

  bool start(const std::string &device = "plughw:0,0",
             std::string *error_message = nullptr);
  void stop();
  bool running() const { return pipeline_ != nullptr; }

  size_t register_consumer(AudioFrameDispatcher::Consumer consumer);
  void unregister_consumer(size_t consumer_id);
  size_t consumer_count() const { return dispatcher_.consumer_count(); }
  AudioFrameDispatcher *dispatcher() { return &dispatcher_; }
  bool push_remote_playback_frame(const AudioFrame &frame);
  bool push_chime_playback_frame(const AudioFrame &frame);
  void set_chime_active(bool active);

private:
  enum class PlaybackSource {
    kRemote,
    kChime,
  };

  bool enqueue_playback_frame(const AudioFrame &frame, PlaybackSource source);
  bool push_mixed_playback_buffer(const std::vector<int16_t> &samples);
  void playback_loop();
  void pull_loop();

  AudioFrameDispatcher dispatcher_;
  GstElement *pipeline_ = nullptr;
  GstElement *appsink_ = nullptr;
  GstElement *playback_appsrc_ = nullptr;
  GstPad *playback_sink_pad_ = nullptr;
  gulong playback_sink_probe_id_ = 0;
  std::mutex playback_mtx_;
  std::condition_variable playback_cv_;
  std::deque<int16_t> remote_playback_samples_;
  std::deque<int16_t> chime_playback_samples_;
  std::thread playback_thread_;
  std::thread pull_thread_;
  std::atomic<bool> stop_requested_{false};
  std::atomic<bool> chime_active_{false};
  PcmS16LevelMeter remote_playback_level_meter_;
  PcmS16LevelMeter chime_playback_level_meter_;
  PcmS16LevelMeter mixed_playback_level_meter_;
  PcmS16LevelMeter playback_sink_level_meter_;
  PcmS16LevelMeter capture_level_meter_;
  std::atomic<uint64_t> latest_playback_pts_ns_{0};
  uint64_t last_metrics_log_ns_ = 0;
  uint64_t seq_ = 0;
};
