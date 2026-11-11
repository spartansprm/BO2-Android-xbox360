#pragma once

#include <algorithm>
#include <atomic>
#include <cstdint>

namespace bo2zm::page_writes {


extern std::atomic<bool> g_enabled;
extern std::atomic<uint32_t> g_epoch;
extern std::atomic<uint32_t> g_stamps[1u << 17];


extern std::atomic<uint32_t> g_regions[1u << 13];
extern std::atomic<bool> g_use_regions;


[[gnu::noinline]] void MarkPhysical(uint32_t physical, uint32_t bytes);


inline void MarkGuest(uint32_t guest, uint32_t bytes) {
  if (guest >= 0xA0000000u && g_enabled.load(std::memory_order_relaxed)) [[unlikely]] {
    MarkPhysical((guest & 0x1FFFFFFFu) + (guest >= 0xE0000000u ? 0x1000u : 0u), bytes);
  }
}


void MarkVolatile(uint32_t physical, uint32_t bytes);


inline bool Unchanged(uint32_t physical, uint32_t bytes, uint32_t since) {
  if (bytes == 0) {
    return true;
  }
  const uint32_t first = (physical & 0x1FFFFFFFu) >> 12;
  const uint32_t last = std::min<uint32_t>(((physical & 0x1FFFFFFFu) + bytes - 1) >> 12, (1u << 17) - 1);
  if (g_use_regions.load(std::memory_order_relaxed)) {
    for (uint32_t r = first >> 4; r <= last >> 4; ++r) {
      if (g_regions[r].load(std::memory_order_acquire) < since) {
        continue;
      }
      const uint32_t hi = std::min(last, (r << 4) + 15);
      for (uint32_t p = std::max(first, r << 4); p <= hi; ++p) {
        if (g_stamps[p].load(std::memory_order_acquire) >= since) {
          return false;
        }
      }
    }
    return true;
  }
  for (uint32_t p = first; p <= last; ++p) {
    if (g_stamps[p].load(std::memory_order_acquire) >= since) {
      return false;
    }
  }
  return true;
}

inline uint32_t Epoch() { return g_epoch.load(std::memory_order_relaxed); }
void NextEpoch();
void Enable(bool on);

}
