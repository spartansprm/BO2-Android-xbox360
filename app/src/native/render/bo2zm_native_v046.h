// SPDX-License-Identifier: GPL-3.0-only


#pragma once

#include <atomic>
#include <cstdint>

namespace bo2zm::native::v046 {


inline constexpr uint32_t kAspectDepth = 0x2;
inline constexpr uint32_t kAspectStencil = 0x4;

inline constexpr uint32_t kLoadDepthClearOnly = 1u << 12;
inline constexpr uint32_t kLoadStencilClearOnly = 1u << 13;

struct AspectFuse {
  bool fuse = false;
  uint32_t bits = 0;
};


inline AspectFuse PlanDepthAspectFuse(int32_t x, int32_t y, uint32_t w, uint32_t h, uint32_t aspects,
                                      uint32_t area_width, uint32_t area_height, bool format_has_stencil) {
  AspectFuse r;
  if (x != 0 || y != 0 || w != area_width || h != area_height || !format_has_stencil) {
    return r;
  }
  if (aspects == kAspectDepth) {
    r.fuse = true;
    r.bits = kLoadDepthClearOnly;
  } else if (aspects == kAspectStencil) {
    r.fuse = true;
    r.bits = kLoadStencilClearOnly;
  }
  return r;
}

enum class Load : uint32_t { kLoad, kClear, kDontCare };

struct DepthLoads {
  Load depth, stencil;
};
inline DepthLoads DepthSlotLoads(Load base, uint32_t mode_load) {
  DepthLoads r{base, base};
  if (mode_load & kLoadDepthClearOnly) r.depth = Load::kClear;
  if (mode_load & kLoadStencilClearOnly) r.stencil = Load::kClear;
  return r;
}


inline constexpr uint32_t kLazyLearnCopies = 600;
struct UnreadLearner {
  uint32_t reads_since_copy = 0;
  uint32_t unread_streak = 0;
  bool seen_copy = false;

  bool OnCopy() {
    if (seen_copy) {
      unread_streak = reads_since_copy == 0 ? unread_streak + 1 : 0;
    }
    seen_copy = true;
    reads_since_copy = 0;
    return unread_streak >= kLazyLearnCopies;
  }
  void OnRead() {
    ++reads_since_copy;
    unread_streak = 0;
  }
};


struct FrontLearner {
  uint32_t resolves_since_swap = 0;
  uint32_t streak = 0;
  void OnResolve() { ++resolves_since_swap; }
  void OnSwap() {
    streak = resolves_since_swap > 0 ? streak + 1 : 0;
    resolves_since_swap = 0;
  }
  void OnSampled() { streak = 0; }
  bool MayDrop() const { return streak >= kLazyLearnCopies; }
};


struct CopyFacts {
  uint64_t source_image = 0;
  uint64_t source_epoch = 0;
  int32_t src_x = 0, src_y = 0;
  uint32_t width = 0, height = 0;
  uint32_t format = 0;
  uint32_t dest_width = 0, dest_height = 0;
  bool whole_dest = false;
  uint32_t dest_address = 0;
};

inline bool SameResolve(const CopyFacts& prev, const CopyFacts& next) {
  return prev.source_image != 0 && prev.source_image == next.source_image &&
         prev.source_epoch == next.source_epoch && prev.src_x == next.src_x && prev.src_y == next.src_y &&
         prev.width == next.width && prev.height == next.height && prev.format == next.format &&
         prev.whole_dest && next.whole_dest && prev.dest_width == next.dest_width &&
         prev.dest_height == next.dest_height && prev.dest_address != next.dest_address;
}


enum ResolveDiff : uint32_t {
  kDiffNoPrevious = 1u << 0,
  kDiffSource = 1u << 1,
  kDiffEpoch = 1u << 2,
  kDiffRect = 1u << 3,
  kDiffSize = 1u << 4,
  kDiffFormat = 1u << 5,
  kDiffDestSize = 1u << 6,
  kDiffNotWhole = 1u << 7,
  kDiffSameAddress = 1u << 8,
};
inline constexpr uint32_t kResolveDiffReasons = 9;
inline uint32_t ResolveDifference(const CopyFacts& prev, const CopyFacts& next) {
  if (prev.source_image == 0) return kDiffNoPrevious;
  uint32_t d = 0;
  if (prev.source_image != next.source_image) d |= kDiffSource;
  if (prev.source_epoch != next.source_epoch) d |= kDiffEpoch;
  if (prev.src_x != next.src_x || prev.src_y != next.src_y) d |= kDiffRect;
  if (prev.width != next.width || prev.height != next.height) d |= kDiffSize;
  if (prev.format != next.format) d |= kDiffFormat;
  if (prev.dest_width != next.dest_width || prev.dest_height != next.dest_height) d |= kDiffDestSize;
  if (!prev.whole_dest || !next.whole_dest) d |= kDiffNotWhole;
  if (prev.dest_address == next.dest_address) d |= kDiffSameAddress;
  return d;
}
inline std::atomic<uint64_t> g_alias_diff[kResolveDiffReasons] = {};


enum ClearMiss : uint32_t {
  kMissRectOrigin,
  kMissRectWidth,
  kMissRectShort,
  kMissRectAspects,
  kMissWholeSize,
  kMissOther,
  kClearMissReasons,
};
inline std::atomic<uint64_t> g_clear_miss[kClearMissReasons] = {};

inline std::atomic<uint64_t> g_clear_miss_sample[kClearMissReasons] = {};


inline constexpr uint64_t kFitLearnDraws = 2000;
struct DrawnExtent {
  uint32_t right = 0, bottom = 0;
  uint64_t draws = 0;
  bool banned = false;
};
inline bool FitAreaToClear(const DrawnExtent& e, int32_t x, int32_t y, uint32_t w, uint32_t h, uint32_t pass_width,
                           uint32_t area_height) {
  return !e.banned && e.draws >= kFitLearnDraws && x == 0 && y == 0 && w > 0 && h > 0 && w <= pass_width &&
         h <= area_height && (w < pass_width || h < area_height) && e.right <= w && e.bottom <= h;
}
inline std::atomic<bool> g_fit_enabled{false};
inline std::atomic<uint64_t> g_fit_passes{0}, g_fit_fused{0}, g_fit_clipped{0}, g_fit_texels_saved{0};


inline std::atomic<uint64_t> g_fused_depth_only{0}, g_fused_stencil_only{0};
inline std::atomic<uint64_t> g_fronts_dropped{0}, g_fronts_kept{0};
inline std::atomic<uint64_t> g_lazy_deferred{0}, g_lazy_recorded{0}, g_lazy_dropped{0}, g_lazy_mismatch{0};
inline std::atomic<uint64_t> g_alias_served{0}, g_alias_swapped{0}, g_alias_materialized{0}, g_alias_dropped{0}, g_alias_rejected{0},
    g_alias_mismatch{0};
inline std::atomic<uint64_t> g_pixels_not_copied{0};

}
