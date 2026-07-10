#include "utils/audio_capture_manager.h"
#include "utils/audio_pipeline_config.h"

#include <gst/app/gstappsink.h>
#include <gst/app/gstappsrc.h>

#include <chrono>
#include <cstdio>
#include <cstring>
#include <utility>
#include <vector>

namespace {

std::once_flag g_audio_gst_init_once;

constexpr uint64_t kDefaultAudioFrameNs = 20ULL * 1000ULL * 1000ULL;
constexpr uint64_t kMetricsLogIntervalNs = 1000ULL * 1000ULL * 1000ULL;

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

  // 先创建回声参考探针，再激活采集侧 DSP；AEC 要求二者位于同一个顶层 pipeline。
  gchar *pipeline_desc = g_strdup_printf(
      "appsrc name=playback_src is-live=true block=false format=time "
      "do-timestamp=true "
      "caps=%s "
      "! "
      "queue leaky=downstream max-size-buffers=16 ! audioconvert ! "
      "audioresample ! "
      "%s ! "
      "volume name=live_playback_gain volume=0.3548 ! "
      "webrtcechoprobe name=echo_probe ! "
      "audioconvert ! audioresample ! "
      "%s ! "
      "alsasink name=playback_sink device=%s sync=true "
      "async=false "
      "alsasrc device=%s do-timestamp=true ! "
      "%s ! "
      "queue leaky=downstream max-size-buffers=8 ! audioconvert ! "
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
  GstPad *playback_sink_pad =
      playback_sink ? gst_element_get_static_pad(playback_sink, "sink")
                    : nullptr;
  if (!appsink || !playback_appsrc || !playback_sink_pad) {
    if (playback_sink_pad)
      gst_object_unref(playback_sink_pad);
    if (playback_sink)
      gst_object_unref(playback_sink);
    if (appsink)
      gst_object_unref(appsink);
    if (playback_appsrc)
      gst_object_unref(playback_appsrc);
    gst_object_unref(pipeline);
    if (error_message)
      *error_message = "audio manager app elements not found";
    return false;
  }
  gst_object_unref(playback_sink);
  gst_app_src_set_stream_type(GST_APP_SRC(playback_appsrc),
                              GST_APP_STREAM_TYPE_STREAM);
  g_object_set(G_OBJECT(playback_appsrc), "block", FALSE, nullptr);

  playback_level_meter_.snapshot_and_reset();
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
  latest_playback_pts_ns_.store(0);
  last_metrics_log_ns_ = monotonic_time_ns();
  seq_ = 0;
  pull_thread_ = std::thread(&AudioCaptureManager::pull_loop, this);
  std::fprintf(stdout,
               "[audio] capture started device=%s hardware_rate=%d "
               "hardware_channels=%d webrtc_rate=%d webrtc_channels=%d\n",
               device.c_str(), kAudioHardwareRate, kAudioHardwareChannels,
               kAudioWebRtcRate, kAudioWebRtcChannels);
  return true;
}

void AudioCaptureManager::stop() {
  stop_requested_.store(true);
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

bool AudioCaptureManager::push_playback_frame(const AudioFrame &frame) {
  if (!frame.data || frame.size == 0)
    return false;

  std::lock_guard<std::mutex> lock(playback_mtx_);
  if (!playback_appsrc_)
    return false;

  GstBuffer *buffer = gst_buffer_new_allocate(nullptr, frame.size, nullptr);
  if (!buffer)
    return false;

  GstMapInfo map;
  if (!gst_buffer_map(buffer, &map, GST_MAP_WRITE)) {
    gst_buffer_unref(buffer);
    return false;
  }
  std::memcpy(map.data, frame.data, frame.size);
  gst_buffer_unmap(buffer, &map);

  // 由 playback appsrc 按音频管理器当前 running-time 打时间戳，供 AEC
  // 对齐参考信号。
  GST_BUFFER_PTS(buffer) = GST_CLOCK_TIME_NONE;
  GST_BUFFER_DTS(buffer) = GST_CLOCK_TIME_NONE;
  GST_BUFFER_DURATION(buffer) = static_cast<GstClockTime>(frame.duration_ns);

  const uint64_t playback_pts_ns = pipeline_running_time_ns(pipeline_);
  const GstFlowReturn flow_ret =
      gst_app_src_push_buffer(GST_APP_SRC(playback_appsrc_), buffer);
  if (flow_ret == GST_FLOW_OK) {
    playback_level_meter_.add(frame.data, frame.size);
    latest_playback_pts_ns_.store(playback_pts_ns);
  }
  if (flow_ret != GST_FLOW_OK && flow_ret != GST_FLOW_FLUSHING) {
    std::fprintf(stderr, "[audio] playback appsrc push failed: %d\n", flow_ret);
  }
  return flow_ret == GST_FLOW_OK;
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
        const AudioLevelSnapshot playback =
            playback_level_meter_.snapshot_and_reset();
        const AudioLevelSnapshot playback_sink =
            playback_sink_level_meter_.snapshot_and_reset();
        const AudioLevelSnapshot capture =
            capture_level_meter_.snapshot_and_reset();
        last_metrics_log_ns_ = now_ns;
        if (playback.sample_count > 0 || playback_sink.sample_count > 0) {
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
