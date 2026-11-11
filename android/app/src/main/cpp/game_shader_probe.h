// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <vulkan/vulkan.h>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
struct GameShaderProbeResult {
  bool passed=false, index_loaded=false, pack_verified=false, objects_destroyed=false;
  uint32_t expected=0, modules_created=0, pipelines_created=0, vertex=0, pixel=0, current=0;
  int32_t vk_result=VK_SUCCESS;
  uint64_t elapsed_ms=0;
  std::string phase="game_shaders_start", stage, error;
};
using GameShaderProgress=std::function<void(const GameShaderProbeResult&)>;


GameShaderProbeResult ProbeGameShaderPipelines(VkPhysicalDevice physical,VkDevice device,
    const std::filesystem::path& pack,const GameShaderProgress& progress);
