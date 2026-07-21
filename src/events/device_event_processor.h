#pragma once

#include "events/event_record.h"
#include "events/event_store.h"
#include "events/http_snapshot_uploader.h"
#include "events/http_clip_uploader.h"
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
    using CommandAckPublisher = std::function<bool(
        const std::string &trace_id,
        const std::string &cmd_id,
        bool ok,
        const std::string &error_code,
        const std::string &data_json)>;

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
        const std::string &recording_id,
        std::string *error = nullptr);

    // 录像管线启动成功后调用；生成不可变 recording_id 并写本地 SQLite。
    std::optional<std::string> register_started_recording(
        std::chrono::system_clock::time_point started_at,
        std::string *error = nullptr);

    bool complete_recording(
        const std::string &recording_id,
        const std::string &clip_ref,
        std::chrono::system_clock::time_point started_at,
        std::chrono::system_clock::time_point ended_at,
        int64_t media_duration_ms,
        int64_t size_bytes,
        const std::string &sha256,
        const std::string &status,
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
    // 只有后端业务 ACK 才能推进 reported；MQTT PUBACK 不改变本地状态。
    void handle_report_ack(const std::string &payload);
    bool handle_media_command(const std::string &payload, const CommandAckPublisher &ack_publisher);

private:
    struct Job {
        enum class Kind {
            ReportEvent,
            SaveSnapshot,
        };

        Kind kind = Kind::ReportEvent;
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
    HttpClipUploader clip_uploader_;
    bool media_available_ = false;

    std::atomic<bool> stop_{false};
    std::mutex mutex_;
    std::condition_variable cv_;
    std::deque<Job> jobs_;
    std::thread worker_;
};
