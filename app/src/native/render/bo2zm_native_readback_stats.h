#pragma once
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace bo2zm::native {
struct Uint16ReadbackStats {
  size_t texels = 0;
  std::array<uint16_t, 4> minimum{65535, 65535, 65535, 65535};
  std::array<uint16_t, 4> maximum{};
  std::array<uint64_t, 4> sum{};
  std::array<size_t, 4> nonzero{};

  double Mean(size_t c) const { return texels ? double(sum[c]) / double(texels) : 0.0; }
};

inline Uint16ReadbackStats MeasureUint16Readback(const uint8_t* data, size_t texels) {
  Uint16ReadbackStats s;
  s.texels = texels;
  for (size_t t = 0; t < texels; ++t) {
    for (size_t c = 0; c < 4; ++c) {
      uint16_t v;
      std::memcpy(&v, data + t * 8 + c * 2, sizeof(v));
      s.minimum[c] = std::min(s.minimum[c], v);
      s.maximum[c] = std::max(s.maximum[c], v);
      s.sum[c] += v;
      s.nonzero[c] += v != 0;
    }
  }
  if (!texels) s.minimum.fill(0);
  return s;
}
}
