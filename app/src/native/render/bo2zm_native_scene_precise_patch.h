// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <array>
#include <cstdint>
#include <vulkan/vulkan.h>


namespace bo2zm::native::scene::precise_patch {
enum DependencyBits : uint32_t {
  kDepColor = 1u << 0,
  kDepDepth = 1u << 1,
  kDepSampled = 1u << 2,
  kDepTransferIn = 1u << 3,
  kDepTransferOut = 1u << 4,
  kDepVertex = 1u << 5,
  kDepShaderWrite = 1u << 6,
  kDepLegacyTransfer = 1u << 7,
};
constexpr uint32_t kDepDefaultMask = kDepColor | kDepDepth | kDepSampled | kDepTransferIn | kDepTransferOut |
                                     kDepShaderWrite;

struct PassDependencies {
  std::array<VkSubpassDependency, 16> items{};
  uint32_t count = 0;
  void Add(uint32_t src_subpass, uint32_t dst_subpass, VkPipelineStageFlags src_stage,
           VkPipelineStageFlags dst_stage, VkAccessFlags src_access, VkAccessFlags dst_access) {
    if (count < items.size()) {
      items[count++] = {src_subpass, dst_subpass, src_stage, dst_stage, src_access, dst_access, 0};
    }
  }
};
namespace detail {
constexpr VkPipelineStageFlags kColorStage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
constexpr VkPipelineStageFlags kDepthStages =
    VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
constexpr VkPipelineStageFlags kAttachStages = kColorStage | kDepthStages;
constexpr VkAccessFlags kColorRW =
    VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
constexpr VkAccessFlags kDepthRW =
    VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
constexpr VkAccessFlags kAttachWrite =
    VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
constexpr VkAccessFlags kAttachRW = kColorRW | kDepthRW;
}

inline PassDependencies LegacyPassDependencies() {
  constexpr VkAccessFlags accesses = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
  PassDependencies d;
  d.Add(VK_SUBPASS_EXTERNAL, 0, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT, accesses,
        accesses);
  d.Add(0, VK_SUBPASS_EXTERNAL, VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, accesses,
        accesses);
  return d;
}
inline std::array<VkSubpassDependency, 2> ImageDependencies() {
  const PassDependencies d = LegacyPassDependencies();
  return {{d.items[0], d.items[1]}};
}
inline PassDependencies PrecisePassDependencies(uint32_t mask) {
  using namespace detail;
  PassDependencies d;
  const VkPipelineStageFlags vertex = (mask & kDepVertex) ? VK_PIPELINE_STAGE_VERTEX_SHADER_BIT : 0;
  const VkPipelineStageFlags sampling = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | vertex;
  if (mask & kDepColor) {
    d.Add(VK_SUBPASS_EXTERNAL, 0, kColorStage, kColorStage, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, kColorRW);
  }
  if (mask & kDepDepth) {
    d.Add(VK_SUBPASS_EXTERNAL, 0, kDepthStages, kDepthStages, VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
          kDepthRW);
  }
  if (mask & kDepSampled) {
    d.Add(VK_SUBPASS_EXTERNAL, 0, kAttachStages, sampling, kAttachWrite, VK_ACCESS_SHADER_READ_BIT);
    d.Add(VK_SUBPASS_EXTERNAL, 0, sampling, kAttachStages, 0, kAttachWrite);
    d.Add(0, VK_SUBPASS_EXTERNAL, kAttachStages, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | sampling, kAttachWrite,
          VK_ACCESS_SHADER_READ_BIT);
  }
  if (mask & kDepTransferIn) {
    d.Add(VK_SUBPASS_EXTERNAL, 0, VK_PIPELINE_STAGE_TRANSFER_BIT, kAttachStages | sampling,
          VK_ACCESS_TRANSFER_WRITE_BIT, kAttachRW | VK_ACCESS_SHADER_READ_BIT);
  }
  if (mask & kDepTransferOut) {
    d.Add(0, VK_SUBPASS_EXTERNAL, kAttachStages, VK_PIPELINE_STAGE_TRANSFER_BIT, kAttachWrite,
          VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT);
  }
  if (mask & kDepShaderWrite) {
    constexpr VkPipelineStageFlags shaders =
        VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
    d.Add(VK_SUBPASS_EXTERNAL, 0, shaders, shaders | vertex | kAttachStages,
          VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT | kAttachRW);
    d.Add(0, VK_SUBPASS_EXTERNAL, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
          shaders | vertex | VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_SHADER_WRITE_BIT,
          VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_READ_BIT);
  }
  return d;
}
inline PassDependencies PassDependenciesFor(bool precise, uint32_t mask) {
  return precise ? PrecisePassDependencies(mask) : LegacyPassDependencies();
}

inline void BeforeTransfer(PFN_vkCmdPipelineBarrier barrier, VkCommandBuffer commands, bool precise,
                           uint32_t mask) {
  if (!precise || (mask & kDepLegacyTransfer)) {
    const VkMemoryBarrier memory{VK_STRUCTURE_TYPE_MEMORY_BARRIER, nullptr,
        VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT,
        VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT};
    barrier(commands, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
            0, 1, &memory, 0, nullptr, 0, nullptr);
    return;
  }
  if (!(mask & kDepTransferOut)) {
    return;
  }
  using namespace detail;
  const VkPipelineStageFlags src = kAttachStages | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT |
                                   VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT |
                                   ((mask & kDepVertex) ? VK_PIPELINE_STAGE_VERTEX_SHADER_BIT : 0);
  const VkMemoryBarrier memory{VK_STRUCTURE_TYPE_MEMORY_BARRIER, nullptr,
      kAttachWrite | VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT,
      VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT};
  barrier(commands, src, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &memory, 0, nullptr, 0, nullptr);
}

inline void BeforeOutputRead(PFN_vkCmdPipelineBarrier barrier, VkCommandBuffer commands, bool precise) {
  if (!precise) {
    const VkMemoryBarrier memory{VK_STRUCTURE_TYPE_MEMORY_BARRIER, nullptr,
        VK_ACCESS_MEMORY_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT};
    barrier(commands, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
            0, 1, &memory, 0, nullptr, 0, nullptr);
    return;
  }
  const VkMemoryBarrier memory{VK_STRUCTURE_TYPE_MEMORY_BARRIER, nullptr,
      VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT};
  barrier(commands, VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
          VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 1, &memory, 0, nullptr, 0, nullptr);
}
}
