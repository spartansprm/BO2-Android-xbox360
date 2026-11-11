// SPDX-License-Identifier: GPL-3.0-only


#include "graphics_probe.h"
#include <iostream>
#include <vector>

int main() {
  VkInstance instance = VK_NULL_HANDLE;
  VkApplicationInfo app{};
  app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
  app.apiVersion = VK_API_VERSION_1_2;
  VkInstanceCreateInfo create{};
  create.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
  create.pApplicationInfo = &app;
  if (vkCreateInstance(&create, nullptr, &instance) != VK_SUCCESS) return 77;
  uint32_t count = 0;
  vkEnumeratePhysicalDevices(instance, &count, nullptr);
  if (!count) { vkDestroyInstance(instance, nullptr); return 77; }
  std::vector<VkPhysicalDevice> physical(count);
  vkEnumeratePhysicalDevices(instance, &count, physical.data());
  const auto gpu = physical.front();
  VkPhysicalDeviceProperties properties{};
  vkGetPhysicalDeviceProperties(gpu, &properties);
  std::cout << "ICD: " << properties.deviceName << '\n';
  VkPhysicalDeviceVulkan12Features f12{};
  f12.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
  VkPhysicalDeviceFeatures2 features{};
  features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
  features.pNext = &f12;
  vkGetPhysicalDeviceFeatures2(gpu, &features);
  const bool supported = properties.apiVersion >= VK_API_VERSION_1_2 &&
      features.features.shaderSampledImageArrayDynamicIndexing && f12.runtimeDescriptorArray &&
      f12.descriptorBindingPartiallyBound && f12.descriptorBindingSampledImageUpdateAfterBind &&
      f12.descriptorBindingUpdateUnusedWhilePending && properties.limits.maxBoundDescriptorSets >= 5;
  if (!supported) {
    std::cout << "SKIP: descriptor feature set or five bound sets unavailable\n";
    vkDestroyInstance(instance, nullptr);
    return 77;
  }
  uint32_t queues = 0, family = UINT32_MAX;
  vkGetPhysicalDeviceQueueFamilyProperties(gpu, &queues, nullptr);
  std::vector<VkQueueFamilyProperties> families(queues);
  vkGetPhysicalDeviceQueueFamilyProperties(gpu, &queues, families.data());
  for (uint32_t i = 0; i < queues; ++i)
    if (families[i].queueCount && (families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT)) { family = i; break; }
  if (family == UINT32_MAX) { vkDestroyInstance(instance, nullptr); return 77; }
  float priority = 1;
  VkDeviceQueueCreateInfo queueInfo{};
  queueInfo.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
  queueInfo.queueFamilyIndex = family;
  queueInfo.queueCount = 1;
  queueInfo.pQueuePriorities = &priority;
  VkPhysicalDeviceFeatures enabled{};
  enabled.shaderSampledImageArrayDynamicIndexing = VK_TRUE;
  VkPhysicalDeviceVulkan12Features enabled12{};
  enabled12.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
  enabled12.runtimeDescriptorArray = VK_TRUE;
  enabled12.descriptorBindingPartiallyBound = VK_TRUE;
  enabled12.descriptorBindingSampledImageUpdateAfterBind = VK_TRUE;
  enabled12.descriptorBindingUpdateUnusedWhilePending = VK_TRUE;
  VkDeviceCreateInfo deviceInfo{};
  deviceInfo.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
  deviceInfo.pNext = &enabled12;
  deviceInfo.pEnabledFeatures = &enabled;
  deviceInfo.queueCreateInfoCount = 1;
  deviceInfo.pQueueCreateInfos = &queueInfo;
  VkDevice device = VK_NULL_HANDLE;
  const auto status = vkCreateDevice(gpu, &deviceInfo, nullptr, &device);
  if (status != VK_SUCCESS) { vkDestroyInstance(instance, nullptr); return 1; }
  VkQueue queue = VK_NULL_HANDLE;
  vkGetDeviceQueue(device, family, 0, &queue);
  auto print = [](const char* name, const GraphicsProbeResult& result) {
    std::cout << name << " layouts=" << result.layout_support << " descriptors=" << result.descriptor_allocation
            << " pipeline=" << result.pipeline_created << " submitted=" << result.draw_submitted
            << " pixels=" << result.pixels_checked << " max_error=" << result.max_channel_error
            << " first_rgba=0x" << std::hex << result.first_pixel_rgba << std::dec
            << " match=" << result.pixels_match << " error=" << result.error << '\n';
    return result.pixels_match && result.error.empty();
  };
  bool passed = print("independent", ProbeUboGraphics(gpu, device, queue, family));
  passed = print("port-ubo", ProbePortUboGraphics(gpu, device, queue, family)) && passed;
  using bo2zm::texture::BcFormat;
  for (const auto& [format, name] : {std::pair{BcFormat::BC1, "bc1"}, {BcFormat::BC2, "bc2"},
         {BcFormat::BC3, "bc3"}, {BcFormat::BC4, "bc4"}, {BcFormat::BC5, "bc5"}}) {
    const auto texture = ProbePortBcGraphics(gpu, device, queue, family, format);
    passed = print(name, texture) && texture.cpu_decode_match && texture.texture_uploaded && passed;
    const auto chain = ProbePortBcMipGraphics(gpu, device, queue, family, format);
    std::cout << "mip-chain ";
    passed = print(name, chain) && chain.subresources_checked == 30 && passed;
    if (format == BcFormat::BC4 || format == BcFormat::BC5) {
      const auto fallback = ProbePortBcMipGraphics(gpu, device, queue, family, format,
                                                  bo2zm::texture::BcStorage::Rgba8);
      std::cout << "rgba8-fallback ";
      passed = print(name, fallback) && fallback.subresources_checked == 30 && passed;
    }
  }
  vkDestroyDevice(device, nullptr);
  vkDestroyInstance(instance, nullptr);
  return passed ? 0 : 1;
}
