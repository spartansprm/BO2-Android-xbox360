// SPDX-License-Identifier: GPL-3.0-only
#include "bo2zm_native_texture_bc.h"
#include <array>
#include <algorithm>
#include <cstring>
#include <limits>

namespace bo2zm::texture {
namespace {
using Pixel = std::array<uint8_t, 4>;
uint16_t Read16(const uint8_t* p) { return uint16_t(p[0]) | (uint16_t(p[1]) << 8); }
Pixel Color565(uint16_t value) {
  const auto r = uint8_t((value >> 11) & 31), g = uint8_t((value >> 5) & 63), b = uint8_t(value & 31);
  return {uint8_t((r << 3) | (r >> 2)), uint8_t((g << 2) | (g >> 4)),
          uint8_t((b << 3) | (b >> 2)), 255};
}
void DecodeColors(const uint8_t* block, bool allow_transparent, Pixel* pixels) {
  const uint16_t c0 = Read16(block), c1 = Read16(block + 2);
  std::array<Pixel, 4> palette{Color565(c0), Color565(c1), Pixel{}, Pixel{}};
  if (!allow_transparent || c0 > c1) {
    for (size_t c = 0; c < 3; ++c) {
      palette[2][c] = uint8_t((2u * palette[0][c] + palette[1][c]) / 3);
      palette[3][c] = uint8_t((palette[0][c] + 2u * palette[1][c]) / 3);
    }
    palette[2][3] = palette[3][3] = 255;
  } else {
    for (size_t c = 0; c < 3; ++c) palette[2][c] = uint8_t((unsigned(palette[0][c]) + palette[1][c]) / 2);
    palette[2][3] = 255;
  }
  for (size_t y = 0; y < 4; ++y)
    for (size_t x = 0; x < 4; ++x) pixels[y * 4 + x] = palette[(block[4 + y] >> (2 * x)) & 3];
}
void DecodeChannel(const uint8_t* block, uint8_t* values) {
  std::array<uint8_t, 8> palette{block[0], block[1], 0, 0, 0, 0, 0, 0};
  if (block[0] > block[1]) {
    for (unsigned i = 1; i <= 6; ++i) palette[i + 1] = uint8_t(((7 - i) * block[0] + i * block[1]) / 7);
  } else {
    for (unsigned i = 1; i <= 4; ++i) palette[i + 1] = uint8_t(((5 - i) * block[0] + i * block[1]) / 5);
    palette[7] = 255;
  }
  uint64_t indices = 0;
  for (unsigned i = 0; i < 6; ++i) indices |= uint64_t(block[i + 2]) << (8 * i);
  for (unsigned i = 0; i < 16; ++i) values[i] = palette[(indices >> (3 * i)) & 7];
}
}

size_t BcBlockBytes(BcFormat format) {
  switch (format) {
    case BcFormat::BC1: case BcFormat::BC4: return 8;
    case BcFormat::BC2: case BcFormat::BC3: case BcFormat::BC5: return 16;
  }
  return 0;
}

size_t BcPixelBytes(BcFormat format, BcStorage storage) {
  if (!BcBlockBytes(format)) return 0;
  if (storage == BcStorage::Rgba8) return 4;
  if (storage != BcStorage::Compact) return 0;
  return format == BcFormat::BC4 ? 1 : format == BcFormat::BC5 ? 2 : 4;
}

bool DecodeBc(BcFormat format, uint32_t width, uint32_t height,
              std::span<const uint8_t> source, std::span<uint8_t> output, BcStorage storage) {
  const size_t bytes = BcBlockBytes(format);
  const size_t pixel_bytes = BcPixelBytes(format, storage);
  if (!width || !height || !bytes || !pixel_bytes) return false;
  const uint64_t bx = (uint64_t(width) + 3) / 4, by = (uint64_t(height) + 3) / 4;
  const uint64_t texels = uint64_t(width) * height;
  if (texels > std::numeric_limits<size_t>::max() / pixel_bytes ||
      bx * by > std::numeric_limits<size_t>::max() / bytes ||
      source.size() != bx * by * bytes || output.size() != texels * pixel_bytes) return false;
  for (uint64_t yb = 0; yb < by; ++yb) {
    for (uint64_t xb = 0; xb < bx; ++xb) {
      const auto* block = source.data() + (yb * bx + xb) * bytes;
      const size_t columns = std::min<uint64_t>(4, width - xb * 4);
      if (pixel_bytes < 4) {
        uint8_t red[16], green[16];
        DecodeChannel(block, red);
        if (pixel_bytes == 2) DecodeChannel(block + 8, green);
        for (uint32_t y = 0; y < 4 && yb * 4 + y < height; ++y) {
          auto* row = output.data() + ((yb * 4 + y) * width + xb * 4) * pixel_bytes;
          if (pixel_bytes == 1) {
            std::memcpy(row, red + y * 4, columns);
          } else {
            for (size_t x = 0; x < columns; ++x) {
              row[x * 2] = red[y * 4 + x];
              row[x * 2 + 1] = green[y * 4 + x];
            }
          }
        }
        continue;
      }
      std::array<Pixel, 16> pixels{};
      if (format == BcFormat::BC1 || format == BcFormat::BC2 || format == BcFormat::BC3) {
        DecodeColors(block + (format == BcFormat::BC1 ? 0 : 8), format == BcFormat::BC1, pixels.data());
        if (format == BcFormat::BC2) {
          for (unsigned i = 0; i < 16; ++i) pixels[i][3] = uint8_t(((block[i / 2] >> ((i % 2) * 4)) & 15) * 17);
        } else if (format == BcFormat::BC3) {
          uint8_t alpha[16]; DecodeChannel(block, alpha);
          for (unsigned i = 0; i < 16; ++i) pixels[i][3] = alpha[i];
        }
      } else {
        uint8_t red[16], green[16]{};
        DecodeChannel(block, red);
        if (format == BcFormat::BC5) DecodeChannel(block + 8, green);
        for (unsigned i = 0; i < 16; ++i) pixels[i] = {red[i], green[i], 0, 255};
      }
      for (uint32_t y = 0; y < 4 && yb * 4 + y < height; ++y) {
        auto* row = output.data() + ((yb * 4 + y) * width + xb * 4) * pixel_bytes;
        std::memcpy(row, pixels.data() + y * 4, columns * 4);
      }
    }
  }
  return true;
}

bool DecodeBcRgba8(BcFormat format, uint32_t width, uint32_t height,
                   std::span<const uint8_t> source, std::span<uint8_t> output) {
  return DecodeBc(format, width, height, source, output, BcStorage::Rgba8);
}
}
