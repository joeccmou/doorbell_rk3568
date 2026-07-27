#pragma once

#include "utils/audio_capture_manager.h"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>

class DoorbellChimePlayer {
public:
    DoorbellChimePlayer(AudioCaptureManager *audio, std::string wav_path);
    ~DoorbellChimePlayer();

    DoorbellChimePlayer(const DoorbellChimePlayer &) = delete;
    DoorbellChimePlayer &operator=(const DoorbellChimePlayer &) = delete;

    void start();
    void stop();
    void play_once();

private:
    enum class PlaybackResult {
        kCompleted,
        kInterrupted,
        kFailed,
    };

    void worker_loop();
    PlaybackResult play_file_once(
        uint64_t play_sequence,
        unsigned repeat_index);

    AudioCaptureManager *audio_ = nullptr;
    std::string wav_path_;
    std::atomic<bool> stop_requested_{false};
    std::atomic<uint64_t> requested_play_sequence_{0};
    std::mutex mutex_;
    std::condition_variable cv_;
    std::thread worker_;
};
