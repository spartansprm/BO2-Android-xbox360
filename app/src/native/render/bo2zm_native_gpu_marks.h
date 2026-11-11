// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <cstdint>
#include <vector>
#include <vulkan/vulkan.h>

namespace bo2zm::native::timing {
inline constexpr uint32_t kMarksPerSlot = 128;
inline constexpr uint8_t kEnd = 0xFF;


inline uint32_t ResetCount(bool categories) { return categories ? kMarksPerSlot : 2; }

inline void Category(PFN_vkCmdWriteTimestamp write, VkCommandBuffer commands,
                     VkQueryPool queries, uint32_t base, std::vector<uint8_t>& marks,
                     uint8_t category, bool categories, bool precise, bool force = false) {
  if (!write || queries == VK_NULL_HANDLE || commands == VK_NULL_HANDLE ||
      marks.size() + 1 >= kMarksPerSlot ||
      (!marks.empty() && (!categories || (!force && marks.back() == category)))) {
    return;
  }
  write(commands, precise ? VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT : VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
        queries, base + uint32_t(marks.size()));
  marks.push_back(category);
}

inline void End(PFN_vkCmdWriteTimestamp write, VkCommandBuffer commands,
                VkQueryPool queries, uint32_t base, std::vector<uint8_t>& marks) {
  if (!write || queries == VK_NULL_HANDLE || commands == VK_NULL_HANDLE ||
      marks.empty() || marks.size() >= kMarksPerSlot) {
    return;
  }
  write(commands, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, queries, base + uint32_t(marks.size()));
  marks.push_back(kEnd);
}
}
