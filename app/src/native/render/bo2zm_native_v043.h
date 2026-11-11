// SPDX-License-Identifier: GPL-3.0-only


#pragma once

#include <algorithm>
#include <array>
#include <cstdint>

namespace bo2zm::native::v043 {


inline constexpr uint32_t kAspectColor = 0x1;
inline constexpr uint32_t kAspectDepth = 0x2;
inline constexpr uint32_t kAspectStencil = 0x4;

inline constexpr uint32_t kSlots = 5;
inline constexpr uint32_t kSlotDepth = 4;

struct Rect {
  int32_t x = 0;
  int32_t y = 0;
  uint32_t width = 0;
  uint32_t height = 0;
};


constexpr uint32_t FullAspects(uint32_t slot) {
  return slot == kSlotDepth ? (kAspectDepth | kAspectStencil) : kAspectColor;
}


constexpr bool AspectsFitSlot(uint32_t aspects, uint32_t slot) {
  if (!aspects) {
    return false;
  }
  return slot == kSlotDepth ? (aspects & ~(kAspectDepth | kAspectStencil)) == 0 : aspects == kAspectColor;
}


constexpr bool RectInside(const Rect& r, uint32_t width, uint32_t height) {
  return r.width && r.height && r.x >= 0 && r.y >= 0 && uint64_t(uint32_t(r.x)) + r.width <= width &&
         uint64_t(uint32_t(r.y)) + r.height <= height;
}


constexpr bool RectWhole(const Rect& r, uint32_t width, uint32_t height) {
  return r.x == 0 && r.y == 0 && r.width == width && r.height == height;
}


constexpr bool WholeImageClear(const Rect& r, uint32_t aspects, uint32_t slot, uint32_t width, uint32_t height) {
  return RectWhole(r, width, height) && aspects == FullAspects(slot);
}


inline uint32_t ColorAttachmentIndex(const uint32_t formats[kSlots], uint32_t slot) {
  uint32_t n = 0;
  for (uint32_t i = 0; i < slot && i < 4; ++i) {
    if (formats[i]) {
      ++n;
    }
  }
  return n;
}


struct RectRequest {
  bool active = false;
  uint32_t aspects = 0;
  Rect area{};
};

struct PassPlan {
  uint32_t area_height = 0;
  std::array<bool, kSlots> fuse{};
  std::array<bool, kSlots> in_pass{};
};


inline PassPlan PlanRectClears(uint32_t width, uint32_t height, uint32_t useful_height, bool whole_fused_any,
                               const std::array<RectRequest, kSlots>& requests) {
  PassPlan plan;
  uint32_t area = whole_fused_any ? height : std::min(std::max<uint32_t>(useful_height, 1), height);
  if (!whole_fused_any) {
    for (uint32_t i = 0; i < kSlots; ++i) {
      const RectRequest& r = requests[i];
      if (r.active && RectInside(r.area, width, height)) {
        area = std::max<uint32_t>(area, uint32_t(r.area.y) + r.area.height);
      }
    }
  }
  plan.area_height = area;
  for (uint32_t i = 0; i < kSlots; ++i) {
    const RectRequest& r = requests[i];
    if (!r.active || !AspectsFitSlot(r.aspects, i) || !RectInside(r.area, width, area)) {
      continue;
    }
    if (r.area.x == 0 && r.area.y == 0 && r.area.width == width && r.area.height == area &&
        r.aspects == FullAspects(i)) {
      plan.fuse[i] = true;
    } else {
      plan.in_pass[i] = true;
    }
  }
  return plan;
}

}
