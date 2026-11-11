// SPDX-License-Identifier: GPL-3.0-only


#pragma once

#include <atomic>
#include <cstdint>

namespace bo2zm::native::v055 {


constexpr uint32_t UsefulRows(uint32_t bottom, bool to_tile) {
  return to_tile ? (bottom + 15u) & ~15u : (bottom + 63u) & ~63u;
}


inline constexpr uint32_t kAspectColor = 0x1;
inline constexpr uint32_t kAspectDepth = 0x2;
inline constexpr uint32_t kAspectStencil = 0x4;


constexpr bool ClearsWholeAttachment(uint32_t aspects, bool color_attachment, bool format_has_stencil) {
  if (color_attachment) {
    return aspects == kAspectColor;
  }
  return aspects == (kAspectDepth | (format_has_stencil ? kAspectStencil : 0u));
}


inline constexpr uint32_t kLoadDepthStoreNone = 1u << 24;


inline constexpr int32_t kStoreOpNone = 1000301000;


constexpr bool WritesDepthStencil(uint32_t depth_control) {
  return (depth_control & 0x1u) != 0 || (depth_control & 0x6u) == 0x6u;
}


struct ReadOnlyLearn {
  uint32_t clean = 0;
  uint32_t misses = 0;
  bool banned = false;
};
inline constexpr uint32_t kLearnClean = 300;
inline constexpr uint32_t kMaxMisses = 4;


constexpr uint64_t LearnKey(uint64_t pass_key, uint32_t vs, uint32_t ps) {
  uint64_t h = pass_key ^ (uint64_t(vs) * 0x9E3779B97F4A7C15ull) ^ (uint64_t(ps) * 0xC2B2AE3D27D4EB4Full);
  h ^= h >> 29;
  return h * 0xBF58476D1CE4E5B9ull;
}

constexpr bool OpenReadOnly(const ReadOnlyLearn& l) { return !l.banned && l.clean >= kLearnClean; }


constexpr void PassClosed(ReadOnlyLearn& l, bool written) {
  if (written) {
    l.clean = 0;
  } else if (l.clean < UINT32_MAX) {
    ++l.clean;
  }
}


constexpr bool Missed(ReadOnlyLearn& l) {
  l.clean = 0;
  if (++l.misses >= kMaxMisses) {
    l.banned = true;
  }
  return l.banned;
}


inline std::atomic<uint64_t> g_converted_no_load{0};
inline std::atomic<uint64_t> g_converted_texels{0};
inline std::atomic<uint64_t> g_separate_clears_as_load{0};
inline std::atomic<uint64_t> g_separate_clear_texels{0};
inline std::atomic<uint64_t> g_readonly_passes{0};
inline std::atomic<uint64_t> g_readonly_texels{0};
inline std::atomic<uint64_t> g_readonly_candidates{0};
inline std::atomic<uint64_t> g_readonly_misses{0};
inline std::atomic<uint64_t> g_readonly_banned{0};
inline std::atomic<int> g_readonly_state{0};

}
