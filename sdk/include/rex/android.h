// SPDX-License-Identifier: BSD-3-Clause
#pragma once
#include <cstdint>
#include <filesystem>
namespace rex {
uint32_t GetAndroidApiLevel();
void ConfigureAndroidPaths(std::filesystem::path native_libraries, std::filesystem::path private_files);
const std::filesystem::path& GetAndroidNativeLibraryDir();
const std::filesystem::path& GetAndroidPrivateFilesDir();
}
