// SPDX-License-Identifier: GPL-3.0-only
#include "shader_capture.h"
#include <algorithm>
#include <cstdio>
#include <fstream>
#include <mutex>
#include <stdexcept>
#include <vector>
#define XXH_FORCE_MEMORY_ACCESS 0
#define XXH_INLINE_ALL
#include <xxhash.h>

namespace bo2zm::android::capture {
namespace {
std::mutex guard;
std::filesystem::path directory;
Stats stats;
uint64_t byte_limit=0;
uint32_t file_limit=0;
uint32_t be(std::span<const uint8_t> b,size_t at) {
  return uint32_t(b[at])<<24|uint32_t(b[at+1])<<16|uint32_t(b[at+2])<<8|b[at+3];
}
}
void Start(const std::filesystem::path& root,uint64_t bytes,uint32_t files) {
  std::lock_guard lock(guard);
  if(!root.is_absolute()||bytes>kMaxBytes||files>kMaxFiles)throw std::runtime_error("Invalid private shader capture limits/path");
  std::filesystem::create_directories(root);
  if(!std::filesystem::is_empty(root))throw std::runtime_error("Capture directory must be new and empty");
  directory=root;stats={};byte_limit=bytes;file_limit=files;
}
void Record(std::span<const uint8_t> blob,bool known) {
  std::lock_guard lock(guard);
  if(directory.empty())return;
  ++stats.calls;
  if(known){++stats.known;return;}
  if(blob.size()<24||blob.size()>65536||(be(blob,0)&0xFFFFFF00u)!=0x102A1100u||
     be(blob,4)<24||!be(blob,8)||uint64_t(be(blob,4))+be(blob,8)!=blob.size()) {
    ++stats.rejected;return;
  }
  try {
    const auto hash=XXH3_128bits(blob.data(),blob.size());char name[48];
    std::snprintf(name,sizeof(name),"%c_%016llx%016llx.bin",(be(blob,0)&1)?'v':'p',
                  static_cast<unsigned long long>(hash.high64),static_cast<unsigned long long>(hash.low64));
    const auto file=directory/name;
    if(std::filesystem::exists(file)) {
      std::ifstream input(file,std::ios::binary);
      std::vector<uint8_t> prior(blob.size());input.read(reinterpret_cast<char*>(prior.data()),prior.size());
      if(!input||input.peek()!=std::char_traits<char>::eof()||!std::equal(prior.begin(),prior.end(),blob.begin()))
        throw std::runtime_error("Shader capture hash collision or changed private file");
      ++stats.duplicates;return;
    }
    if(stats.unique>=file_limit||blob.size()>byte_limit-stats.bytes){++stats.limited;return;}
    auto part=file;part+=".part";
    try {
      std::ofstream output(part,std::ios::binary|std::ios::trunc);
      output.write(reinterpret_cast<const char*>(blob.data()),blob.size());output.close();
      if(!output)throw std::runtime_error("Cannot write private shader capture");
      std::filesystem::rename(part,file);
    }catch(...) {std::error_code ec;std::filesystem::remove(part,ec);throw;}
    ++stats.unique;stats.bytes+=blob.size();
  }catch(const std::exception& e){++stats.errors;stats.last_error=e.what();}
}
Stats Snapshot(){std::lock_guard lock(guard);return stats;}
}
