#include "utils/audio_pipeline_config.h"

#include <cassert>
#include <string>

void test_audio_pipeline_keeps_hardware_and_webrtc_rates_separate() {
  assert(kAudioHardwareRate == 44100);
  assert(kAudioHardwareChannels == 2);
  assert(kAudioWebRtcRate == 48000);
  assert(kAudioWebRtcChannels == 1);
}

void test_audio_pipeline_caps_are_named_for_log_and_pipeline_use() {
  assert(std::string(audio_hardware_raw_caps()) ==
         "audio/x-raw,format=S16LE,layout=interleaved,rate=44100,channels=2");
  assert(std::string(audio_webrtc_raw_caps()) ==
         "audio/x-raw,format=S16LE,layout=interleaved,rate=48000,channels=1");
}

void test_capture_mix_selects_hardware_channel_zero() {
  const auto coefficients = audio_capture_channel_mix_coefficients();
  const float channel_zero = 1000.0F;
  const float channel_one = 30000.0F;
  const float output = channel_zero * coefficients[0] +
                       channel_one * coefficients[1];

  assert(output == 1000.0F);
}

int main() {
  test_audio_pipeline_keeps_hardware_and_webrtc_rates_separate();
  test_audio_pipeline_caps_are_named_for_log_and_pipeline_use();
  test_capture_mix_selects_hardware_channel_zero();
  return 0;
}
