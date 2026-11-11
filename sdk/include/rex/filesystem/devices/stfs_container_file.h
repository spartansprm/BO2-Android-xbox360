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

#pragma once

#include <span>

#include <rex/filesystem/devices/host_path_file.h>  // RangeStats
#include <rex/filesystem/file.h>

namespace rex::filesystem {

class StfsContainerEntry;

// Counters of the exact-range read cache that sits in front of StfsContainerFile::ReadSync (see
// range_read_cache.h). Same shape as ReadRangeStats() so the I/O report can print both.
RangeStats StfsReadCacheStats();

class StfsContainerFile : public File {
 public:
  StfsContainerFile(uint32_t file_access, StfsContainerEntry* entry);
  ~StfsContainerFile() override;

  void Destroy() override;

  X_STATUS ReadSync(std::span<uint8_t> buffer, size_t byte_offset, size_t* out_bytes_read) override;
  X_STATUS WriteSync(std::span<const uint8_t> /*buffer*/, size_t /*byte_offset*/,
                     size_t* /*out_bytes_written*/) override {
    return X_STATUS_ACCESS_DENIED;
  }
  X_STATUS SetLength(size_t /*length*/) override { return X_STATUS_ACCESS_DENIED; }

 private:
  // The original ReadSync body: reads straight from the container's descriptors.
  X_STATUS ReadSyncUncached(std::span<uint8_t> buffer, size_t byte_offset, size_t* out_bytes_read);

  StfsContainerEntry* entry_;
};

}  // namespace rex::filesystem
