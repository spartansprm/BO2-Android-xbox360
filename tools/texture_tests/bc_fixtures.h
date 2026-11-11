// SPDX-License-Identifier: GPL-3.0-only

#pragma once
#include "bo2zm_native_texture_bc.h"
#include <array>
#include <algorithm>
#include <vector>

namespace bo2test {
using bo2zm::texture::BcFormat;
struct BcFixture {
  BcFormat format;
  std::vector<uint8_t> blocks, rgba;
};
inline void WriteChannel(uint8_t* block, bool short_palette, unsigned phase) {
  block[0] = short_palette ? 0 : 252; block[1] = short_palette ? 252 : 0;
  uint64_t packed = 0;
  for (unsigned i = 0; i < 16; ++i) packed |= uint64_t((i + phase) % 8) << (3 * i);
  for (unsigned i = 0; i < 6; ++i) block[2 + i] = uint8_t(packed >> (8 * i));
}
inline BcFixture MakeBcFixture(BcFormat format, uint32_t width = 16, uint32_t height = 16, unsigned phase_offset = 0) {
  const size_t bytes = bo2zm::texture::BcBlockBytes(format);
  const uint32_t bx = (width + 3) / 4, by = (height + 3) / 4;
  BcFixture fixture{format, std::vector<uint8_t>(size_t(bx) * by * bytes),
                    std::vector<uint8_t>(size_t(width) * height * 4)};
  constexpr uint8_t alpha[2][8] = {{252, 0, 216, 180, 144, 108, 72, 36},
                                  {0, 252, 50, 100, 151, 201, 0, 255}};
  constexpr uint8_t color[2][4][4] = {
    {{255, 0, 0, 255}, {0, 0, 255, 255}, {170, 0, 85, 255}, {85, 0, 170, 255}},
    {{0, 0, 255, 255}, {255, 0, 0, 255}, {127, 0, 127, 255}, {0, 0, 0, 0}}};
  constexpr uint8_t reverse_four[4][4] = {{0, 0, 255, 255}, {255, 0, 0, 255},
                                        {85, 0, 170, 255}, {170, 0, 85, 255}};
  for (unsigned yb = 0; yb < by; ++yb) for (unsigned xb = 0; xb < bx; ++xb) {
    const unsigned block_index = xb + yb * bx, phase = block_index + phase_offset;
    const bool short_palette = (phase % 2) != 0;
    auto* block = fixture.blocks.data() + size_t(block_index) * bytes;
    if (format == BcFormat::BC1 || format == BcFormat::BC2 || format == BcFormat::BC3) {
      auto* colors = block + (format == BcFormat::BC1 ? 0 : 8);
      const uint16_t c0 = short_palette ? 0x001f : 0xf800, c1 = short_palette ? 0xf800 : 0x001f;
      colors[0] = uint8_t(c0); colors[1] = uint8_t(c0 >> 8);
      colors[2] = uint8_t(c1); colors[3] = uint8_t(c1 >> 8);
      for (unsigned y = 0; y < 4; ++y) for (unsigned x = 0; x < 4; ++x)
        colors[4 + y] |= uint8_t(((x + 2 * y + phase) % 4) << (2 * x));
      if (format == BcFormat::BC2)
        for (unsigned i = 0; i < 16; ++i) block[i / 2] |= uint8_t(((i + phase) % 16) << ((i % 2) * 4));
      if (format == BcFormat::BC3) WriteChannel(block, short_palette, phase);
    } else {
      WriteChannel(block, short_palette, phase);
      if (format == BcFormat::BC5) WriteChannel(block + 8, !short_palette, phase + 3);
    }
    for (unsigned y = 0; y < 4 && yb * 4 + y < height; ++y)
      for (unsigned x = 0; x < 4 && xb * 4 + x < width; ++x) {
        std::array<uint8_t, 4> expected{};
        const unsigned i = y * 4 + x, ci = (x + 2 * y + phase) % 4;
        if (format == BcFormat::BC4 || format == BcFormat::BC5) {
          expected = {alpha[short_palette][(i + phase) % 8],
                      uint8_t(format == BcFormat::BC5 ? alpha[!short_palette][(i + phase + 3) % 8] : 0), 0, 255};
        } else {
          const auto* p = short_palette && format != BcFormat::BC1 ? reverse_four[ci] : color[short_palette][ci];
          for (unsigned c = 0; c < 4; ++c) expected[c] = p[c];
          if (format == BcFormat::BC2) expected[3] = uint8_t(((i + phase) % 16) * 17);
          if (format == BcFormat::BC3) expected[3] = alpha[short_palette][(i + phase) % 8];
        }
        const size_t offset = (size_t(yb * 4 + y) * width + xb * 4 + x) * 4;
        for (unsigned c = 0; c < 4; ++c) fixture.rgba[offset + c] = expected[c];
      }
  }
  return fixture;
}
struct BcMipFixture {
  std::vector<uint8_t> blocks, rgba;
  std::vector<BcFixture> images;
};
inline BcMipFixture MakeBcMipFixture(BcFormat format, uint32_t width, uint32_t height,
                                      uint32_t layers, uint32_t levels) {
  BcMipFixture result;
  for (uint32_t n = 0; n < levels; ++n) for (uint32_t layer = 0; layer < layers; ++layer) {
    auto fixture = MakeBcFixture(format, std::max(width >> n, 1u), std::max(height >> n, 1u), n * 7 + layer * 3);
    result.blocks.insert(result.blocks.end(), fixture.blocks.begin(), fixture.blocks.end());
    result.rgba.insert(result.rgba.end(), fixture.rgba.begin(), fixture.rgba.end());
    result.images.push_back(std::move(fixture));
  }
  return result;
}
}
