// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <cstdint>
#include <cstddef>
#include <span>
namespace bo2zm::native {


inline bool UboOnlySpirv(std::span<const uint32_t> words) {
  if (words.size() < 5 || words[0] != 0x07230203) return false;
  bool logical = false;
  for (std::size_t i = 5; i < words.size();) {
    const uint32_t size = words[i] >> 16, op = words[i] & 0xffff;
    if (!size || size > words.size() - i) return false;
    if (op == 17) {
      if (size != 2 || words[i+1] == 11 || words[i+1] == 12 || words[i+1] == 5347) return false;
    } else if (op == 21) {
      if (size != 4 || words[i+2] == 64) return false;
    } else if (op == 14) {
      if (size != 3 || words[i+1] != 0 || logical) return false;
      logical = true;
    } else if (op == 59) {
      if ((size != 4 && size != 5) || words[i+3] == 9) return false;
    }
    i += size;
  }
  return logical;
}
}
