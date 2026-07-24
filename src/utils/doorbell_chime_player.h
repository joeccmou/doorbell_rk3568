#pragma once

#include "utils/audio_capture_manager.h"

#include <atomic>
#include <condition_variable>
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
    void worker_loop();
    bool play_file_once();

    AudioCaptureManager *audio_ = nullptr;
    std::string wav_path_;
    std::atomic<bool> stop_requested_{false};
    std::mutex mutex_;
    std::condition_variable cv_;
    unsigned pending_plays_ = 0;
    std::thread worker_;
};
