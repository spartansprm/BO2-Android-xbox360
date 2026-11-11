// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <filesystem>
#include <string>
namespace bo2zm::android::fault {
void Start(const std::filesystem::path& files,const std::string& run,const void* virtual_base);
void Stop();
}
