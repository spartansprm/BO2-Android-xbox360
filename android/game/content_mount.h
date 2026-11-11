// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <rex/filesystem/vfs.h>
#include <span>
namespace bo2zm::android {


void MountContent(rex::filesystem::VirtualFileSystem& vfs,int header,
                  std::span<const int> fragments,int update);
}
