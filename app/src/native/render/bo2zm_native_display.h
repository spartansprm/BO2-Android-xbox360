// SPDX-License-Identifier: GPL-3.0-only


#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string_view>

#include "bo2zm_native_resolution.h"

namespace bo2zm::native::v042 {

inline constexpr uint32_t kGuestOutputWidth = 1280;
inline constexpr uint32_t kGuestOutputHeight = 720;
inline constexpr double kAspect16x9 = 16.0 / 9.0;


enum class Output : uint32_t { k720 = 0, k1080 = 1, kAuto = 2 };
constexpr const char* OutputKey(Output o) {
  return o == Output::k1080 ? "1920x1080" : o == Output::kAuto ? "auto" : "1280x720";
}
constexpr bool ParseOutput(std::string_view text, Output& out) {
  if (text == "1280x720" || text == "original") { out = Output::k720; return true; }
  if (text == "1920x1080" || text == "fullhd") { out = Output::k1080; return true; }
  if (text == "auto") { out = Output::kAuto; return true; }
  return false;
}


enum class Screen : uint32_t { kOriginal = 0, kStretch = 1, kUltrawide = 2 };
constexpr const char* ScreenKey(Screen s) {
  return s == Screen::kStretch ? "stretch" : s == Screen::kUltrawide ? "ultrawide" : "original";
}
constexpr bool ParseScreen(std::string_view text, Screen& out) {
  if (text == "original" || text == "16:9") { out = Screen::kOriginal; return true; }
  if (text == "stretch" || text == "fullscreen") { out = Screen::kStretch; return true; }
  if (text == "ultrawide") { out = Screen::kUltrawide; return true; }
  return false;
}


struct Display {
  uint32_t width = 0;
  uint32_t height = 0;
  constexpr bool Known() const { return width && height; }
  constexpr double Aspect() const { return Known() ? double(width) / double(height) : kAspect16x9; }
};
constexpr Display Landscape(uint32_t a, uint32_t b) {
  return (a && b) ? Display{std::max(a, b), std::min(a, b)} : Display{};
}


constexpr double UltrawideAspect(Display d) {
  const double a = d.Aspect();
  return (a > kAspect16x9 * 1.01) ? std::min(a, 32.0 / 9.0) : kAspect16x9;
}

constexpr double UltrawideFactor(Display d) { return UltrawideAspect(d) / kAspect16x9; }


constexpr uint32_t WidenedWidth(uint32_t width, Display d) {
  const double f = UltrawideFactor(d);
  if (f <= 1.0) return width;
  const uint32_t w = uint32_t(double(width) * f + 0.5);
  return std::min<uint32_t>(4096, (w + 1) & ~1u);
}


constexpr v041::Resolution SceneSize(v041::Resolution preset, Screen s, Display d) {
  if (s != Screen::kUltrawide) return preset;
  return {std::max(preset.width, WidenedWidth(preset.width, d)), preset.height};
}


constexpr v041::Resolution OutputSize(Output o, Screen s, Display d) {
  if (o == Output::k720) {
    return {s == Screen::kUltrawide ? WidenedWidth(kGuestOutputWidth, d) : kGuestOutputWidth, kGuestOutputHeight};
  }
  if (o == Output::k1080 || !d.Known()) return {s == Screen::kUltrawide ? WidenedWidth(1920, d) : 1920u, 1080};
  const uint32_t h = std::clamp<uint32_t>(d.height, 720, 1440);
  const double aspect = s == Screen::kOriginal ? kAspect16x9 : std::min(d.Aspect(), 32.0 / 9.0);
  uint32_t w = uint32_t(double(h) * aspect + 0.5);
  w = std::min<uint32_t>(4096, (w + 1) & ~1u);
  return {std::max<uint32_t>(w, kGuestOutputWidth), h};
}


constexpr v041::SceneScale OutputScale(v041::Resolution r) {
  return v041::SceneScale{v041::MakeAxis(r.width, kGuestOutputWidth), v041::MakeAxis(r.height, kGuestOutputHeight)};
}


struct PresentAspect {
  uint32_t x = kGuestOutputWidth;
  uint32_t y = kGuestOutputHeight;
};
constexpr PresentAspect PresentAspectFor(Screen s, Display surface, bool ultrawide_confirmed) {
  if (!surface.Known()) return {};
  if (s == Screen::kStretch || (s == Screen::kUltrawide && ultrawide_confirmed)) return {surface.width, surface.height};
  return {};
}


inline int FindAspectTriple(const float* floats, int count) {
  int found = -1;
  for (int i = 0; i + 2 < count; ++i) {
    const bool window = std::fabs(floats[i] - float(kAspect16x9)) < 0.005f;
    const bool scene = std::fabs(floats[i + 1] - 16.0f / 11.0f) < 0.005f;
    const bool display = std::fabs(floats[i + 2] - 1.0f) < 0.001f;
    if (window && scene && display) {
      if (found >= 0) return -1;
      found = i;
    }
  }
  return found;
}

struct AspectTriple {
  float window, scene_pixel, display_pixel;
};
constexpr AspectTriple AspectFor(double factor) {
  return AspectTriple{float(kAspect16x9 * factor), float(16.0 / 11.0 * factor), float(factor)};
}


inline constexpr int kPlacementFloats = 31;


inline bool AdjustPlacement(float* p, double wanted, double known) {
  const float sx = p[0], sy = p[1];
  if (!(sx > 0.0f) || !(sy > 0.0f) || !(p[16] > 0.0f) || !(p[17] > 0.0f) || !std::isfinite(sx) || !std::isfinite(sy))
    return false;
  const double current = double(sy) / double(sx);
  const auto near = [](double a, double b) { return std::fabs(a / b - 1.0) < 1e-3; };
  if (!near(current, 1.0) && !near(current, known)) return false;
  if (near(current, wanted)) return true;
  const double k = current / wanted;
  for (int i : {0, 2}) p[i] = float(double(p[i]) * k);
  for (int i : {4, 6, 8, 10, 12}) p[i] = float(double(p[i]) / k);
  return true;
}

inline double PlacementFactor(const float* p) {
  return (p[0] > 0.0f && p[1] > 0.0f) ? double(p[1]) / double(p[0]) : 0.0;
}


struct Fov {
  double aspect = 0, horizontal_deg = 0, vertical_deg = 0;
};
inline Fov FovFromTangents(float tan_x, float tan_y) {
  Fov f;
  if (tan_x > 0.0f && tan_y > 0.0f && std::isfinite(tan_x) && std::isfinite(tan_y)) {
    constexpr double kDeg = 57.29577951308232;
    f.aspect = double(tan_x) / double(tan_y);
    f.horizontal_deg = 2.0 * std::atan(double(tan_x)) * kDeg;
    f.vertical_deg = 2.0 * std::atan(double(tan_y)) * kDeg;
  }
  return f;
}


inline int FindTangentPair(const float* f, int count, double aspect_a, double aspect_b) {
  int found = -1;
  for (int i = 0; i + 1 < count; ++i) {
    const float tx = f[i], ty = f[i + 1];
    if (!(tx > 0.0f && ty > 0.04f && ty < 11.5f) || !std::isfinite(tx) || !std::isfinite(ty)) continue;
    const double r = double(tx) / double(ty);
    if (std::fabs(r / aspect_a - 1.0) < 0.01 || std::fabs(r / aspect_b - 1.0) < 0.01) {
      if (found >= 0) return -1;
      found = i;
    }
  }
  return found;
}

constexpr bool AspectMatches(double measured, double wanted, double tolerance = 0.01) {
  return measured > 0 && wanted > 0 && (measured / wanted - 1.0) < tolerance && (wanted / measured - 1.0) < tolerance;
}

}
