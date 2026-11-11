// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
namespace bo2zm::android::capture {
inline constexpr uint64_t kMaxBytes = 32 * 1024 * 1024;
inline constexpr uint32_t kMaxFiles = 8192;
struct Stats {
  uint64_t calls=0,known=0,unique=0,duplicates=0,rejected=0,limited=0,errors=0,bytes=0;
  std::string last_error;
};

void Start(const std::filesystem::path& root,uint64_t bytes=kMaxBytes,uint32_t files=kMaxFiles);
void Record(std::span<const uint8_t> container,bool known);
Stats Snapshot();
}
