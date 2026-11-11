// SPDX-License-Identifier: GPL-3.0-only
//
// Exact-range read cache for read-only containers (used by StfsContainerFile).
//
// Why: on Android the game reads base.ipak through SAF descriptors (StfsContainerFile::ReadSync ->

// took 8-13 ms each. host_path_file.cpp has a range cache for exactly this, but a container never goes through
// that class, so the cache showed 0 hits / 0 entries.
//
// Same rules as that cache, deliberately:
//   * the key is the exact range (owner, offset, length): no block amplification;
//   * only reads inside [floor, ceiling] are kept (the game repeats reads of 64 KB or more in a loop);
//   * a purely sequential read (offset == end of the previous read of that owner) is the level-load sweep: it is
//     not kept, it would only fill the LRU;
//   * the data is read-only, so nothing ever has to be invalidated;
//   * if the host runs out of memory the cache turns itself off for the session.
//
// Thread safety: the mutex is held only for the table lookup/update. The copy to the caller's buffer happens
// outside it, from a shared_ptr that keeps the bytes alive even if the entry is evicted meanwhile.

#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <list>
#include <memory>
#include <mutex>
#include <new>
#include <unordered_map>

namespace rex::filesystem::detail {

class RangeReadCache {
 public:
  struct Limits {
    uint64_t cap_bytes = 0;      // 0 = cache off
    uint64_t floor_bytes = 0;    // smaller reads are not kept
    uint64_t ceiling_bytes = 0;  // larger reads are not kept
  };
  struct Stats {
    uint64_t hits = 0;
    uint64_t misses = 0;
    uint64_t bytes_ram = 0;   // delivered from the cache
    uint64_t bytes_disk = 0;  // read from the container on a miss
    uint64_t entries = 0;
    uint64_t live_bytes = 0;
    uint64_t evictions = 0;
    uint64_t low_floor = 0;
    uint64_t over_ceiling = 0;
    uint64_t sequential = 0;
    uint64_t sequential_bytes = 0;
    bool no_memory = false;
  };
  enum class Result { kHit, kMiss, kBypass };
  static constexpr uint64_t kSweepMin = uint64_t(4) << 20;

  // Looks the range up. kHit: 'dst' holds the bytes. kMiss: the caller reads from the container and then calls
  // Store(). kBypass: not eligible, the caller reads from the container and must not call Store().
  Result Serve(const void* owner, uint64_t offset, uint8_t* dst, size_t length, const Limits& limits) {
    if (!limits.cap_bytes) {
      return Result::kBypass;
    }
    std::shared_ptr<uint8_t[]> data;
    {
      std::lock_guard lock(mutex_);
      if (no_memory_) {
        return Result::kBypass;
      }
      // The sweep test looks at every read of the owner, kept or not. As in host_path_file.cpp it only counts
      // as the level-load sweep if the read is also 4 MB or more: smaller contiguous reads are the
      // pairs the game repeats in a loop, and those must be kept.
      uint64_t& last_end = last_end_[owner];
      const bool sequential = offset != 0 && offset == last_end && length >= kSweepMin;
      last_end = offset + length;
      if (length < limits.floor_bytes) {
        ++low_floor_;
        return Result::kBypass;
      }
      if (length > limits.ceiling_bytes || length > limits.cap_bytes / 2) {
        ++over_ceiling_;
        return Result::kBypass;
      }
      const Key key{owner, offset, length};
      if (const auto it = map_.find(key); it != map_.end()) {
        lru_.splice(lru_.begin(), lru_, it->second.lru);  // most recently used
        data = it->second.data;
        ++hits_;
        bytes_ram_ += length;
      } else if (sequential) {
        ++sequential_;
        sequential_bytes_ += length;
        return Result::kBypass;
      } else {
        ++misses_;
        bytes_disk_ += length;
        return Result::kMiss;
      }
    }
    std::memcpy(dst, data.get(), length);
    return Result::kHit;
  }

  // Keeps the bytes of a range that was just read from the container after a kMiss.
  void Store(const void* owner, uint64_t offset, const uint8_t* src, size_t length, const Limits& limits) {
    if (!limits.cap_bytes || length > limits.cap_bytes / 2) {
      return;
    }
    std::shared_ptr<uint8_t[]> copy;
    try {
      copy = std::shared_ptr<uint8_t[]>(new uint8_t[length]);  // outside the lock: it can be large
    } catch (const std::bad_alloc&) {
      std::lock_guard lock(mutex_);
      no_memory_ = true;
      DropAllLocked();
      return;
    }
    std::memcpy(copy.get(), src, length);
    std::lock_guard lock(mutex_);
    if (no_memory_) {
      return;
    }
    const Key key{owner, offset, length};
    if (map_.find(key) != map_.end()) {
      return;  // another thread stored the same range first
    }
    while (!lru_.empty() && live_bytes_ + length > limits.cap_bytes) {
      EvictOldestLocked();
    }
    lru_.push_front(key);
    map_.emplace(key, Entry{std::move(copy), lru_.begin()});
    live_bytes_ += length;
  }

  Stats GetStats() const {
    std::lock_guard lock(mutex_);
    Stats s;
    s.hits = hits_;
    s.misses = misses_;
    s.bytes_ram = bytes_ram_;
    s.bytes_disk = bytes_disk_;
    s.entries = map_.size();
    s.live_bytes = live_bytes_;
    s.evictions = evictions_;
    s.low_floor = low_floor_;
    s.over_ceiling = over_ceiling_;
    s.sequential = sequential_;
    s.sequential_bytes = sequential_bytes_;
    s.no_memory = no_memory_;
    return s;
  }

 private:
  struct Key {
    const void* owner;
    uint64_t offset;
    uint64_t length;
    bool operator==(const Key& o) const {
      return owner == o.owner && offset == o.offset && length == o.length;
    }
  };
  struct KeyHash {
    size_t operator()(const Key& k) const {
      uint64_t h = uint64_t(reinterpret_cast<uintptr_t>(k.owner)) * 0x9E3779B97F4A7C15ull;
      h ^= (k.offset + 0x9E3779B97F4A7C15ull + (h << 6) + (h >> 2));
      h ^= (k.length * 0xBF58476D1CE4E5B9ull) + (h << 6) + (h >> 2);
      return size_t(h);
    }
  };
  struct Entry {
    std::shared_ptr<uint8_t[]> data;
    std::list<Key>::iterator lru;
  };

  void EvictOldestLocked() {
    const Key key = lru_.back();
    lru_.pop_back();
    if (const auto it = map_.find(key); it != map_.end()) {
      live_bytes_ -= key.length;
      map_.erase(it);
      ++evictions_;
    }
  }
  void DropAllLocked() {
    map_.clear();
    lru_.clear();
    live_bytes_ = 0;
  }

  mutable std::mutex mutex_;
  std::unordered_map<Key, Entry, KeyHash> map_;
  std::list<Key> lru_;
  std::unordered_map<const void*, uint64_t> last_end_;
  uint64_t live_bytes_ = 0;
  uint64_t hits_ = 0, misses_ = 0, bytes_ram_ = 0, bytes_disk_ = 0, evictions_ = 0;
  uint64_t low_floor_ = 0, over_ceiling_ = 0, sequential_ = 0, sequential_bytes_ = 0;
  bool no_memory_ = false;
};

}  // namespace rex::filesystem::detail
