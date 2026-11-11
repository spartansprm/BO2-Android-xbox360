// SPDX-License-Identifier: GPL-3.0-only


#pragma once

#include <algorithm>
#include <cstdint>
#include <numeric>
#include <string_view>

namespace bo2zm::native::v041 {


inline constexpr uint32_t kGuestSceneWidth = 880;
inline constexpr uint32_t kGuestSceneHeight = 720;

struct Resolution {
  uint32_t width = kGuestSceneWidth;
  uint32_t height = kGuestSceneHeight;
};


struct Preset {
  const char* key;
  Resolution resolution;
};
inline constexpr Preset kPresets[] = {
    {"880x720", {880, 720}},
    {"1280x720", {1280, 720}},
    {"1920x1080", {1920, 1080}},
};


constexpr bool ParsePreset(std::string_view text, Resolution& out) {
  if (text == "original") text = "880x720";
  if (text == "hd") text = "1280x720";
  if (text == "fullhd") text = "1920x1080";
  for (const Preset& p : kPresets) {
    if (text == p.key) {
      out = p.resolution;
      return true;
    }
  }
  return false;
}


struct AxisScale {
  uint32_t num = 1;
  uint32_t den = 1;
  constexpr bool Identity() const { return num == den; }
};

struct SceneScale {
  AxisScale x;
  AxisScale y;
  constexpr bool Identity() const { return x.Identity() && y.Identity(); }
};

constexpr AxisScale MakeAxis(uint64_t num, uint64_t den) {
  const uint64_t g = std::gcd(num, den);
  return AxisScale{uint32_t(num / g), uint32_t(den / g)};
}


constexpr SceneScale MakeSceneScale(Resolution r, uint32_t percent) {
  percent = std::clamp<uint32_t>(percent, 50, 100);
  return SceneScale{MakeAxis(uint64_t(r.width) * percent, uint64_t(kGuestSceneWidth) * 100),
                    MakeAxis(uint64_t(r.height) * percent, uint64_t(kGuestSceneHeight) * 100)};
}


constexpr uint32_t ScaledSide(uint32_t side, AxisScale a) {
  return std::max<uint32_t>(16, uint32_t((uint64_t(side) * a.num + a.den - 1) / a.den));
}


constexpr Resolution SceneHost(const SceneScale& s) {
  return Resolution{uint32_t((uint64_t(kGuestSceneWidth) * s.x.num + s.x.den / 2) / s.x.den),
                    uint32_t((uint64_t(kGuestSceneHeight) * s.y.num + s.y.den / 2) / s.y.den)};
}


constexpr float PassScale(uint32_t host, uint32_t guest) {
  return (guest && guest != host) ? float(host) / float(guest) : 1.0f;
}

}
