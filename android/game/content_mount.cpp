// SPDX-License-Identifier: GPL-3.0-only
#include "content_mount.h"
#include <rex/filesystem/devices/stfs_container_device.h>
#include <stdexcept>
namespace bo2zm::android {
void MountContent(rex::filesystem::VirtualFileSystem& vfs,int header,
                  std::span<const int> fragments,int update) {
  using rex::filesystem::StfsContainerDevice;
  auto base=std::make_unique<StfsContainerDevice>("\\Device\\AndroidBO2Base","[saf-base]");
  auto patch=std::make_unique<StfsContainerDevice>("\\Device\\AndroidBO2Update","[saf-update]");
  if(!base->InitializeFromFileDescriptors(header,fragments)||!patch->InitializeFromFileDescriptors(update,{}))
    throw std::runtime_error("Read-only game/update descriptor mount failed");
  if(base->header().metadata.execution_info.title_id!=0x415608C3||
     patch->header().metadata.execution_info.title_id!=0x415608C3||
     base->header().metadata.execution_info.media_id!=patch->header().metadata.execution_info.media_id)
    throw std::runtime_error("Game/update title or media IDs differ");
  if(!vfs.RegisterDevice(std::move(base)))throw std::runtime_error("Game device registration failed");
  if(!vfs.RegisterDevice(std::move(patch))) {
    vfs.UnregisterDevice("\\Device\\AndroidBO2Base");throw std::runtime_error("Update device registration failed");
  }
  for(const auto& [alias,target]:{
      std::pair{"game:","\\Device\\AndroidBO2Base"},
      std::pair{"d:","\\Device\\AndroidBO2Base"},
      std::pair{"update:","\\Device\\AndroidBO2Update"},
      std::pair{"\\Device\\Harddisk0\\Partition1","\\Device\\AndroidBO2Base"},
      std::pair{"\\Device\\Harddisk0\\PartitionUpdate","\\Device\\AndroidBO2Update"}}) {
    vfs.UnregisterSymbolicLink(alias);vfs.RegisterSymbolicLink(alias,target);
  }
}
}
