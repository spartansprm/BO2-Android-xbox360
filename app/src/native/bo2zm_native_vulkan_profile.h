// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <rex/cvar.h>
#include <rex/platform.h>
#include <rex/ui/vulkan/provider.h>

namespace bo2zm::native {
inline std::unique_ptr<rex::ui::vulkan::VulkanProvider> CreateNativeVulkanProvider(bool presentation) {
  if (!rex::cvar::SetFlagByName("vulkan_native_shader_features", "true")) return nullptr;
#if REX_PLATFORM_ANDROID
  if (!rex::cvar::SetFlagByName("vulkan_native_ubo_only", "true")) return nullptr;
  return rex::ui::vulkan::VulkanProvider::Create(false, presentation);
#else
  return rex::ui::vulkan::VulkanProvider::Create(true, presentation);
#endif
}
}
