#pragma once

#include <rex/ui/vulkan/device.h>

namespace bo2zm::native::deferred {

using Functions = rex::ui::vulkan::VulkanDevice::Functions;


const Functions& Table(const rex::ui::vulkan::VulkanDevice& device);


PFN_vkVoidFunction Wrap(const char* name, PFN_vkVoidFunction real);


void Begin(VkCommandBuffer commands);

void Finish();


void SetDrawBatchSafe(VkCommandBuffer commands, bool safe);


void Kick();

bool Deferring();


void PublishInBatches(bool on);

void Prefetch(void (*prefetch)(VkPipeline), VkPipeline pipeline);

void Report();

}
