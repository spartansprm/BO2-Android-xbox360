// SPDX-License-Identifier: GPL-3.0-only

#include "../app/src/native/render/bo2zm_native_scene_sync.h"
#include <cassert>
#include <cstdint>
#include <cstdio>

using bo2zm::native::scene::BeforeTransfer;

struct BarrierCapture {
  uint32_t calls = 0;
  VkPipelineStageFlags src = 0, dst = 0;
  VkDependencyFlags dependencies = 0;
  VkMemoryBarrier memory{};
} last;

VKAPI_ATTR void VKAPI_CALL Intercept(VkCommandBuffer, VkPipelineStageFlags src,
                                    VkPipelineStageFlags dst, VkDependencyFlags flags,
                                    uint32_t count, const VkMemoryBarrier* memory,
                                    uint32_t buffers, const VkBufferMemoryBarrier*,
                                    uint32_t images, const VkImageMemoryBarrier*) {
  assert(count == 1 && memory != nullptr && buffers == 0 && images == 0);
  ++last.calls;
  last.src = src;
  last.dst = dst;
  last.dependencies = flags;
  last.memory = *memory;
}

int main() {
  constexpr auto all = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
  constexpr auto transfer = VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
  BeforeTransfer(&Intercept, VK_NULL_HANDLE, false);
  assert(last.calls == 1);
  assert(last.src == VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);
  assert(last.dst == VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);
  assert(last.dependencies == 0);
  assert(last.memory.srcAccessMask == all && last.memory.dstAccessMask == all);
  assert(bo2zm::native::scene::transfer_barriers_original.load() == 1);
  assert(bo2zm::native::scene::transfer_barriers_experimental.load() == 0);

  BeforeTransfer(&Intercept, VK_NULL_HANDLE, true);
  assert(last.calls == 2);
  assert(last.src == VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);
  assert(last.dst == VK_PIPELINE_STAGE_TRANSFER_BIT);
  assert(last.memory.srcAccessMask == all && last.memory.dstAccessMask == transfer);
  assert(bo2zm::native::scene::transfer_barriers_original.load() == 1);
  assert(bo2zm::native::scene::transfer_barriers_experimental.load() == 1);

  auto scene = bo2zm::native::scene::ImageDependencies();
  assert(scene.size() == 2);
  assert(scene[0].srcStageMask == VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);
  assert(scene[0].dstStageMask == VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT);
  assert(scene[0].srcAccessMask == all && scene[0].dstAccessMask == all);
  assert(scene[0].dependencyFlags == 0 && scene[1].dependencyFlags == 0);
  assert(scene[1].srcStageMask == VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT);
  assert(scene[1].dstStageMask == VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);
  assert(scene[1].srcAccessMask == all && scene[1].dstAccessMask == all);

  bo2zm::native::scene::BeforeOutputRead(&Intercept, VK_NULL_HANDLE);
  assert(last.calls == 3);
  assert(last.src == VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);
  assert(last.dst == VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
  assert(last.memory.srcAccessMask == VK_ACCESS_MEMORY_WRITE_BIT);
  assert(last.memory.dstAccessMask == VK_ACCESS_SHADER_READ_BIT);
  std::puts("v0.31 barrier signatures and original protections: PASS");
}
