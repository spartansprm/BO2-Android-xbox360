// SPDX-License-Identifier: GPL-3.0-only


#pragma once

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <string>

#include <rex/cvar.h>
#include <rex/ui/present_stats.h>

#include "bo2zm_native_display.h"

namespace bo2zm::display {


inline std::atomic<uint32_t> g_output{0};
inline std::atomic<uint32_t> g_screen{0};
inline std::atomic<uint32_t> g_display_width{0};
inline std::atomic<uint32_t> g_display_height{0};
inline std::atomic<uint32_t> g_output_width{1280};
inline std::atomic<uint32_t> g_output_height{720};
inline std::atomic<uint32_t> g_scene_requested_width{880};
inline std::atomic<uint32_t> g_scene_requested_height{720};


struct Extent {
  std::atomic<uint32_t> width{0}, height{0};
  void Set(uint32_t w, uint32_t h) {
    width.store(w, std::memory_order_relaxed);
    height.store(h, std::memory_order_relaxed);
  }
};

inline constexpr uint32_t kClassNone = 0, kClassScene = 1, kClassComposite = 2, kClasses = 3;
inline Extent g_scene_color;
inline Extent g_scene_depth;
inline Extent g_scene_texture;
inline Extent g_composite_color;
inline Extent g_composite_depth;
inline Extent g_front_texture;
inline Extent g_front_painted;
inline std::atomic<uint32_t> g_present_aspect_x{1280};
inline std::atomic<uint32_t> g_present_aspect_y{720};
inline std::atomic<uint64_t> g_swaps_painted{0};

inline std::atomic<uint64_t> g_pass_texels[kClasses] = {};
inline std::atomic<uint64_t> g_passes[kClasses] = {};


inline std::atomic<uint64_t> g_err_scaled_resolve_rejected{0};
inline std::atomic<uint64_t> g_err_front_size_mismatch{0};
inline std::atomic<uint64_t> g_err_present_failed{0};
inline std::atomic<uint64_t> g_err_lazy_front_fallback{0};
inline std::atomic<uint64_t> g_err_target_create{0};


enum class Ultrawide : uint32_t {
  kOff = 0,
  kSearching = 1,
  kApplied = 2,
  kConfirmed = 3,
  kFallback = 4,
};
inline constexpr const char* UltrawideName(Ultrawide u) {
  switch (u) {
    case Ultrawide::kOff: return "off";
    case Ultrawide::kSearching: return "searching";
    case Ultrawide::kApplied: return "applied";
    case Ultrawide::kConfirmed: return "confirmed";
    case Ultrawide::kFallback: return "fallback";
  }
  return "unknown";
}
inline std::atomic<uint32_t> g_ultrawide{0};
inline std::atomic<const char*> g_ultrawide_reason{""};
inline std::atomic<uint64_t> g_target_aspect_micro{1777778};
inline std::atomic<uint32_t> g_vidconfig_address{0};
inline std::atomic<uint32_t> g_vidconfig_window_bits{0}, g_vidconfig_scene_bits{0}, g_vidconfig_display_bits{0};
inline std::atomic<uint64_t> g_vidconfig_writes{0};
inline std::atomic<uint64_t> g_placements_adjusted{0};
inline std::atomic<uint64_t> g_placement_factor_micro{0};
inline std::atomic<uint32_t> g_refdef_tangent_offset{0};
inline std::atomic<uint32_t> g_tan_x_bits{0}, g_tan_y_bits{0};
inline std::atomic<uint32_t> g_refdef_width{0}, g_refdef_height{0};
inline std::atomic<uint64_t> g_fov_samples{0};
inline std::atomic<uint64_t> g_lui_aspect_micro{0};
inline std::atomic<uint64_t> g_lui_samples{0};

inline bool UltrawideConfirmed() { return g_ultrawide.load(std::memory_order_acquire) == uint32_t(Ultrawide::kConfirmed); }


inline void LoadSettingsOnce() {
  static std::once_flag once;
  std::call_once(once, [] {
    namespace v = bo2zm::native::v042;
    v::Output output = v::Output::k720;
    v::Screen screen = v::Screen::kOriginal;
    v::ParseOutput(rex::cvar::GetFlagByName("bo2zm_native_output_resolution"), output);
    v::ParseScreen(rex::cvar::GetFlagByName("bo2zm_native_screen_mode"), screen);
    const auto number = [](const char* name) {
      const std::string text = rex::cvar::GetFlagByName(name);
      const long value = std::strtol(text.c_str(), nullptr, 10);
      return uint32_t(value > 0 && value <= 16384 ? value : 0);
    };
    const v::Display display = v::Landscape(number("bo2zm_native_display_width"), number("bo2zm_native_display_height"));
    g_output.store(uint32_t(output), std::memory_order_relaxed);
    g_screen.store(uint32_t(screen), std::memory_order_relaxed);
    g_display_width.store(display.width, std::memory_order_relaxed);
    g_display_height.store(display.height, std::memory_order_relaxed);
    g_target_aspect_micro.store(uint64_t(v::UltrawideAspect(display) * 1e6 + 0.5), std::memory_order_relaxed);
  });
}
inline bo2zm::native::v042::Display DisplayOf() {
  return {g_display_width.load(std::memory_order_relaxed), g_display_height.load(std::memory_order_relaxed)};
}

inline float Bits(uint32_t b) {
  float f;
  static_assert(sizeof(f) == sizeof(b));
  __builtin_memcpy(&f, &b, sizeof(f));
  return f;
}
inline uint32_t BitsOf(float f) {
  uint32_t b;
  __builtin_memcpy(&b, &f, sizeof(b));
  return b;
}

inline std::string Json() {
  namespace ps = rex::ui::present_stats;
  namespace v = bo2zm::native::v042;
  const auto u = [](const std::atomic<uint32_t>& a) { return std::to_string(a.load(std::memory_order_relaxed)); };
  const auto q = [](const std::atomic<uint64_t>& a) { return std::to_string(a.load(std::memory_order_relaxed)); };
  const auto e = [&](const Extent& x) { return "{\"width\":" + u(x.width) + ",\"height\":" + u(x.height) + "}"; };
  const auto hex = [](uint32_t value) {
    char text[16];
    std::snprintf(text, sizeof(text), "\"%08X\"", value);
    return std::string(text);
  };
  const auto f = [](double value) {
    char text[32];
    std::snprintf(text, sizeof(text), "%.4f", value);
    return std::string(text);
  };
  const float tx = Bits(g_tan_x_bits.load(std::memory_order_relaxed));
  const float ty = Bits(g_tan_y_bits.load(std::memory_order_relaxed));
  const auto fov = v::FovFromTangents(tx, ty);
  const uint64_t scene_texels = g_pass_texels[kClassScene].load(std::memory_order_relaxed);
  const uint64_t composite_texels = g_pass_texels[kClassComposite].load(std::memory_order_relaxed);
  const uint64_t other_texels = g_pass_texels[kClassNone].load(std::memory_order_relaxed);
  const uint64_t swaps = g_swaps_painted.load(std::memory_order_relaxed);
  const auto per_swap = [swaps](uint64_t texels) { return swaps ? std::to_string(texels / swaps) : std::string("null"); };
  std::string out = "{";
  out += "\"output_requested\":\"" + std::string(v::OutputKey(v::Output(g_output.load()))) + "\"";
  out += ",\"screen_mode\":\"" + std::string(v::ScreenKey(v::Screen(g_screen.load()))) + "\"";
  out += ",\"display\":{\"width\":" + u(g_display_width) + ",\"height\":" + u(g_display_height) + "}";
  out += ",\"internal_requested\":{\"width\":" + u(g_scene_requested_width) + ",\"height\":" +
         u(g_scene_requested_height) + "}";
  out += ",\"internal_applied_vk\":{\"scene_color\":" + e(g_scene_color) + ",\"scene_depth\":" + e(g_scene_depth) +
         ",\"scene_resolved_texture\":" + e(g_scene_texture) + "}";
  out += ",\"composite_requested\":{\"width\":" + u(g_output_width) + ",\"height\":" + u(g_output_height) + "}";
  out += ",\"composite_vk\":{\"color\":" + e(g_composite_color) + ",\"depth\":" + e(g_composite_depth) + "}";
  out += ",\"front_buffer_vk\":{\"texture\":" + e(g_front_texture) + ",\"painted\":" + e(g_front_painted) + "}";
  out += ",\"presenter\":{\"aspect_requested\":\"" + u(g_present_aspect_x) + ":" + u(g_present_aspect_y) + "\"" +
         ",\"guest_output\":{\"width\":" + u(ps::frontbuffer_width) + ",\"height\":" + u(ps::frontbuffer_height) + "}" +
         ",\"surface\":{\"width\":" + u(ps::surface_width) + ",\"height\":" + u(ps::surface_height) + "}" +
         ",\"swapchain_vk\":{\"width\":" + u(ps::swapchain_width) + ",\"height\":" + u(ps::swapchain_height) +
         ",\"creations\":" + q(ps::swapchain_creations) + "}" +
         ",\"host_render_target\":{\"width\":" + u(ps::host_rt_width) + ",\"height\":" + u(ps::host_rt_height) + "}" +
         ",\"output_rect\":{\"x\":" + std::to_string(ps::output_x.load()) + ",\"y\":" +
         std::to_string(ps::output_y.load()) + ",\"width\":" + u(ps::output_width) + ",\"height\":" +
         u(ps::output_height) + "},\"flows\":" + q(ps::flows) + "}";
  out += ",\"ultrawide\":{\"state\":\"" +
         std::string(UltrawideName(Ultrawide(g_ultrawide.load(std::memory_order_relaxed)))) + "\",\"reason\":\"" +
         std::string(g_ultrawide_reason.load(std::memory_order_relaxed)) + "\",\"target_aspect\":" +
         f(double(g_target_aspect_micro.load()) / 1e6) + ",\"vidconfig_address\":" + hex(g_vidconfig_address.load(std::memory_order_relaxed)) +
         ",\"vidconfig_aspect\":[" + f(Bits(g_vidconfig_window_bits.load())) + "," +
         f(Bits(g_vidconfig_scene_bits.load())) + "," + f(Bits(g_vidconfig_display_bits.load())) + "]" +
         ",\"vidconfig_writes\":" + q(g_vidconfig_writes) + ",\"placements_adjusted\":" + q(g_placements_adjusted) +
         ",\"full_placement_factor\":" + f(double(g_placement_factor_micro.load()) / 1e6) +
         ",\"menu_safe_area_aspect\":" + f(double(g_lui_aspect_micro.load()) / 1e6) +
         ",\"menu_samples\":" + q(g_lui_samples) + "}";
  out += ",\"view\":{\"refdef\":{\"width\":" + u(g_refdef_width) + ",\"height\":" + u(g_refdef_height) +
         "},\"tangent_offset\":" + hex(g_refdef_tangent_offset.load(std::memory_order_relaxed)) + ",\"tan_half_fov\":[" + f(tx) + "," + f(ty) +
         "],\"effective_aspect\":" + f(fov.aspect) + ",\"effective_hfov_deg\":" + f(fov.horizontal_deg) +
         ",\"effective_vfov_deg\":" + f(fov.vertical_deg) + ",\"samples\":" + q(g_fov_samples) + "}";
  out += ",\"pixels\":{\"swaps\":" + std::to_string(swaps) + ",\"scene_pass_texels\":" + std::to_string(scene_texels) +
         ",\"composite_pass_texels\":" + std::to_string(composite_texels) + ",\"other_pass_texels\":" +
         std::to_string(other_texels) + ",\"scene_per_swap\":" + per_swap(scene_texels) +
         ",\"composite_per_swap\":" + per_swap(composite_texels) + ",\"other_per_swap\":" + per_swap(other_texels) +
         ",\"scene_passes\":" + q(g_passes[kClassScene]) + ",\"composite_passes\":" + q(g_passes[kClassComposite]) + "}";
  out += ",\"errors\":{\"scaled_resolve_rejected\":" + q(g_err_scaled_resolve_rejected) +
         ",\"front_size_mismatch\":" + q(g_err_front_size_mismatch) + ",\"present_failed\":" +
         q(g_err_present_failed) + ",\"lazy_front_fallback\":" + q(g_err_lazy_front_fallback) +
         ",\"target_create_failed\":" + q(g_err_target_create) + "}";
  out += "}";
  return out;
}

}
