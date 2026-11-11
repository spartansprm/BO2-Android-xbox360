// Offline profile namespaces and the primary profile's legacy folder migration.
#pragma once

#include <cstdio>
#include <cstdint>
#include <filesystem>
#include <string>

#include <rex/system/xam/user_profile.h>

namespace rex::system::xam {
struct ProfileLocation {
  std::filesystem::path path, legacy;
  std::error_code error;
};

inline std::filesystem::path ProfileDirectory(const std::filesystem::path& root,
                                             uint32_t title_id, uint64_t xuid) {
  char title[9], user[17];
  std::snprintf(title, sizeof(title), "%08X", unsigned(title_id));
  std::snprintf(user, sizeof(user), "%016llX", static_cast<unsigned long long>(UserProfile::StorageXuid(xuid)));
  return root / title / "profile" / user;
}

inline ProfileLocation ResolveProfileDirectory(const std::filesystem::path& root,
                                               uint32_t title_id, uint64_t xuid,
                                               uint64_t primary_xuid) {
  ProfileLocation result{ProfileDirectory(root, title_id, xuid), {}, {}};
  // Secondary profiles never inherit name folders or another XUID's data.
  if (xuid != primary_xuid) return result;
  const auto profiles = result.path.parent_path();
  auto& ec = result.error;
  if (std::filesystem::exists(result.path, ec) || !std::filesystem::is_directory(profiles, ec)) return result;
  std::filesystem::file_time_type latest_time{};
  for (const auto& entry : std::filesystem::directory_iterator(profiles, ec)) {
    if (!entry.is_directory(ec)) continue;
    const auto folder = entry.path().filename().string();
    if (folder.size() == 16 && folder.find_first_not_of("0123456789abcdefABCDEF") == std::string::npos)
      continue;
    for (const auto& file : std::filesystem::directory_iterator(entry.path(), ec)) {
      const auto time = file.last_write_time(ec);
      if (!ec && (result.legacy.empty() || time > latest_time)) {
        result.legacy = entry.path();
        latest_time = time;
      }
    }
  }
  if (result.legacy.empty()) return result;
  std::filesystem::rename(result.legacy, result.path, ec);
  if (ec) {
    ec.clear();
    std::filesystem::copy(result.legacy, result.path, std::filesystem::copy_options::recursive, ec);
  }
  return result;
}
}  // namespace rex::system::xam
