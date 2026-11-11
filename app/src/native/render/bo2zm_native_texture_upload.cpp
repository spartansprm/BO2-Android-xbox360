// SPDX-License-Identifier: GPL-3.0-only
#include "bo2zm_native_texture_upload.h"
#include <algorithm>
#include <bit>
#include <limits>
#include <new>
#include <stdexcept>

namespace bo2zm::texture {
bool PlanBcMips(BcFormat format, uint32_t width, uint32_t height, uint32_t layers,
                 uint32_t levels, BcMipLayout& layout, size_t budget, BcStorage storage) {
  const size_t block = BcBlockBytes(format);
  const size_t pixel = BcPixelBytes(format, storage);
  if (!width || !height || !layers || !levels || levels > 16 || !block || !pixel ||
      levels > uint32_t(std::bit_width(std::max(width, height)))) return false;
  BcMipLayout plan;
  for (uint32_t n = 0; n < levels; ++n) {
    const uint64_t w = std::max(width >> n, 1u), h = std::max(height >> n, 1u);


    const size_t padding = (4 - plan.pixel_bytes % 4) % 4;
    if (plan.pixel_bytes > budget || padding > budget - plan.pixel_bytes) return false;
    plan.pixel_bytes += padding;
    if (w * h > budget / pixel) return false;
    const uint64_t pixels = w * h * pixel, bc = ((w + 3) / 4) * ((h + 3) / 4) * block;
    if (pixels > (budget - plan.pixel_bytes) / layers ||
        bc > (std::numeric_limits<size_t>::max() - plan.block_bytes) / layers) return false;
    plan.pixel_offsets[n] = plan.pixel_bytes; plan.block_offsets[n] = plan.block_bytes;
    plan.pixel_layer_bytes[n] = size_t(pixels); plan.block_layer_bytes[n] = size_t(bc);
    plan.pixel_bytes += size_t(pixels) * layers; plan.block_bytes += size_t(bc) * layers;
  }

  if (plan.pixel_bytes > UINT32_MAX || plan.block_bytes > UINT32_MAX) return false;
  layout = plan;
  return true;
}
bool ConvertBcMips(BcFormat format, uint32_t width, uint32_t height, uint32_t layers,
                    uint32_t levels, std::span<const uint8_t> blocks, BcMipUpload& upload, size_t budget, BcStorage storage) {
  BcMipUpload converted;
  if (!PlanBcMips(format, width, height, layers, levels, converted.layout, budget, storage) ||
      blocks.size() != converted.layout.block_bytes) return false;
  try { converted.pixels.resize(converted.layout.pixel_bytes); }
  catch (const std::bad_alloc&) { return false; }
  catch (const std::length_error&) { return false; }
  for (uint32_t n = 0; n < levels; ++n) for (uint32_t layer = 0; layer < layers; ++layer) {
    const auto source = blocks.subspan(converted.layout.block_offsets[n] +
        layer * converted.layout.block_layer_bytes[n], converted.layout.block_layer_bytes[n]);
    const auto target = std::span(converted.pixels).subspan(converted.layout.pixel_offsets[n] +
        layer * converted.layout.pixel_layer_bytes[n], converted.layout.pixel_layer_bytes[n]);
    if (!DecodeBc(format, std::max(width >> n, 1u), std::max(height >> n, 1u), source, target, storage)) return false;
  }
  upload = std::move(converted);
  return true;
}
bool BcFormatOf(VkFormat format, BcFormat& bc) {
  switch (format) {
    case VK_FORMAT_BC1_RGBA_UNORM_BLOCK: bc = BcFormat::BC1; return true;
    case VK_FORMAT_BC2_UNORM_BLOCK: bc = BcFormat::BC2; return true;
    case VK_FORMAT_BC3_UNORM_BLOCK: bc = BcFormat::BC3; return true;
    case VK_FORMAT_BC4_UNORM_BLOCK: bc = BcFormat::BC4; return true;
    case VK_FORMAT_BC5_UNORM_BLOCK: bc = BcFormat::BC5; return true;
    default: return false;
  }
}
VkFormat BcHostFormat(BcFormat format, BcStorage storage) {
  switch (BcPixelBytes(format, storage)) {
    case 1: return VK_FORMAT_R8_UNORM;
    case 2: return VK_FORMAT_R8G8_UNORM;
    case 4: return VK_FORMAT_R8G8B8A8_UNORM;
    default: return VK_FORMAT_UNDEFINED;
  }
}
bool TextureCopyRegions(uint32_t width, uint32_t height, uint32_t layers, uint32_t depth,
                         uint32_t levels, std::span<const uint32_t> offsets, VkDeviceSize base,
                         std::array<VkBufferImageCopy, 16>& regions, uint32_t offset_alignment) {
  if (!width || !height || !layers || !depth || !levels || levels > 16 || offsets.size() < levels ||
      levels > uint32_t(std::bit_width(std::max(width, height))) || !std::has_single_bit(offset_alignment) ||
      base % offset_alignment) return false;
  std::array<VkBufferImageCopy, 16> copies{};
  for (uint32_t n = 0; n < levels; ++n) {
    if (offsets[n] % offset_alignment || (n && offsets[n] <= offsets[n - 1]) ||
        base > std::numeric_limits<VkDeviceSize>::max() - offsets[n]) return false;
    copies[n].bufferOffset = base + offsets[n];
    copies[n].imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, n, 0, layers};
    copies[n].imageExtent = {std::max(width >> n, 1u), std::max(height >> n, 1u), depth};
  }
  regions = copies;
  return true;
}
}
