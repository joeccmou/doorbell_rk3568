#include "utils/audio_capture_manager.h"

#include <gst/app/gstappsink.h>
#include <gst/app/gstappsrc.h>

#include <cstdio>
#include <cstring>
#include <utility>
#include <vector>

namespace {

std::once_flag g_audio_gst_init_once;

constexpr uint64_t kDefaultAudioFrameNs = 20ULL * 1000ULL * 1000ULL;

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
      "caps=audio/x-raw,format=S16LE,layout=interleaved,rate=48000,channels=1 "
      "! "
      "queue leaky=downstream max-size-buffers=16 ! audioconvert ! "
      "audioresample ! "
      "audio/x-raw,format=S16LE,layout=interleaved,rate=48000,channels=1 ! "
      "webrtcechoprobe name=echo_probe ! alsasink device=%s sync=true "
      "async=false "
      "alsasrc device=%s do-timestamp=true ! "
      "audio/x-raw,format=S16LE,layout=interleaved,rate=48000,channels=1 ! "
      "webrtcdsp name=capture_dsp probe=echo_probe echo-cancel=true "
      "echo-suppression-level=high delay-agnostic=true extended-filter=true "
      "noise-suppression=true noise-suppression-level=high gain-control=true "
      "high-pass-filter=true ! "
      "queue leaky=downstream max-size-buffers=8 ! "
      "appsink name=audio_sink emit-signals=false sync=false max-buffers=8 "
      "drop=true",
      device.c_str(), device.c_str());

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
  if (!appsink || !playback_appsrc) {
    if (appsink)
      gst_object_unref(appsink);
    if (playback_appsrc)
      gst_object_unref(playback_appsrc);
    gst_object_unref(pipeline);
    if (error_message)
      *error_message = "audio manager app elements not found";
    return false;
  }
  gst_app_src_set_stream_type(GST_APP_SRC(playback_appsrc),
                              GST_APP_STREAM_TYPE_STREAM);
  g_object_set(G_OBJECT(playback_appsrc), "block", FALSE, nullptr);

  GstStateChangeReturn ret = gst_element_set_state(pipeline, GST_STATE_PLAYING);
  if (ret == GST_STATE_CHANGE_FAILURE) {
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
  stop_requested_.store(false);
  seq_ = 0;
  pull_thread_ = std::thread(&AudioCaptureManager::pull_loop, this);
  std::fprintf(stdout, "[audio] capture started device=%s\n", device.c_str());
  return true;
}

void AudioCaptureManager::stop() {
  stop_requested_.store(true);
  if (pull_thread_.joinable()) {
    pull_thread_.join();
  }

  GstElement *appsink = appsink_;
  GstElement *pipeline = pipeline_;
  GstElement *playback_appsrc = nullptr;
  {
    std::lock_guard<std::mutex> lock(playback_mtx_);
    playback_appsrc = playback_appsrc_;
    playback_appsrc_ = nullptr;
  }
  appsink_ = nullptr;
  pipeline_ = nullptr;

  if (playback_appsrc)
    gst_app_src_end_of_stream(GST_APP_SRC(playback_appsrc));
  if (pipeline) {
    gst_element_set_state(pipeline, GST_STATE_NULL);
  }
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

  const GstFlowReturn flow_ret =
      gst_app_src_push_buffer(GST_APP_SRC(playback_appsrc_), buffer);
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
      dispatcher_.dispatch(frame);
      gst_buffer_unmap(buffer, &map);
    }

    gst_sample_unref(sample);
  }
}
