#include "events/event_paths.h"

#include <cassert>
#include <chrono>

int main() {
    using namespace std::chrono;
    const auto at = system_clock::time_point(milliseconds(1783731723456LL));
    assert(format_event_id(at, 7) == "20260711T010203456Z-0000007");
    // 当前项目设备时区为 Asia/Shanghai；文件路径使用设备本地时间，事件 ID 仍用 UTC。
    assert(snapshot_relative_path(at) == "snapshots/20260711/20260711-090203456.jpg");
    assert(clip_directory_relative_path(at) == "clips/20260711");
    assert(local_date_iso(at) == "2026-07-11");
    assert(utc_offset_minutes(at) == 480);
    const auto year_boundary = system_clock::time_point(milliseconds(1798740000000LL));
    assert(utc_offset_minutes(year_boundary) == 480);
    assert(clip_relative_path(at, 1) == "clips/20260711/20260711T090203456-0000001.mp4");
    assert(is_canonical_clip_ref("clips/20260711/20260711-010203456起-010233止.mp4"));
    assert(!is_canonical_clip_ref("clips/../secret.mp4"));
    assert(!is_canonical_clip_ref("/clips/20260711/a.mp4"));
    assert(is_canonical_snapshot_path("snapshots/20260711/20260711-090203456.jpg"));
    assert(!is_canonical_snapshot_path("snapshots/20260711/../../secret.jpg"));
    return 0;
}
