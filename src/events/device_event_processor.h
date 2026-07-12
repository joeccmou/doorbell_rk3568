#pragma once

#include "events/event_record.h"
#include "events/event_store.h"
#include "events/http_snapshot_uploader.h"
#include "events/snapshot_service.h"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

class DeviceEventProcessor {
public:
    using Publisher = std::function<bool(const std::string &payload)>;

    DeviceEventProcessor(std::string device_id,
                         std::string device_secret,
                         std::string api_base_url,
                         std::string data_dir,
                         std::string media_root,
                         Publisher publisher);
    ~DeviceEventProcessor();

    bool start(std::string *error = nullptr);
    void stop();

    // 首次检测时先同步落 SQLite，事件时间不受后续选帧窗口影响。
    std::optional<EventRecord> begin_person_event(
        double confidence,
        std::chrono::system_clock::time_point at,
        std::string *error = nullptr);

    std::optional<EventRecord> begin_person_event_with_clip(
        double confidence,
        std::chrono::system_clock::time_point at,
        const std::string &existing_clip_ref,
        std::string *error = nullptr);

    // 每次真正新建物理 MP4 时调用；返回设备媒体根目录下的相对路径。
    std::optional<std::string> allocate_recording_clip(
        std::chrono::system_clock::time_point at,
        std::string *error = nullptr);

    // 选帧窗口结束后，把最佳完整画面交给后台线程保存、上传并上报。
    void submit_person_snapshot(EventRecord event,
                                std::vector<uint8_t> rgb,
                                uint32_t width,
                                uint32_t height);

    // 先同步落 SQLite，再把抓拍上传与 MQTT 上报交给后台线程。
    std::optional<EventRecord> create_person_event(const std::vector<uint8_t> &rgb,
                                                   uint32_t width,
                                                   uint32_t height,
                                                   double confidence,
                                                   std::string *error = nullptr);

private:
    struct Job {
        EventRecord event;
        std::vector<uint8_t> rgb;
        uint32_t width = 0;
        uint32_t height = 0;
    };

    void worker_loop();
    bool process(Job *job);
    void enqueue(Job job);

    std::string device_id_;
    std::string data_dir_;
    std::string media_root_;
    Publisher publisher_;
    EventStore store_;
    SnapshotService snapshots_;
    HttpSnapshotUploader uploader_;
    bool media_available_ = false;

    std::atomic<bool> stop_{false};
    std::mutex mutex_;
    std::condition_variable cv_;
    std::deque<Job> jobs_;
    std::thread worker_;
};
