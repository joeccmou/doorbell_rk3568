#include "utils/audio_capture_manager.h"
#include "utils/audio_pipeline_config.h"

#include <gst/app/gstappsink.h>
#include <gst/app/gstappsrc.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <utility>
#include <vector>

namespace {

std::once_flag g_audio_gst_init_once;

constexpr uint64_t kDefaultAudioFrameNs = 20ULL * 1000ULL * 1000ULL;
constexpr uint64_t kMetricsLogIntervalNs = 1000ULL * 1000ULL * 1000ULL;
constexpr size_t kPlaybackFrameSamples = 960;
constexpr size_t kRemotePlaybackQueueMaxSamples = 9600;
constexpr size_t kChimePlaybackQueueMaxSamples = 48000;
constexpr double kPlaybackGain = 0.3548;
constexpr double kRemotePlaybackGainDuringChime = 0.1778;

int16_t decode_s16le(const uint8_t *data) {
  const uint16_t raw = static_cast<uint16_t>(data[0]) |
                       (static_cast<uint16_t>(data[1]) << 8);
  return static_cast<int16_t>(raw);
}

int16_t mix_pcm_sample(int16_t remote, int16_t chime, bool duck_remote) {
  const double remote_gain =
      duck_remote ? kRemotePlaybackGainDuringChime : kPlaybackGain;
  const int32_t mixed =
      static_cast<int32_t>(std::lround(static_cast<double>(remote) *
                                      remote_gain)) +
      static_cast<int32_t>(
          std::lround(static_cast<double>(chime) * kPlaybackGain));
  return static_cast<int16_t>(
      std::clamp(mixed,
                 static_cast<int32_t>(std::numeric_limits<int16_t>::min()),
                 static_cast<int32_t>(std::numeric_limits<int16_t>::max())));
}

uint64_t monotonic_time_ns() {
  return static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(
          std::chrono::steady_clock::now().time_since_epoch())
          .count());
}

uint64_t pipeline_running_time_ns(GstElement *pipeline) {
  if (!pipeline)
    return 0;
  GstClock *clock = gst_element_get_clock(pipeline);
  if (!clock)
    return 0;
  const GstClockTime now = gst_clock_get_time(clock);
  const GstClockTime base = gst_element_get_base_time(pipeline);
  gst_object_unref(clock);
  if (!GST_CLOCK_TIME_IS_VALID(now) || !GST_CLOCK_TIME_IS_VALID(base) ||
      now < base) {
    return 0;
  }
  return static_cast<uint64_t>(now - base);
}

void configure_capture_channel_mix(GstElement *channel_select) {
  GValue matrix = G_VALUE_INIT;
  GValue output_row = G_VALUE_INIT;
  GValue coefficient = G_VALUE_INIT;

  g_value_init(&matrix, GST_TYPE_ARRAY);
  g_value_init(&output_row, GST_TYPE_ARRAY);
  g_value_init(&coefficient, G_TYPE_DOUBLE);
  for (const double value : audio_capture_channel_mix_coefficients()) {
    g_value_set_double(&coefficient, value);
    gst_value_array_append_value(&output_row, &coefficient);
  }
  gst_value_array_append_value(&matrix, &output_row);
  g_object_set_property(G_OBJECT(channel_select), "mix-matrix", &matrix);

  g_value_unset(&coefficient);
  g_value_unset(&output_row);
  g_value_unset(&matrix);
}

} // namespace

size_t AudioFrameDispatcher::register_consumer(Consumer consumer) {
  if (!consumer)
    return 0;
  std::lock_guard<std::mutex> lock(mtx_);
  const size_t id = next_id_++;
  consumers_[id] = std::move(consumer);
  return id;
}

void AudioFrameDispatcher::unregister_consumer(size_t consumer_id) {
  if (consumer_id == 0)
    return;
  std::lock_guard<std::mutex> lock(mtx_);
  consumers_.erase(consumer_id);
}

void AudioFrameDispatcher::dispatch(const AudioFrame &frame) {
  std::vector<Consumer> consumers;
  {
    std::lock_guard<std::mutex> lock(mtx_);
    consumers.reserve(consumers_.size());
    for (const auto &entry : consumers_) {
      consumers.push_back(entry.second);
    }
  }
  for (const auto &consumer : consumers) {
    consumer(frame);
  }
}

size_t AudioFrameDispatcher::consumer_count() const {
  std::lock_guard<std::mutex> lock(mtx_);
  return consumers_.size();
}

AudioCaptureManager::~AudioCaptureManager() { stop(); }

bool AudioCaptureManager::start(const std::string &device,
                                std::string *error_message) {
  if (pipeline_)
    return true;

  std::call_once(g_audio_gst_init_once, []() { gst_init(nullptr, nullptr); });

  // 远端语音和门铃声先在 C++ 中按 20 ms 帧混音，再经唯一的 appsrc
  // 送入 AEC 和 ALSA。没有真实播放数据时不持续写入静音帧。
  gchar *pipeline_desc = g_strdup_printf(
      "appsrc name=playback_src is-live=true block=false format=time "
      "do-timestamp=true "
      "caps=%s "
      "! queue leaky=downstream max-size-buffers=16 ! audioconvert ! "
      "audioresample ! "
      "%s ! "
      "volume name=live_playback_gain volume=1.0 ! "
      "webrtcechoprobe name=echo_probe ! "
      "audioconvert ! audioresample ! "
      "%s ! "
      "alsasink name=playback_sink device=%s sync=true async=false "
      "alsasrc device=%s do-timestamp=true ! "
      "%s ! "
      "queue leaky=downstream max-size-buffers=8 ! "
      "audioconvert name=capture_channel_select ! "
      "audioresample ! "
      "%s ! "
      "webrtcdsp name=capture_dsp probe=echo_probe echo-cancel=true "
      "echo-suppression-level=high delay-agnostic=true extended-filter=true "
      "noise-suppression=true noise-suppression-level=high "
      "gain-control=true compression-gain-db=3 target-level-dbfs=6 "
      "limiter=true "
      "high-pass-filter=true ! "
      "queue leaky=downstream max-size-buffers=8 ! "
      "appsink name=audio_sink emit-signals=false sync=false max-buffers=8 "
      "drop=true",
      audio_webrtc_raw_caps(), audio_webrtc_raw_caps(),
      audio_hardware_raw_caps(), device.c_str(), device.c_str(),
      audio_hardware_raw_caps(), audio_webrtc_raw_caps());

  GError *error = nullptr;
  GstElement *pipeline = gst_parse_launch(pipeline_desc, &error);
  g_free(pipeline_desc);
  if (!pipeline) {
    if (error_message)
      *error_message =
          error ? error->message : "create audio capture pipeline failed";
    if (error)
      g_error_free(error);
    return false;
  }
  if (error) {
    std::fprintf(stderr, "[audio] pipeline warning: %s\n", error->message);
    g_error_free(error);
  }

  GstElement *appsink = gst_bin_get_by_name(GST_BIN(pipeline), "audio_sink");
  GstElement *playback_appsrc =
      gst_bin_get_by_name(GST_BIN(pipeline), "playback_src");
  GstElement *playback_sink =
      gst_bin_get_by_name(GST_BIN(pipeline), "playback_sink");
  GstElement *capture_channel_select =
      gst_bin_get_by_name(GST_BIN(pipeline), "capture_channel_select");
  GstPad *playback_sink_pad =
      playback_sink ? gst_element_get_static_pad(playback_sink, "sink")
                    : nullptr;
  if (!appsink || !playback_appsrc || !playback_sink_pad ||
      !capture_channel_select) {
    if (playback_sink_pad)
      gst_object_unref(playback_sink_pad);
    if (playback_sink)
      gst_object_unref(playback_sink);
    if (appsink)
      gst_object_unref(appsink);
    if (playback_appsrc)
      gst_object_unref(playback_appsrc);
    if (capture_channel_select)
      gst_object_unref(capture_channel_select);
    gst_object_unref(pipeline);
    if (error_message)
      *error_message = "audio manager app elements not found";
    return false;
  }
  configure_capture_channel_mix(capture_channel_select);
  gst_object_unref(capture_channel_select);
  gst_object_unref(playback_sink);
  gst_app_src_set_stream_type(GST_APP_SRC(playback_appsrc),
                              GST_APP_STREAM_TYPE_STREAM);
  g_object_set(G_OBJECT(playback_appsrc), "block", FALSE, nullptr);

  remote_playback_level_meter_.snapshot_and_reset();
  chime_playback_level_meter_.snapshot_and_reset();
  mixed_playback_level_meter_.snapshot_and_reset();
  playback_sink_level_meter_.snapshot_and_reset();
  capture_level_meter_.snapshot_and_reset();
  const gulong playback_sink_probe_id = gst_pad_add_probe(
      playback_sink_pad, GST_PAD_PROBE_TYPE_BUFFER,
      [](GstPad *, GstPadProbeInfo *info, gpointer user_data) {
        auto *manager = static_cast<AudioCaptureManager *>(user_data);
        GstBuffer *buffer = GST_PAD_PROBE_INFO_BUFFER(info);
        GstMapInfo map;
        if (buffer && gst_buffer_map(buffer, &map, GST_MAP_READ)) {
          manager->playback_sink_level_meter_.add(map.data, map.size);
          gst_buffer_unmap(buffer, &map);
        }
        return GST_PAD_PROBE_OK;
      },
      this, nullptr);
  if (playback_sink_probe_id == 0) {
    gst_object_unref(playback_sink_pad);
    gst_object_unref(playback_appsrc);
    gst_object_unref(appsink);
    gst_object_unref(pipeline);
    if (error_message)
      *error_message = "install playback sink diagnostic probe failed";
    return false;
  }

  GstStateChangeReturn ret = gst_element_set_state(pipeline, GST_STATE_PLAYING);
  if (ret == GST_STATE_CHANGE_FAILURE) {
    gst_pad_remove_probe(playback_sink_pad, playback_sink_probe_id);
    gst_object_unref(playback_sink_pad);
    gst_object_unref(playback_appsrc);
    gst_object_unref(appsink);
    gst_object_unref(pipeline);
    if (error_message)
      *error_message = "set audio capture pipeline PLAYING failed";
    return false;
  }

  pipeline_ = pipeline;
  appsink_ = appsink;
  playback_appsrc_ = playback_appsrc;
  playback_sink_pad_ = playback_sink_pad;
  playback_sink_probe_id_ = playback_sink_probe_id;
  stop_requested_.store(false);
  chime_active_.store(false);
  remote_playback_samples_.clear();
  chime_playback_samples_.clear();
  latest_playback_pts_ns_.store(0);
  last_metrics_log_ns_ = monotonic_time_ns();
  seq_ = 0;
  playback_thread_ = std::thread(&AudioCaptureManager::playback_loop, this);
  pull_thread_ = std::thread(&AudioCaptureManager::pull_loop, this);
  std::fprintf(stdout,
               "[audio] capture started device=%s hardware_rate=%d "
               "hardware_channels=%d webrtc_rate=%d webrtc_channels=%d "
               "capture_channel=0 playback_mixer=cpp_pcm "
               "playback_clock=active_source\n",
               device.c_str(), kAudioHardwareRate, kAudioHardwareChannels,
               kAudioWebRtcRate, kAudioWebRtcChannels);
  return true;
}

void AudioCaptureManager::stop() {
  stop_requested_.store(true);
  playback_cv_.notify_all();
  if (playback_thread_.joinable()) {
    playback_thread_.join();
  }
  if (pull_thread_.joinable()) {
    pull_thread_.join();
  }

  GstElement *appsink = appsink_;
  GstElement *pipeline = pipeline_;
  GstPad *playback_sink_pad = playback_sink_pad_;
  const gulong playback_sink_probe_id = playback_sink_probe_id_;
  GstElement *playback_appsrc = nullptr;
  {
    std::lock_guard<std::mutex> lock(playback_mtx_);
    playback_appsrc = playback_appsrc_;
    playback_appsrc_ = nullptr;
    remote_playback_samples_.clear();
    chime_playback_samples_.clear();
  }
  appsink_ = nullptr;
  pipeline_ = nullptr;
  playback_sink_pad_ = nullptr;
  playback_sink_probe_id_ = 0;

  if (playback_appsrc)
    gst_app_src_end_of_stream(GST_APP_SRC(playback_appsrc));
  if (playback_sink_pad && playback_sink_probe_id != 0)
    gst_pad_remove_probe(playback_sink_pad, playback_sink_probe_id);
  if (pipeline) {
    gst_element_set_state(pipeline, GST_STATE_NULL);
  }
  if (playback_sink_pad)
    gst_object_unref(playback_sink_pad);
  if (playback_appsrc)
    gst_object_unref(playback_appsrc);
  if (appsink)
    gst_object_unref(appsink);
  if (pipeline)
    gst_object_unref(pipeline);
}

size_t AudioCaptureManager::register_consumer(
    AudioFrameDispatcher::Consumer consumer) {
  return dispatcher_.register_consumer(std::move(consumer));
}

void AudioCaptureManager::unregister_consumer(size_t consumer_id) {
  dispatcher_.unregister_consumer(consumer_id);
}

bool AudioCaptureManager::push_remote_playback_frame(const AudioFrame &frame) {
  return enqueue_playback_frame(frame, PlaybackSource::kRemote);
}

bool AudioCaptureManager::push_chime_playback_frame(const AudioFrame &frame) {
  return enqueue_playback_frame(frame, PlaybackSource::kChime);
}

void AudioCaptureManager::set_chime_active(bool active) {
  {
    std::lock_guard<std::mutex> lock(playback_mtx_);
    if (!playback_appsrc_)
      return;
    const bool was_active = chime_active_.exchange(active);
    if (active && !was_active) {
      // 新一轮按铃从头播放，避免上一次被中断的尾音残留到队列中。
      chime_playback_samples_.clear();
    }
  }
  playback_cv_.notify_all();
  const double remote_gain =
      active ? kRemotePlaybackGainDuringChime : kPlaybackGain;
  std::fprintf(stdout,
               "[audio] chime mix active=%d remote_gain=%.4f\n",
               active ? 1 : 0,
               remote_gain);
}

bool AudioCaptureManager::enqueue_playback_frame(const AudioFrame &frame,
                                                 PlaybackSource source) {
  if (!frame.data || frame.size < sizeof(int16_t))
    return false;

  const size_t sample_count = frame.size / sizeof(int16_t);
  std::vector<int16_t> samples(sample_count);
  for (size_t i = 0; i < sample_count; ++i) {
    samples[i] = decode_s16le(frame.data + i * sizeof(int16_t));
  }

  size_t dropped_samples = 0;
  {
    std::lock_guard<std::mutex> lock(playback_mtx_);
    if (stop_requested_.load() || !playback_appsrc_)
      return false;

    std::deque<int16_t> &queue =
        source == PlaybackSource::kRemote ? remote_playback_samples_
                                          : chime_playback_samples_;
    const size_t max_samples = source == PlaybackSource::kRemote
                                   ? kRemotePlaybackQueueMaxSamples
                                   : kChimePlaybackQueueMaxSamples;
    const size_t kept_samples = std::min(samples.size(), max_samples);
    const size_t source_offset = samples.size() - kept_samples;
    const size_t required_size = queue.size() + kept_samples;
    if (required_size > max_samples) {
      dropped_samples = required_size - max_samples;
      for (size_t i = 0; i < dropped_samples; ++i) {
        queue.pop_front();
      }
    }
    queue.insert(queue.end(), samples.begin() + source_offset, samples.end());
  }

  if (source == PlaybackSource::kRemote) {
    remote_playback_level_meter_.add(frame.data,
                                     sample_count * sizeof(int16_t));
  } else {
    chime_playback_level_meter_.add(frame.data,
                                    sample_count * sizeof(int16_t));
  }
  if (dropped_samples > 0) {
    std::fprintf(stderr,
                 "[audio] playback queue overflow source=%s "
                 "dropped_samples=%llu\n",
                 source == PlaybackSource::kRemote ? "remote" : "chime",
                 static_cast<unsigned long long>(dropped_samples));
  }
  playback_cv_.notify_one();
  return true;
}

bool AudioCaptureManager::push_mixed_playback_buffer(
    const std::vector<int16_t> &samples) {
  if (samples.empty() || !playback_appsrc_)
    return false;

  const size_t byte_count = samples.size() * sizeof(int16_t);
  GstBuffer *buffer = gst_buffer_new_allocate(nullptr, byte_count, nullptr);
  if (!buffer)
    return false;

  GstMapInfo map;
  if (!gst_buffer_map(buffer, &map, GST_MAP_WRITE)) {
    gst_buffer_unref(buffer);
    return false;
  }
  std::memcpy(map.data, samples.data(), byte_count);
  gst_buffer_unmap(buffer, &map);

  // 由 playback appsrc 按音频管理器当前 running-time 打时间戳，供 AEC
  // 对齐参考信号。
  GST_BUFFER_PTS(buffer) = GST_CLOCK_TIME_NONE;
  GST_BUFFER_DTS(buffer) = GST_CLOCK_TIME_NONE;
  GST_BUFFER_DURATION(buffer) = static_cast<GstClockTime>(
      samples.size() * GST_SECOND / kAudioWebRtcRate);

  const uint64_t playback_pts_ns = pipeline_running_time_ns(pipeline_);
  const GstFlowReturn flow_ret =
      gst_app_src_push_buffer(GST_APP_SRC(playback_appsrc_), buffer);
  if (flow_ret == GST_FLOW_OK) {
    mixed_playback_level_meter_.add(
        reinterpret_cast<const uint8_t *>(samples.data()), byte_count);
    latest_playback_pts_ns_.store(playback_pts_ns);
  }
  if (flow_ret != GST_FLOW_OK && flow_ret != GST_FLOW_FLUSHING) {
    std::fprintf(stderr, "[audio] playback appsrc push failed flow=%d\n",
                 flow_ret);
  }
  return flow_ret == GST_FLOW_OK;
}

void AudioCaptureManager::playback_loop() {
  using Clock = std::chrono::steady_clock;
  auto next_push_time = Clock::now();
  bool playback_clock_active = false;

  while (!stop_requested_.load()) {
    std::vector<int16_t> mixed_samples;
    {
      std::unique_lock<std::mutex> lock(playback_mtx_);
      if (remote_playback_samples_.empty() &&
          chime_playback_samples_.empty()) {
        playback_clock_active = false;
        playback_cv_.wait(lock, [this] {
          return stop_requested_.load() ||
                 !remote_playback_samples_.empty() ||
                 !chime_playback_samples_.empty();
        });
      }
      if (stop_requested_.load())
        break;

      if (!playback_clock_active) {
        next_push_time = Clock::now();
        playback_clock_active = true;
      }
      const size_t remote_count =
          std::min(kPlaybackFrameSamples, remote_playback_samples_.size());
      const size_t chime_count =
          std::min(kPlaybackFrameSamples, chime_playback_samples_.size());
      const size_t mixed_count = std::max(remote_count, chime_count);
      const bool duck_remote = chime_active_.load() || chime_count > 0 ||
                               !chime_playback_samples_.empty();
      mixed_samples.reserve(mixed_count);
      for (size_t i = 0; i < mixed_count; ++i) {
        int16_t remote = 0;
        int16_t chime = 0;
        if (i < remote_count) {
          remote = remote_playback_samples_.front();
          remote_playback_samples_.pop_front();
        }
        if (i < chime_count) {
          chime = chime_playback_samples_.front();
          chime_playback_samples_.pop_front();
        }
        mixed_samples.push_back(mix_pcm_sample(remote, chime, duck_remote));
      }
    }

    if (mixed_samples.empty())
      continue;
    if (!push_mixed_playback_buffer(mixed_samples) &&
        stop_requested_.load()) {
      break;
    }

    const auto frame_duration = std::chrono::nanoseconds(
        mixed_samples.size() * 1000000000ULL / kAudioWebRtcRate);
    next_push_time += frame_duration;
    std::unique_lock<std::mutex> lock(playback_mtx_);
    playback_cv_.wait_until(lock, next_push_time,
                            [this] { return stop_requested_.load(); });
  }
}

void AudioCaptureManager::pull_loop() {
  while (!stop_requested_.load()) {
    GstSample *sample =
        gst_app_sink_try_pull_sample(GST_APP_SINK(appsink_), 100 * GST_MSECOND);
    if (!sample)
      continue;

    GstBuffer *buffer = gst_sample_get_buffer(sample);
    GstMapInfo map;
    if (buffer && gst_buffer_map(buffer, &map, GST_MAP_READ)) {
      AudioFrame frame;
      frame.data = map.data;
      frame.size = map.size;
      frame.pts_ns = GST_CLOCK_TIME_IS_VALID(GST_BUFFER_PTS(buffer))
                         ? GST_BUFFER_PTS(buffer)
                         : 0;
      frame.duration_ns = GST_CLOCK_TIME_IS_VALID(GST_BUFFER_DURATION(buffer))
                              ? GST_BUFFER_DURATION(buffer)
                              : kDefaultAudioFrameNs;
      frame.seq = ++seq_;
      capture_level_meter_.add(frame.data, frame.size);

      const uint64_t now_ns = monotonic_time_ns();
      if (now_ns - last_metrics_log_ns_ >= kMetricsLogIntervalNs) {
        const AudioLevelSnapshot remote_playback =
            remote_playback_level_meter_.snapshot_and_reset();
        const AudioLevelSnapshot chime_playback =
            chime_playback_level_meter_.snapshot_and_reset();
        const AudioLevelSnapshot playback =
            mixed_playback_level_meter_.snapshot_and_reset();
        const AudioLevelSnapshot playback_sink =
            playback_sink_level_meter_.snapshot_and_reset();
        const AudioLevelSnapshot capture =
            capture_level_meter_.snapshot_and_reset();
        last_metrics_log_ns_ = now_ns;
        if (remote_playback.peak > 0 || chime_playback.peak > 0 ||
            playback.peak > 0 || playback_sink.peak > 0) {
          const AudioPlaybackPathState playback_path =
              audio_playback_path_state(playback, playback_sink);
          const uint64_t playback_pts_ns = latest_playback_pts_ns_.load();
          const int64_t pts_delta_ms =
              audio_timestamp_delta_ms(frame.pts_ns, playback_pts_ns);
          std::fprintf(
              stderr,
              "[audio] aec metrics playback_path=%s "
              "playback_samples=%llu playback_rms=%.1f "
              "playback_dbfs=%.1f playback_peak=%u "
              "remote_samples=%llu remote_rms=%.1f remote_dbfs=%.1f "
              "remote_peak=%u chime_samples=%llu chime_rms=%.1f "
              "chime_dbfs=%.1f chime_peak=%u "
              "playback_sink_samples=%llu playback_sink_rms=%.1f "
              "playback_sink_dbfs=%.1f playback_sink_peak=%u "
              "capture_after_aec_samples=%llu capture_after_aec_rms=%.1f "
              "capture_after_aec_dbfs=%.1f capture_after_aec_peak=%u "
              "playback_pts_ns=%llu capture_pts_ns=%llu pts_delta_ms=%lld\n",
              audio_playback_path_state_name(playback_path),
              static_cast<unsigned long long>(playback.sample_count),
              playback.rms,
              playback.rms_dbfs,
              playback.peak,
              static_cast<unsigned long long>(remote_playback.sample_count),
              remote_playback.rms,
              remote_playback.rms_dbfs,
              remote_playback.peak,
              static_cast<unsigned long long>(chime_playback.sample_count),
              chime_playback.rms,
              chime_playback.rms_dbfs,
              chime_playback.peak,
              static_cast<unsigned long long>(playback_sink.sample_count),
              playback_sink.rms,
              playback_sink.rms_dbfs,
              playback_sink.peak,
              static_cast<unsigned long long>(capture.sample_count),
              capture.rms,
              capture.rms_dbfs,
              capture.peak,
              static_cast<unsigned long long>(playback_pts_ns),
              static_cast<unsigned long long>(frame.pts_ns),
              static_cast<long long>(pts_delta_ms));
        }
      }
      dispatcher_.dispatch(frame);
      gst_buffer_unmap(buffer, &map);
    }

    gst_sample_unref(sample);
  }
}
