// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <array>
#include <cctype>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <rex/filesystem/devices/stfs_container_device.h>
#include <rex/system/xam/read_only_content.h>
#include "dlc_license.h"
namespace bo2zm::android {
struct DlcPackage {
  std::unique_ptr<rex::filesystem::StfsContainerDevice> device;
  rex::system::xam::ReadOnlyContent content;
  std::vector<std::string> markers;
  bool mtx=false;
  uint32_t header_license=0;
};
template<class T> inline auto DlcHeaderValue(const T* field) {
  T aligned;std::memcpy(&aligned,field,sizeof(aligned));
  if constexpr(requires{aligned.get();})return aligned.get();else return aligned;
}
inline DlcPackage OpenDlcPackage(int fd,std::string name,std::string mount,uint32_t title) {
  using namespace rex::system;
  DlcPackage p;p.device=std::make_unique<rex::filesystem::StfsContainerDevice>(mount,"[saf-dlc]");
  if(!p.device->InitializeFromFileDescriptors(fd,{}))throw std::runtime_error("Invalid read-only STFS package");
  const auto& header=p.device->header();
  if(DlcHeaderValue(&header.metadata.execution_info.title_id)!=title||DlcHeaderValue(&header.metadata.content_type)!=XContentType::kMarketplaceContent||
     DlcHeaderValue(&header.metadata.volume_type)!=rex::filesystem::XContentVolumeType::kStfs)
    throw std::runtime_error("Not a BO2 Marketplace STFS package");
  auto& c=p.content;c.device_path=mount;c.data.device_id=1;c.data.content_type=XContentType::kMarketplaceContent;
  c.data.title_id=title;c.data.xuid=0;c.data.set_file_name(name);
  std::array<rex::be<uint16_t>,128> english{};
  std::memcpy(english.data(),&header.metadata.display_name_raw.uint[0],sizeof(english));
  std::u16string display;for(const auto& word:english){if(!word.get())break;display.push_back(char16_t(word.get()));}
  c.data.set_display_name(display);
  decltype(header.header.licenses) licenses{};
  std::memcpy(licenses,&header.header.licenses,sizeof(licenses));
  for(const auto& license:licenses)if(DlcHeaderValue(&license.license_flags))c.license|=DlcHeaderValue(&license.license_bits);
  if(auto* root=p.device->ResolvePath(""))for(const auto& entry:root->children()) {
    const auto n=entry->name();
    if(n.starts_with("dlc")&&n.ends_with(".id"))p.markers.push_back(n);
    std::string lower=n;for(auto& ch:lower)ch=char(std::tolower(static_cast<unsigned char>(ch)));
    if(lower.starts_with("mtx")&&lower.ends_with(".id"))p.mtx=true;
  }
  p.header_license=c.license;c.license=DlcGameLicense(c.license,p.mtx);

  if(name.size()>42||c.data.file_name()!=name)throw std::runtime_error("Invalid DLC package filename");
  xam::ReadOnlyContentCatalog check;
  if(!check.Register(c))throw std::runtime_error("Invalid DLC package identity");
  return p;
}
}
