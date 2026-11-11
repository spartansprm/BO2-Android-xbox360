#pragma once

#include <atomic>
#include <cstdint>
#include <cstring>

#include <rex/platform.h>

namespace bo2zm::native::microcode {

constexpr uint32_t kBitsSlot = 12;
constexpr uint32_t kSlots = uint32_t(1) << kBitsSlot;

struct SlotEntry {
  std::atomic<uint32_t> start{0};
  std::atomic<uint32_t> end{0};
};
inline SlotEntry g_slots[kSlots];
inline std::atomic<uint32_t> g_global{0};


inline std::atomic<uint64_t> g_patches_in_room{0};
inline std::atomic<uint64_t> g_patches_in_copy{0};
inline std::atomic<uint64_t> g_creations{0};

inline std::atomic<uint64_t> g_patches_others{0};
inline std::atomic<uint32_t> g_other_return{0};
inline std::atomic<uint32_t> g_other_target{0};

#if defined(BO2ZM_NATIVE_SHADER_LIBRARY)


inline constexpr bool kCreationsWatched = false;
#else
inline constexpr bool kCreationsWatched = true;
#endif


inline uint32_t Physical(uint32_t address) {
  return (address & 0x1FFFFFFFu) + ((((address >> 20) & 0xFFFu) + 0x200u) & 0x1000u);
}


inline uint32_t SlotOf(uint32_t physical) {
  return ((physical >> 2) * 0x9E3779B1u) >> (32 - kBitsSlot);
}


inline uint32_t ReadGuest32(const uint8_t* base, uint32_t address) {
#if REX_PLATFORM_WIN32
  const uint32_t displacement = address >= 0xE0000000u ? 0x1000u : 0u;
#else
  const uint32_t displacement = 0u;
#endif
  uint32_t value;
  std::memcpy(&value, base + address + displacement, 4);
  return __builtin_bswap32(value);
}


inline void Count(std::atomic<uint64_t>& count) {
  count.store(count.load(std::memory_order_relaxed) + 1, std::memory_order_relaxed);
}


inline void BeginWrite(uint32_t slot) {
  g_slots[slot].start.fetch_add(1, std::memory_order_relaxed);

  std::atomic_thread_fence(std::memory_order_seq_cst);
}


inline void FinishWrite(uint32_t slot) {
  g_slots[slot].end.fetch_add(1, std::memory_order_release);
  g_global.fetch_add(1, std::memory_order_release);
}


inline void WarnCreation(const uint8_t* base, uint32_t object, bool vertices) {
  if (!object) {
    return;
  }
  const uint32_t address = ReadGuest32(base, object + (vertices ? 40u : 12u));
  if (!address) {
    return;
  }
  const uint32_t slot = SlotOf(Physical(address) & ~uint32_t(3));
  BeginWrite(slot);
  Count(g_creations);
  FinishWrite(slot);
}


inline uint32_t GlobalRelaxed() {
  return g_global.load(std::memory_order_relaxed);
}


inline uint32_t StartAfterGlobal(uint32_t slot) {
  std::atomic_thread_fence(std::memory_order_acquire);
  return g_slots[slot].start.load(std::memory_order_relaxed);
}

struct Readback {
  uint32_t global = 0;
  uint32_t start = 0;
  uint32_t end = 0;
};


inline Readback BeforeRead(uint32_t slot) {
  Readback l;
  l.global = g_global.load(std::memory_order_acquire);
  l.start = g_slots[slot].start.load(std::memory_order_acquire);
  l.end = g_slots[slot].end.load(std::memory_order_acquire);
  return l;
}


inline bool AfterRead(uint32_t slot, const Readback& l) {
  std::atomic_thread_fence(std::memory_order_acquire);
  return l.start == l.end && g_slots[slot].start.load(std::memory_order_relaxed) == l.start;
}

}
