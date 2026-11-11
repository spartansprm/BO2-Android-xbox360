// SPDX-License-Identifier: GPL-3.0-only


#pragma once

#include <algorithm>
#include <array>
#include <cstdint>

namespace bo2zm::native::v040 {


inline constexpr uint32_t kLoadModeMask = 0x3;
inline constexpr uint32_t kLoadClearShift = 4;

constexpr uint32_t AttachmentLoadMode(uint32_t mode_load, uint32_t attachment, uint32_t mode_clear) {
  return ((mode_load >> (kLoadClearShift + attachment)) & 0x1) ? mode_clear : (mode_load & kLoadModeMask);
}

constexpr uint32_t WithClear(uint32_t mode_load, uint32_t attachment) {
  return mode_load | (uint32_t(1) << (kLoadClearShift + attachment));
}


template <class Value>
uint32_t PackClearValues(const uint32_t formats[5], const std::array<bool, 5>& fused,
                         const std::array<Value, 5>& values, std::array<Value, 5>& out) {
  uint32_t n = 0;
  uint32_t count = 0;
  for (uint32_t i = 0; i < 5; ++i) {
    if (!formats[i]) {
      continue;
    }
    if (fused[i]) {
      out[n] = values[i];
      count = n + 1;
    }
    ++n;
  }
  return count;
}


constexpr uint32_t ScaledSide(uint32_t side, uint32_t percent) {
  return std::max<uint32_t>(16, uint32_t((uint64_t(side) * percent + 99) / 100));
}


constexpr int32_t ToHost(int32_t v, uint32_t host, uint32_t guest) {
  return (guest && guest != host) ? int32_t(int64_t(v) * host / guest) : v;
}


constexpr uint32_t StoredSide(uint32_t side_guest, uint32_t target_host, uint32_t target_guest) {
  return std::max<uint32_t>(1, uint32_t(uint64_t(side_guest) * target_host / target_guest));
}

}
