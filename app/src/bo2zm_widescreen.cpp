// SPDX-License-Identifier: GPL-3.0-only


#include "bo2zm_widescreen.h"

#include <array>
#include <atomic>
#include <cmath>
#include <cstring>
#include <mutex>
#include <string>

#include <fmt/format.h>
#include <rex/logging.h>

#include "native/render/bo2zm_display_state.h"

namespace bo2zm::widescreen {
namespace {

namespace v = bo2zm::native::v042;
namespace d = bo2zm::display;


constexpr uint32_t kVidWindow = 0x82C153C0;
constexpr int kVidWindowFloats = 32;
constexpr uint32_t kCgArray = 0x82BBAE68, kCgStride = 0x87700, kLocals = 0x82C70F44;
constexpr uint32_t kRefdefRect = 0x4D880;
constexpr uint32_t kRefdefFloats = 0x4D890;
constexpr int kRefdefFloatCount = 10;
constexpr uint32_t kViewPlacements = 0x82CBC168, kFullPlacement = 0x82CBC360, kPlacementSize = 124;
constexpr uint32_t kHorizontalFactor = 0x82CBC358;

constexpr int kGraceFrames = 30;
constexpr uint32_t kConfirmFrames = 30;
constexpr uint32_t kRejectFrames = 60;
constexpr uint64_t kTimeoutFrames = 1800;
constexpr uint64_t kSearchFrames = 600;

uint32_t Word(const uint8_t* base, uint32_t address) {
  uint32_t value;
  std::memcpy(&value, base + address, 4);
  return __builtin_bswap32(value);
}
float Scalar(const uint8_t* base, uint32_t address) { return d::Bits(Word(base, address)); }
void PutScalar(uint8_t* base, uint32_t address, float value) {
  const uint32_t bits = __builtin_bswap32(d::BitsOf(value));
  std::memcpy(base + address, &bits, 4);
}

std::mutex mutex;
std::atomic<uint32_t> desired_bits{0};
std::atomic<bool> engaged{false};

struct State {
  bool initialized = false;
  double factor = 1.0, target = v::kAspect16x9;
  uint32_t vid = 0;
  std::array<float, 3> original{}, patched{};
  uint64_t boot_polls = 0, frames = 0, frames_searching = 0, frames_applied = 0;
  uint32_t fov_ok = 0, fov_bad = 0, lui_ok = 0, lui_bad = 0;
  uint32_t tangent_offset = 0;
  bool dumped = false, placements_reverted = false;
} g;

d::Ultrawide StateNow() { return d::Ultrawide(d::g_ultrawide.load(std::memory_order_relaxed)); }
void SetState(d::Ultrawide u, const char* reason) {
  d::g_ultrawide.store(uint32_t(u), std::memory_order_release);
  d::g_ultrawide_reason.store(reason, std::memory_order_relaxed);
}

void InitLocked() {
  if (g.initialized) return;
  g.initialized = true;
  d::LoadSettingsOnce();
  const auto screen = v::Screen(d::g_screen.load(std::memory_order_relaxed));
  const v::Display display = d::DisplayOf();
  g.target = v::UltrawideAspect(display);
  g.factor = g.target / v::kAspect16x9;
  if (screen != v::Screen::kUltrawide) {
    SetState(d::Ultrawide::kOff, "screen_mode_not_ultrawide");
    return;
  }
  if (!display.Known()) {
    SetState(d::Ultrawide::kOff, "display_size_unknown");
    REXLOG_WARN("[widescreen] ultrawide requested but the launcher gave no display size: 16:9 is kept");
    return;
  }
  if (g.factor < 1.01) {
    SetState(d::Ultrawide::kOff, "display_not_wider_than_16_9");
    REXLOG_INFO("[widescreen] ultrawide requested on a {}x{} display: not wider than 16:9, nothing to do",
                display.width, display.height);
    return;
  }
  engaged.store(true, std::memory_order_relaxed);
  desired_bits.store(0, std::memory_order_relaxed);
  SetState(d::Ultrawide::kSearching, "waiting_for_video_configuration");
  REXLOG_INFO("[widescreen] ultrawide for a {}x{} display: target aspect {:.4f}, horizontal factor {:.5f}",
              display.width, display.height, g.target, g.factor);
}

bool WriteAspect(uint8_t* base, const std::array<float, 3>& values) {
  bool changed = false;
  for (int i = 0; i < 3; ++i) {
    if (d::BitsOf(Scalar(base, g.vid + 4 * i)) != d::BitsOf(values[i])) {
      PutScalar(base, g.vid + 4 * i, values[i]);
      changed = true;
    }
  }
  if (changed) d::g_vidconfig_writes.fetch_add(1, std::memory_order_relaxed);
  d::g_vidconfig_window_bits.store(d::BitsOf(Scalar(base, g.vid)), std::memory_order_relaxed);
  d::g_vidconfig_scene_bits.store(d::BitsOf(Scalar(base, g.vid + 4)), std::memory_order_relaxed);
  d::g_vidconfig_display_bits.store(d::BitsOf(Scalar(base, g.vid + 8)), std::memory_order_relaxed);
  return changed;
}


void TryFindLocked(uint8_t* base) {
  std::array<float, kVidWindowFloats> floats{};
  for (int i = 0; i < kVidWindowFloats; ++i) floats[i] = Scalar(base, kVidWindow + 4 * i);
  const int index = v::FindAspectTriple(floats.data(), kVidWindowFloats);
  if (index < 0) {

    if (!g.dumped && g.boot_polls + g.frames_searching > 20000) {
      g.dumped = true;
      std::string text;
      for (int i = 0; i < kVidWindowFloats; ++i)
        text += fmt::format(" {:08X}={:g}", kVidWindow + 4 * i, floats[i]);
      REXLOG_WARN("[widescreen] the video configuration's aspect floats (16/9, 16/11, 1) are not in the window:{}",
                  text);
    }
    return;
  }
  g.vid = kVidWindow + 4 * uint32_t(index);
  g.original = {floats[index], floats[index + 1], floats[index + 2]};
  const auto wanted = v::AspectFor(g.factor);
  g.patched = {wanted.window, wanted.scene_pixel, wanted.display_pixel};
  d::g_vidconfig_address.store(g.vid, std::memory_order_relaxed);
  WriteAspect(base, g.patched);
  g.frames_applied = 0;
  g.fov_ok = g.fov_bad = g.lui_ok = g.lui_bad = 0;
  desired_bits.store(d::BitsOf(float(g.factor)), std::memory_order_relaxed);
  SetState(d::Ultrawide::kApplied, "aspect_written_waiting_for_engine");
  REXLOG_INFO("[widescreen] video configuration aspects at {:08X}: {:g} {:g} {:g} -> {:g} {:g} {:g} (output size at "
              "{:08X}: {}x{}; horizontal factor word {:g})",
              g.vid, g.original[0], g.original[1], g.original[2], g.patched[0], g.patched[1], g.patched[2],
              0x82C153E8u, Word(base, 0x82C153E8), Word(base, 0x82C153EC), Scalar(base, kHorizontalFactor));
}


bool AdjustRecord(uint8_t* base, uint32_t record, double factor) {
  std::array<float, v::kPlacementFloats> p{};
  for (int i = 0; i < v::kPlacementFloats; ++i) p[i] = Scalar(base, record + 4 * i);
  const auto before = p;
  if (!v::AdjustPlacement(p.data(), factor, g.factor)) return false;
  bool changed = false;
  for (int i = 0; i < v::kPlacementFloats; ++i) {
    if (d::BitsOf(p[i]) != d::BitsOf(before[i])) {
      PutScalar(base, record + 4 * i, p[i]);
      changed = true;
    }
  }
  if (changed) d::g_placements_adjusted.fetch_add(1, std::memory_order_relaxed);
  return true;
}

void AdjustAllLocked(uint8_t* base, double factor) {
  for (uint32_t i = 0; i < 4; ++i) AdjustRecord(base, kViewPlacements + i * kPlacementSize, factor);
  if (AdjustRecord(base, kFullPlacement, factor)) {
    std::array<float, v::kPlacementFloats> p{};
    for (int i = 0; i < 2; ++i) p[i] = Scalar(base, kFullPlacement + 4 * i);
    d::g_placement_factor_micro.store(uint64_t(v::PlacementFactor(p.data()) * 1e6 + 0.5), std::memory_order_relaxed);
  }
}

void FallBackLocked(uint8_t* base, const char* reason) {
  if (g.vid) WriteAspect(base, g.original);
  desired_bits.store(d::BitsOf(1.0f), std::memory_order_relaxed);
  AdjustAllLocked(base, 1.0);
  SetState(d::Ultrawide::kFallback, reason);
  REXLOG_WARN("[widescreen] ultrawide FALLS BACK to 16:9 ({}): aspect floats and placements restored, the picture is "
              "pillarboxed. view ok/16:9 {}/{}, menus ok/16:9 {}/{}, frames since the change {}",
              reason, g.fov_ok, g.fov_bad, g.lui_ok, g.lui_bad, g.frames_applied);
}

void DecideLocked(uint8_t* base) {
  const auto state = StateNow();
  if (state != d::Ultrawide::kApplied && state != d::Ultrawide::kConfirmed) return;
  if (g.lui_bad >= 3) {
    FallBackLocked(base, "menus_stay_16x9");
    return;
  }
  if (g.fov_bad >= kRejectFrames) {
    FallBackLocked(base, "view_fov_stays_16x9");
    return;
  }
  if (state == d::Ultrawide::kApplied) {
    if (g.fov_ok >= kConfirmFrames || g.lui_ok >= 2) {
      SetState(d::Ultrawide::kConfirmed, g.fov_ok >= kConfirmFrames ? "view_fov_follows" : "menus_follow");
      REXLOG_INFO("[widescreen] ultrawide CONFIRMED by the engine ({}): the picture uses the whole screen",
                  g.fov_ok >= kConfirmFrames ? "measured view aspect" : "menu safe area");
    } else if (g.frames_applied > kTimeoutFrames) {
      FallBackLocked(base, "not_confirmed_in_time");
    }
  }
}


void MeasureLocked(const uint8_t* base, uint32_t local) {
  if (local != 0) return;
  const uint32_t array = Word(base, kCgArray);
  const uint32_t locals = Word(base, kLocals);
  if (!locals || locals > 4 || array < 0x40000000 || uint64_t(array) + kCgStride > 0x100000000ull) return;
  const uint32_t cg = array;
  const int32_t w = int32_t(Word(base, cg + kRefdefRect + 8)), h = int32_t(Word(base, cg + kRefdefRect + 12));
  if (w <= 0 || h <= 0 || w > 16384 || h > 16384) return;
  d::g_refdef_width.store(uint32_t(w), std::memory_order_relaxed);
  d::g_refdef_height.store(uint32_t(h), std::memory_order_relaxed);
  std::array<float, kRefdefFloatCount> f{};
  for (int i = 0; i < kRefdefFloatCount; ++i) f[i] = Scalar(base, cg + kRefdefFloats + 4 * i);
  if (!g.tangent_offset) {
    const int index = v::FindTangentPair(f.data(), kRefdefFloatCount, v::kAspect16x9, g.target);
    if (index < 0) return;
    g.tangent_offset = kRefdefFloats + 4 * uint32_t(index);
    d::g_refdef_tangent_offset.store(g.tangent_offset, std::memory_order_relaxed);
    REXLOG_INFO("[widescreen] view tangents located at cg + {:X}: {:g} {:g} (refdef {}x{})", g.tangent_offset,
                f[index], f[index + 1], w, h);
  }
  const float tx = Scalar(base, cg + g.tangent_offset), ty = Scalar(base, cg + g.tangent_offset + 4);
  d::g_tan_x_bits.store(d::BitsOf(tx), std::memory_order_relaxed);
  d::g_tan_y_bits.store(d::BitsOf(ty), std::memory_order_relaxed);
  d::g_fov_samples.fetch_add(1, std::memory_order_relaxed);
  const auto fov = v::FovFromTangents(tx, ty);
  if (StateNow() != d::Ultrawide::kApplied && StateNow() != d::Ultrawide::kConfirmed) return;
  if (locals != 1) return;
  if (g.frames_applied < uint64_t(kGraceFrames)) return;
  if (v::AspectMatches(fov.aspect, g.target, 0.01)) {
    ++g.fov_ok;
    g.fov_bad = 0;
  } else if (v::AspectMatches(fov.aspect, v::kAspect16x9, 0.01)) {
    ++g.fov_bad;
  }
}

}

void PollBoot(uint8_t* base) {
  if (!base) return;
  std::lock_guard lock(mutex);
  InitLocked();
  if (!engaged.load(std::memory_order_relaxed)) return;
  ++g.boot_polls;
  const auto state = StateNow();
  if (state == d::Ultrawide::kSearching) {
    TryFindLocked(base);
  } else if ((state == d::Ultrawide::kApplied || state == d::Ultrawide::kConfirmed) && g.vid) {
    WriteAspect(base, g.patched);
  }
}

void OnViewFov(uint8_t* base, uint32_t local) {
  if (!base) return;
  std::lock_guard lock(mutex);
  InitLocked();
  MeasureLocked(base, local);
  if (local != 0 || !engaged.load(std::memory_order_relaxed)) return;
  ++g.frames;
  switch (StateNow()) {
    case d::Ultrawide::kSearching:
      TryFindLocked(base);
      if (StateNow() == d::Ultrawide::kSearching && ++g.frames_searching > kSearchFrames) {
        FallBackLocked(base, "video_configuration_not_found");
      }
      break;
    case d::Ultrawide::kApplied:
    case d::Ultrawide::kConfirmed:
      ++g.frames_applied;
      WriteAspect(base, g.patched);
      AdjustAllLocked(base, g.factor);
      DecideLocked(base);
      break;
    case d::Ultrawide::kFallback:
      if (!g.placements_reverted) {
        g.placements_reverted = true;
        AdjustAllLocked(base, 1.0);
      }
      break;
    case d::Ultrawide::kOff:
      break;
  }
}

uint32_t Placement(uint8_t* base, uint32_t record) {
  if (!base || !engaged.load(std::memory_order_relaxed) || record < 0x40000000u ||
      uint64_t(record) + kPlacementSize > 0x100000000ull) {
    return record;
  }
  const uint32_t wanted_bits = desired_bits.load(std::memory_order_relaxed);
  if (!wanted_bits) return record;
  const float wanted = d::Bits(wanted_bits);

  const float sx = Scalar(base, record), sy = Scalar(base, record + 4);
  if (!(sx > 0.0f) || !(sy > 0.0f)) return record;
  if (std::fabs(double(sy) / double(sx) - double(wanted)) <= 1e-4 * double(wanted)) return record;
  std::lock_guard lock(mutex);
  AdjustRecord(base, record, double(wanted));
  return record;
}

void OnMenuSafeArea(uint8_t* base, const float values[4]) {
  if (!base) return;
  std::lock_guard lock(mutex);
  InitLocked();
  const float left = values[0], top = values[1], right = values[2], bottom = values[3];

  if (!(right > left) || !(bottom > top) || std::fabs(left + right) > 0.5f || std::fabs(top + bottom) > 0.5f) return;
  const double aspect = double(right - left) / double(bottom - top);
  d::g_lui_aspect_micro.store(uint64_t(aspect * 1e6 + 0.5), std::memory_order_relaxed);
  d::g_lui_samples.fetch_add(1, std::memory_order_relaxed);
  if (StateNow() != d::Ultrawide::kApplied && StateNow() != d::Ultrawide::kConfirmed) return;
  if (v::AspectMatches(aspect, g.target, 0.02)) {
    ++g.lui_ok;
  } else if (v::AspectMatches(aspect, v::kAspect16x9, 0.02)) {
    ++g.lui_bad;
  }
  DecideLocked(base);
}

}
