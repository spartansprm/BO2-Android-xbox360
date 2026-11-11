/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2014 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#include <rex/filesystem/devices/stfs_container_entry.h>
#include <rex/filesystem/devices/stfs_container_file.h>

#include <algorithm>
#include <cmath>
#include <cerrno>
#if !REX_PLATFORM_WIN32 && !REX_PLATFORM_SWITCH
#include <unistd.h>
#endif

#include <rex/cvar.h>
#include <rex/math.h>
#include <rex/platform.h>

#include "range_read_cache.h"


#if REX_PLATFORM_ANDROID
#define REX_STFS_CACHE_DEFAULT_MB 96
#else
#define REX_STFS_CACHE_DEFAULT_MB 0
#endif
REXCVAR_DEFINE_INT32(stfs_read_cache_mb, REX_STFS_CACHE_DEFAULT_MB, "Filesystem",
                     "Exact-range read cache for container reads, total cap in MB (0 = off). Read-only "
                     "data only.");
REXCVAR_DEFINE_INT32(stfs_read_cache_min_kb, 64, "Filesystem",
                     "Only container reads of at least this many KB are cached.");
REXCVAR_DEFINE_INT32(stfs_read_cache_max_mb, 12, "Filesystem",
                     "Container reads larger than this many MB are not cached.");

namespace rex::filesystem {

namespace {
detail::RangeReadCache& ReadCache() {
  static detail::RangeReadCache cache;
  return cache;
}
detail::RangeReadCache::Limits CacheLimits() {
  detail::RangeReadCache::Limits limits;
  const int64_t cap = int64_t(REXCVAR_GET(stfs_read_cache_mb));
  limits.cap_bytes = cap > 0 ? uint64_t(cap) << 20 : 0;
  limits.floor_bytes = uint64_t(std::max<int64_t>(1, REXCVAR_GET(stfs_read_cache_min_kb))) << 10;
  limits.ceiling_bytes = uint64_t(std::max<int64_t>(1, REXCVAR_GET(stfs_read_cache_max_mb))) << 20;
  return limits;
}
}  // namespace

RangeStats StfsReadCacheStats() {
  const auto s = ReadCache().GetStats();
  const auto limits = CacheLimits();
  RangeStats r;
  r.hits = s.hits;
  r.misses = s.misses;
  r.bytes_ram = s.bytes_ram;
  r.bytes_disk = s.bytes_disk;
  r.entries = s.entries;
  r.live_bytes = s.live_bytes;
  r.evictions = s.evictions;
  r.cap_mb = limits.cap_bytes >> 20;
  r.low_floor = s.low_floor;
  r.over_ceiling = s.over_ceiling;
  r.sequential = s.sequential;
  r.sequential_bytes = s.sequential_bytes >> 20;
  r.floor_kb = limits.floor_bytes >> 10;
  r.no_memory = s.no_memory;
  return r;
}

StfsContainerFile::StfsContainerFile(uint32_t file_access, StfsContainerEntry* entry)
    : File(file_access, entry), entry_(entry) {}

StfsContainerFile::~StfsContainerFile() = default;

void StfsContainerFile::Destroy() {
  delete this;
}

X_STATUS StfsContainerFile::ReadSync(std::span<uint8_t> buffer, size_t byte_offset,
                                     size_t* out_bytes_read) {
  *out_bytes_read = 0;
  if (byte_offset >= entry_->size()) {
    return X_STATUS_END_OF_FILE;
  }
  // What the uncached path would deliver: the request clipped to the end of the file. Only complete reads of
  // that size are served from, or stored in, the cache.
  const size_t want = std::min(buffer.size(), entry_->size() - byte_offset);
  const auto limits = CacheLimits();
  auto& cache = ReadCache();
  const auto outcome = want ? cache.Serve(entry_, byte_offset, buffer.data(), want, limits)
                            : detail::RangeReadCache::Result::kBypass;
  if (outcome == detail::RangeReadCache::Result::kHit) {
    *out_bytes_read = want;
    return X_STATUS_SUCCESS;
  }
  const X_STATUS status = ReadSyncUncached(buffer, byte_offset, out_bytes_read);
  if (outcome == detail::RangeReadCache::Result::kMiss && status == X_STATUS_SUCCESS &&
      *out_bytes_read == want) {
    cache.Store(entry_, byte_offset, buffer.data(), want, limits);
  }
  return status;
}

X_STATUS StfsContainerFile::ReadSyncUncached(std::span<uint8_t> buffer, size_t byte_offset,
                                             size_t* out_bytes_read) {
  *out_bytes_read = 0;
  if (byte_offset >= entry_->size()) {
    return X_STATUS_END_OF_FILE;
  }

  size_t src_offset = 0;
  uint8_t* p = buffer.data();
  size_t remaining_length = std::min(buffer.size(), entry_->size() - byte_offset);

  *out_bytes_read = 0;
  for (size_t i = 0; i < entry_->block_list().size(); i++) {
    auto& record = entry_->block_list()[i];
    if (src_offset + record.length <= byte_offset) {
      // Doesn't begin in this region. Skip it.
      src_offset += record.length;
      continue;
    }

    size_t read_offset = (byte_offset > src_offset) ? byte_offset - src_offset : 0;
    size_t read_length = std::min(record.length - read_offset, remaining_length);

    auto& file = entry_->files()->at(record.file);
    size_t num_read = 0;
#if !REX_PLATFORM_WIN32 && !REX_PLATFORM_SWITCH
    // Positional I/O avoids shared FILE seek races between simultaneous game reads.
    while (num_read < read_length) {
      const auto got = pread(fileno(file), p + num_read, read_length - num_read,
                              off_t(record.offset + read_offset + num_read));
      if (got < 0 && errno == EINTR) continue;
      if (got <= 0) return X_STATUS_UNSUCCESSFUL;
      num_read += size_t(got);
    }
#else
    rex::filesystem::Seek(file, record.offset + read_offset, SEEK_SET);
    num_read = fread(p, 1, read_length, file);
    if (num_read != read_length) return X_STATUS_UNSUCCESSFUL;
#endif

    *out_bytes_read += num_read;
    p += num_read;
    src_offset += record.length;
    remaining_length -= read_length;
    if (remaining_length == 0) {
      break;
    }
  }

  return X_STATUS_SUCCESS;
}

}  // namespace rex::filesystem
