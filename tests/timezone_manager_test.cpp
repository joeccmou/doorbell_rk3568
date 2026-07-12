#include "device/timezone_manager.h"

#include <cassert>
#include <filesystem>
#include <fstream>
#include <string>

namespace {

void write_text(const std::filesystem::path &path, const std::string &value) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output << value;
}

std::string read_text(const std::filesystem::path &path) {
    std::ifstream input(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
}

}  // namespace

int main() {
    const auto root = std::filesystem::temp_directory_path() / "doorbell_timezone_manager_test";
    std::error_code ec;
    std::filesystem::remove_all(root, ec);
    write_text(root / "zoneinfo/Asia/Shanghai", "shanghai-zone-data");
    write_text(root / "zoneinfo/Asia/Urumqi", "urumqi-zone-data");
    write_text(root / "etc/localtime", "old-zone-data");
    write_text(root / "etc/timezone", "Asia/Shanghai\n");

    TimezoneManager manager({
        (root / "zoneinfo").string(),
        (root / "etc/localtime").string(),
        (root / "etc/timezone").string(),
    });

    auto invalid = manager.apply("../../etc/passwd");
    assert(!invalid.ok);
    assert(invalid.error_code == "TIMEZONE_INVALID");
    assert(read_text(root / "etc/localtime") == "old-zone-data");

    auto missing = manager.apply("Asia/Missing");
    assert(!missing.ok);
    assert(missing.error_code == "TIMEZONE_INVALID");

    auto applied = manager.apply("Asia/Urumqi");
    assert(applied.ok);
    assert(applied.timezone == "Asia/Urumqi");
    assert(read_text(root / "etc/localtime") == "urumqi-zone-data");
    assert(read_text(root / "etc/timezone") == "Asia/Urumqi\n");

    // 模拟上次在 localtime 改名为备份后掉电，本次应用必须先恢复再继续。
    std::filesystem::rename(root / "etc/localtime", root / "etc/localtime.doorbell-backup");
    auto recovered = manager.apply("Asia/Shanghai");
    assert(recovered.ok);
    assert(read_text(root / "etc/localtime") == "shanghai-zone-data");

    std::filesystem::remove_all(root, ec);
    return 0;
}
