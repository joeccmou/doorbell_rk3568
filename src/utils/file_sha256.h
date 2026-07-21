#pragma once

#include <string>

// 计算文件内容的 SHA-256，成功时返回 64 位小写十六进制字符串。
bool file_sha256(const std::string &path,
                 std::string *digest,
                 std::string *error = nullptr);
