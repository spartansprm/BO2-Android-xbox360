// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "bo2zm_native_texture_bc.h"
#include <array>
#include <vector>
#include <vulkan/vulkan.h>

namespace bo2zm::texture {
constexpr size_t kBcUploadLimit = size_t(256) << 20;
struct BcMipLayout {
  std::array<size_t, 16> block_offsets{}, pixel_offsets{}, block_layer_bytes{}, pixel_layer_bytes{};
  size_t block_bytes = 0, pixel_bytes = 0;
};


bool PlanBcMips(BcFormat format, uint32_t width, uint32_t height, uint32_t layers,
                 uint32_t levels, BcMipLayout& layout, size_t budget = kBcUploadLimit,
                 BcStorage storage = BcStorage::Rgba8);
struct BcMipUpload { BcMipLayout layout; std::vector<uint8_t> pixels; };
bool ConvertBcMips(BcFormat format, uint32_t width, uint32_t height, uint32_t layers,
                    uint32_t levels, std::span<const uint8_t> blocks, BcMipUpload& upload,
                    size_t budget = kBcUploadLimit, BcStorage storage = BcStorage::Rgba8);
bool BcFormatOf(VkFormat format, BcFormat& bc);
VkFormat BcHostFormat(BcFormat format, BcStorage storage);


bool TextureCopyRegions(uint32_t width, uint32_t height, uint32_t layers,
                         uint32_t depth, uint32_t levels, std::span<const uint32_t> offsets,
                         VkDeviceSize base, std::array<VkBufferImageCopy, 16>& regions,
                         uint32_t offset_alignment = 1);
}
