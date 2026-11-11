// Physical HID sources are separate from the four guest users. Handheld and
// No1 must not share a PadState when both can be independent local players.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace rex::input::nx {
inline constexpr size_t kNpadExternalSources = 4;
inline constexpr size_t kNpadHandheldSource = kNpadExternalSources;
inline constexpr size_t kNpadSourceCount = kNpadExternalSources + 1;

// Retain external source indices and prefer handheld only during initial
// enumeration. InputSystem preserves guest assignments for connected devices.
inline constexpr std::array<size_t, kNpadSourceCount> kNpadEnumerationOrder = {
    kNpadHandheldSource, 0, 1, 2, 3};

constexpr uint32_t NpadSourceId(size_t source) {
  return source < kNpadExternalSources ? uint32_t(source) :
      source == kNpadHandheldSource ? 0x20u : UINT32_MAX;
}

constexpr uint64_t NpadSourceMask(size_t source) {
  const auto id = NpadSourceId(source);
  return id < 64 ? uint64_t(1) << id : 0;
}
}  // namespace rex::input::nx
