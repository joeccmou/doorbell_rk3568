#include "utils/audio_signal_metrics.h"

#include <cassert>
#include <cmath>
#include <cstdint>
#include <string>

void test_pcm_s16_level_meter_reports_and_resets_window() {
  PcmS16LevelMeter meter;
  const uint8_t samples[] = {
      0xff, 0x7f, // 32767
      0x00, 0x80, // -32768
      0x55,       // 忽略不完整的尾部样本
  };

  meter.add(samples, sizeof(samples));
  const AudioLevelSnapshot snapshot = meter.snapshot_and_reset();

  assert(snapshot.sample_count == 2);
  assert(snapshot.peak == 32768);
  assert(std::abs(snapshot.rms - 32767.5) < 0.01);
  assert(snapshot.rms_dbfs > -0.01);

  const AudioLevelSnapshot reset_snapshot = meter.snapshot_and_reset();
  assert(reset_snapshot.sample_count == 0);
  assert(reset_snapshot.rms == 0.0);
  assert(reset_snapshot.peak == 0);
}

void test_audio_timestamp_delta_ms_handles_both_directions() {
  assert(audio_timestamp_delta_ms(1'050'000'000ULL, 1'000'000'000ULL) == 50);
  assert(audio_timestamp_delta_ms(950'000'000ULL, 1'000'000'000ULL) == -50);
  assert(audio_timestamp_delta_ms(0, 1'000'000'000ULL) == 0);
}

void test_audio_playback_path_state_distinguishes_sink_delivery() {
  AudioLevelSnapshot empty;
  AudioLevelSnapshot input;
  input.sample_count = 48000;
  AudioLevelSnapshot sink;
  sink.sample_count = 48000;

  assert(audio_playback_path_state(empty, empty) ==
         AudioPlaybackPathState::kIdle);
  assert(audio_playback_path_state(input, empty) ==
         AudioPlaybackPathState::kBeforeSink);
  assert(audio_playback_path_state(input, sink) ==
         AudioPlaybackPathState::kAtSink);
  assert(std::string(audio_playback_path_state_name(
             AudioPlaybackPathState::kAtSink)) == "at_sink");
}

int main() {
  test_pcm_s16_level_meter_reports_and_resets_window();
  test_audio_timestamp_delta_ms_handles_both_directions();
  test_audio_playback_path_state_distinguishes_sink_delivery();
  return 0;
}
