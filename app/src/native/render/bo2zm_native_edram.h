// SPDX-License-Identifier: GPL-3.0-only


#pragma once

#include <atomic>
#include <cstdint>
#include <unordered_map>

namespace bo2zm::native::edram {

inline constexpr uint32_t kLearnDead = 600;
inline constexpr uint32_t kMaxOrdinal = 63;

inline std::atomic<bool> g_enabled{false};
inline std::atomic<bool> g_off{false};

inline std::atomic<uint64_t> g_bytes_loaded{0}, g_bytes_stored{0}, g_bytes_not_stored{0};
inline std::atomic<uint64_t> g_discards{0}, g_mismatches{0}, g_dead_seen{0}, g_live_seen{0};

class Tracker {
 public:

  void Clear(uint64_t image) {
    State& s = images_[image];
    if (s.pending) {
      Learn& l = keys_[Key(image, s.pending_k)];
      if (l.dead < UINT32_MAX) ++l.dead;
      g_dead_seen.fetch_add(1, std::memory_order_relaxed);
    }
    s = State{};
  }

  bool Read(uint64_t image) {
    const auto it = images_.find(image);
    if (it == images_.end()) {
      return true;
    }
    State& s = it->second;
    bool ok = true;
    if (s.pending) {
      Learn& l = keys_[Key(image, s.pending_k)];
      l.dead = 0;
      g_live_seen.fetch_add(1, std::memory_order_relaxed);
      if (s.discarded) {
        l.banned = true;
        ok = false;
        g_mismatches.fetch_add(1, std::memory_order_relaxed);
        g_off.store(true, std::memory_order_relaxed);
      }
    }
    s.pending = false;
    s.discarded = false;
    return ok;
  }

  void Touch(uint64_t image) {
    const auto it = images_.find(image);
    if (it == images_.end()) {
      return;
    }
    if (it->second.pending) {
      keys_[Key(image, it->second.pending_k)].dead = 0;
    }
    it->second.pending = false;
    it->second.discarded = false;
  }

  bool ShouldDiscard(uint64_t image) const {
    if (!g_enabled.load(std::memory_order_relaxed) || g_off.load(std::memory_order_relaxed)) {
      return false;
    }
    const auto st = images_.find(image);
    const uint32_t k = st == images_.end() ? 0 : st->second.since_clear;
    if (k > kMaxOrdinal) {
      return false;
    }
    const auto l = keys_.find(Key(image, k));
    return l != keys_.end() && !l->second.banned && l->second.dead >= kLearnDead;
  }

  void PassEnded(uint64_t image, bool discarded) {
    State& s = images_[image];
    s.pending = true;
    s.pending_k = s.since_clear < kMaxOrdinal ? s.since_clear : kMaxOrdinal;
    if (s.since_clear < UINT32_MAX) ++s.since_clear;
    s.discarded = discarded;
    if (discarded) {
      g_discards.fetch_add(1, std::memory_order_relaxed);
    }
  }

  void Forget(uint64_t image) { images_.erase(image); }

 private:
  struct State {
    uint32_t since_clear = 0;
    bool pending = false;
    uint32_t pending_k = 0;
    bool discarded = false;
  };
  struct Learn {
    uint32_t dead = 0;
    bool banned = false;
  };
  static uint64_t Key(uint64_t image, uint32_t k) { return image ^ (uint64_t(k + 1) << 58); }
  std::unordered_map<uint64_t, State> images_;
  std::unordered_map<uint64_t, Learn> keys_;
};

inline Tracker& Get() {
  static Tracker tracker;
  return tracker;
}


inline constexpr uint32_t kStoreDiscardShift = 16;

inline uint32_t BytesPerTexel(uint32_t vk_format) {
  switch (vk_format) {
    case 91:
    case 97:
    case 103:
    case 130:
      return 8;
    default:
      return 4;
  }
}

}
