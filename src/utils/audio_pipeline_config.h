#pragma once

constexpr int kAudioHardwareRate = 44100;
constexpr int kAudioHardwareChannels = 2;
constexpr int kAudioWebRtcRate = 48000;
constexpr int kAudioWebRtcChannels = 1;

inline const char *audio_hardware_raw_caps() {
  return "audio/x-raw,format=S16LE,layout=interleaved,rate=44100,channels=2";
}

inline const char *audio_webrtc_raw_caps() {
  return "audio/x-raw,format=S16LE,layout=interleaved,rate=48000,channels=1";
}
