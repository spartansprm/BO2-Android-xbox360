/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2013 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include <rex/filesystem.h>
#include <rex/filesystem/file.h>

namespace rex::filesystem {

class HostPathEntry;

/*
 * How much is being served from RAM instead of from the SD.
 *
 * hits      = game reads answered without going to disk.
 * fills     = times the disk had to be read to fill the window.
 * direct    = reads that bypassed the window (large ones, writes, save files).
 * bytes_ram = bytes the game received from the window.
 */
struct WindowStats {
  uint64_t hits = 0;
  uint64_t fills = 0;
  uint64_t direct = 0;
  uint64_t bytes_ram = 0;
  uint64_t windows_live = 0;
};
WindowStats ReadWindowStats();

/*
 * Exact-range read cache. See switch_io_ranges_mb in the .cpp.
 *
 * Everything is cumulative since startup, not per interval: there are few events and the total is
 * what matters.
 *
 * hits         = large reads answered from RAM, without going to the SD.
 * misses       = eligible large reads that did have to go to the SD.
 * bytes_ram    = bytes delivered to the game from the cache.
 * bytes_disk  = bytes those same reads brought from the SD.
 * entries      = ranges alive right now.
 * live_bytes  = what those ranges take in the host heap.
 * evictions    = ranges dropped because of the cap. If this grows, the cap is too small.
 * no_memory  = the cache turned itself off for lack of RAM.
 */
struct RangeStats {
  uint64_t hits = 0;
  uint64_t misses = 0;
  uint64_t bytes_ram = 0;
  uint64_t bytes_disk = 0;
  uint64_t entries = 0;
  uint64_t live_bytes = 0;
  uint64_t evictions = 0;
  uint64_t cap_mb = 0;  // the cvar, so the summary does not have to declare it on its own
  // Why something was not cached. Without this the cache could not be tuned.
  uint64_t low_floor = 0;          // reads too small
  uint64_t over_ceiling = 0;         // too large
  uint64_t sequential = 0;        // the level-load sweep, which is not kept
  uint64_t sequential_bytes = 0;
  uint64_t floor_kb = 0;
  bool no_memory = false;
};
RangeStats ReadRangeStats();

// Diagnostics (switch_io_report_files): the files read most since the previous call, by bytes, as one
// line ("name: reads, MB; ..."), at most 'top' of them. Empty when the cvar is off or nothing was read.
std::string TakeFilesReport(size_t top);

class HostPathFile : public File {
 public:
  HostPathFile(uint32_t file_access, HostPathEntry* entry,
               std::unique_ptr<rex::filesystem::FileHandle> file_handle);
  ~HostPathFile() override;

  void Destroy() override;

  X_STATUS ReadSync(std::span<uint8_t> buffer, size_t byte_offset, size_t* out_bytes_read) override;
  X_STATUS WriteSync(std::span<const uint8_t> buffer, size_t byte_offset,
                     size_t* out_bytes_written) override;
  X_STATUS SetLength(size_t length) override;
  X_STATUS Flush() override;

 private:


  bool FillWindow(size_t byte_offset, size_t requested, std::span<uint8_t> buffer,
                       size_t* out_bytes_read, X_STATUS* out_status);

  // Direct read split into chunks. See the comment on switch_io_chunk_mb in the .cpp.
  X_STATUS ReadDirect(std::span<uint8_t> buffer, size_t byte_offset, size_t* out_bytes_read);

  std::unique_ptr<rex::filesystem::FileHandle> file_handle_;

  std::vector<uint8_t> window_;
  size_t window_start_ = 0;
  size_t window_bytes_ = 0;
  size_t window_sz_ = 0;
  uint32_t window_short_ = 0;
  // Per-file audit: a window that does not hit turns itself off.
  uint32_t window_fills_ = 0;
  uint32_t window_hits_ = 0;
  bool window_active_ = false;
  bool window_counted_ = false;
  // File id inside the RAM cache (0 = this file is not cached). See
  // switch_io_cache_mb.
  uint32_t cache_id_ = 0;
};

}  // namespace rex::filesystem
