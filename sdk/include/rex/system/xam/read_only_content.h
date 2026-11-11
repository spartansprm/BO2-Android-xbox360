// SPDX-License-Identifier: BSD-3-Clause
#pragma once
#include <algorithm>
#include <mutex>
#include <optional>
#include <string>
#include <vector>
#include <rex/system/xam/content_manager.h>
namespace rex::system::xam {
struct ReadOnlyContent {
  XCONTENT_AGGREGATE_DATA data{};
  std::string device_path;
  uint32_t license = 0;
};
class ReadOnlyContentCatalog {
 public:
  static bool SameName(std::string_view a, std::string_view b) {
    auto lower=[](unsigned char c){return c>='A'&&c<='Z'?c+('a'-'A'):c;};
    return a.size()==b.size()&&std::equal(a.begin(),a.end(),b.begin(),[&](auto x,auto y){return lower(x)==lower(y);});
  }
  bool Register(ReadOnlyContent value) {
    const auto name=value.data.file_name();
    if(value.data.content_type!=XContentType::kMarketplaceContent||uint64_t(value.data.xuid)!=0||
       !uint32_t(value.data.title_id)||uint32_t(value.data.title_id)==uint32_t(-1)||
       name.empty()||name.size()>42||name=="."||name==".."||
       name.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789._-")!=std::string::npos||
       !value.device_path.starts_with("\\Device\\"))return false;
    std::lock_guard lock(mutex_);
    if(packages_.size()>=32)return false;
    for(const auto& p:packages_)if(p.data.title_id==value.data.title_id&&SameName(p.data.file_name(),name))return false;
    packages_.push_back(std::move(value));return true;
  }
  std::optional<ReadOnlyContent> Find(uint32_t title,XContentType type,std::string_view name)const {
    std::lock_guard lock(mutex_);
    for(const auto& p:packages_)if(uint32_t(p.data.title_id)==title&&p.data.content_type==type&&SameName(p.data.file_name(),name))return p;
    return std::nullopt;
  }
  std::vector<ReadOnlyContent> List(uint32_t title,XContentType type)const {
    std::lock_guard lock(mutex_);std::vector<ReadOnlyContent> out;
    for(const auto& p:packages_)if(uint32_t(p.data.title_id)==title&&p.data.content_type==type)out.push_back(p);
    return out;
  }
 private:
  mutable std::mutex mutex_;
  std::vector<ReadOnlyContent> packages_;
};
}  // namespace rex::system::xam
