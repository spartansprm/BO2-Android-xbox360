// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <vulkan/vulkan.h>
#include <cstdint>
#include <string>
#include "bo2zm_native_texture_bc.h"

struct GraphicsProbeResult {
  bool layout_support = false;
  bool descriptor_allocation = false;
  bool pipeline_created = false;
  bool draw_submitted = false;
  bool pixels_match = false;
  uint32_t pixels_checked = 0;
  uint32_t max_channel_error = 0;
  uint32_t first_pixel_rgba = 0;
  bool cpu_decode_match = false;
  bool texture_uploaded = false;
  uint32_t subresources_checked = 0;
  std::string error;
};


GraphicsProbeResult ProbeUboGraphics(VkPhysicalDevice physical, VkDevice device,
                                     VkQueue queue, uint32_t family);


GraphicsProbeResult ProbePortUboGraphics(VkPhysicalDevice physical, VkDevice device,
                                         VkQueue queue, uint32_t family);
GraphicsProbeResult ProbePortBcGraphics(VkPhysicalDevice physical, VkDevice device,
                                        VkQueue queue, uint32_t family,
                                        bo2zm::texture::BcFormat format,
                                        bo2zm::texture::BcStorage storage = bo2zm::texture::BcStorage::Compact);
const char* PortProbeShaderHeaderSha256();
GraphicsProbeResult ProbePortBcMipGraphics(VkPhysicalDevice physical, VkDevice device,
                                           VkQueue queue, uint32_t family,
                                           bo2zm::texture::BcFormat format,
                                           bo2zm::texture::BcStorage storage = bo2zm::texture::BcStorage::Compact);
