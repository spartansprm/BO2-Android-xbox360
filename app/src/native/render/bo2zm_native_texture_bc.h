// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <cstddef>
#include <cstdint>
#include <span>

namespace bo2zm::texture {
enum class BcFormat { BC1, BC2, BC3, BC4, BC5 };
enum class BcStorage { Rgba8, Compact };
size_t BcBlockBytes(BcFormat format);


size_t BcPixelBytes(BcFormat format, BcStorage storage);
bool DecodeBc(BcFormat format, uint32_t width, uint32_t height,
              std::span<const uint8_t> source, std::span<uint8_t> output, BcStorage storage);


bool DecodeBcRgba8(BcFormat format, uint32_t width, uint32_t height,
                   std::span<const uint8_t> source, std::span<uint8_t> output);
}
