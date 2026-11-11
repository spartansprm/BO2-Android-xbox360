// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <array>
#include <vulkan/vulkan.h>

namespace bo2zm::native::output {


inline std::array<VkSubpassDependency, 2> ImageDependencies() {
  return {{
      {VK_SUBPASS_EXTERNAL, 0, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
       VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_SHADER_READ_BIT,
       VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, 0},
      {0, VK_SUBPASS_EXTERNAL, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
       VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
       VK_ACCESS_SHADER_READ_BIT, 0},
  }};
}
}
