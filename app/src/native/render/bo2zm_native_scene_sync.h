// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <array>
#include <atomic>
#include <vulkan/vulkan.h>

namespace bo2zm::native::scene {


inline std::array<VkSubpassDependency, 2> ImageDependencies(int mode = 0) {
  constexpr VkAccessFlags kAll = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
  if (mode == 3) {


    constexpr VkPipelineStageFlags kSceneStages =
        VK_PIPELINE_STAGE_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT |
        VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT |
        VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT |
        VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    constexpr VkAccessFlags kSceneAccess =
        VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT |
        VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
        VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
        VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    return {{
        {VK_SUBPASS_EXTERNAL, 0, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
         kSceneStages, kAll, kSceneAccess, 0},
        {0, VK_SUBPASS_EXTERNAL, kSceneStages,
         VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, kSceneAccess, kAll, 0},
    }};
  }

  return {{
      {VK_SUBPASS_EXTERNAL, 0, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
       VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT, kAll, kAll, 0},
      {0, VK_SUBPASS_EXTERNAL, VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT,
       VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, kAll, kAll, 0},
  }};
}


inline void SelectImageDependencies(VkRenderPassCreateInfo& info,
                                    const std::array<VkSubpassDependency, 2>& dependencies,
                                    int mode, bool converted_copy,
                                    bool scene_has_depth = false) {
  if (mode == 6 || (mode == 4 && converted_copy) || (mode == 5 && !converted_copy))
    return;
  if (!converted_copy) {
    if (mode == 9 && !scene_has_depth) return;
    if (mode == 10 && scene_has_depth) return;
  }
  const bool incoming_only = mode == 1 || (mode == 8 && !converted_copy);
  const bool outgoing_only = mode == 2 || (mode == 7 && !converted_copy);
  info.dependencyCount = (incoming_only || outgoing_only) ? 1u : 2u;
  info.pDependencies = dependencies.data() + (outgoing_only ? 1 : 0);
}


inline std::atomic<uint64_t> transfer_barriers_original{0};
inline std::atomic<uint64_t> transfer_barriers_experimental{0};
inline void BeforeTransfer(PFN_vkCmdPipelineBarrier barrier, VkCommandBuffer commands,
                           bool transfer_destination_only = false) {
  constexpr VkAccessFlags kAll = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
  const VkMemoryBarrier memory{VK_STRUCTURE_TYPE_MEMORY_BARRIER, nullptr, kAll,
      transfer_destination_only ? (VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT)
                                : kAll};
  barrier(commands, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
          transfer_destination_only ? VK_PIPELINE_STAGE_TRANSFER_BIT
                                    : VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
          0, 1, &memory, 0, nullptr, 0, nullptr);
  (transfer_destination_only ? transfer_barriers_experimental : transfer_barriers_original)
      .fetch_add(1, std::memory_order_relaxed);
}


inline void BeforeOutputRead(PFN_vkCmdPipelineBarrier barrier, VkCommandBuffer commands) {
  const VkMemoryBarrier memory{VK_STRUCTURE_TYPE_MEMORY_BARRIER, nullptr,
      VK_ACCESS_MEMORY_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT};
  barrier(commands, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
          0, 1, &memory, 0, nullptr, 0, nullptr);
}
}
