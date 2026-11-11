// SPDX-License-Identifier: GPL-3.0-only

#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
#include <SDL3/SDL_system.h>
#include <SDL3/SDL_vulkan.h>
#include <android/log.h>
#include <android/sharedmem.h>
#include <jni.h>
#include <sys/mman.h>
#include <sys/utsname.h>
#include <unistd.h>
#include <vulkan/vulkan.h>
#include "graphics_probe.h"
#ifdef BO2_HAS_SDK_PROVIDER
#include "sdk_provider_probe.h"
#endif

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <iomanip>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {
std::string Quote(const std::string& text) {
  std::ostringstream out;
  out << '"';
  for (unsigned char c : text) {
    if (c == '"' || c == '\\') out << '\\' << c;
    else if (c < 32) out << "\\u" << std::hex << std::setw(4) << std::setfill('0') << unsigned(c);
    else out << c;
  }
  out << '"';
  return out.str();
}

class Report {
 public:
  void Text(const std::string& key, const std::string& value) { Put(key, Quote(value)); }
  void Flag(const std::string& key, bool value) { Put(key, value ? "true" : "false"); }
  void Number(const std::string& key, int64_t value) { Put(key, std::to_string(value)); }
  std::string Json() const {
    std::string result = "{";
    for (const auto& [key, value] : fields_) {
      if (result.size() > 1) result += ',';
      result += Quote(key) + ':' + value;
    }
    return result + '}';
  }
 private:
  void Put(const std::string& key, const std::string& value) {
    for (auto& item : fields_) if (item.first == key) { item.second = value; return; }
    fields_.emplace_back(key, value);
  }
  std::vector<std::pair<std::string, std::string>> fields_;
};

void RecordGraphics(Report& report, const std::string& prefix, const GraphicsProbeResult& result, bool texture = false) {
    report.Flag(prefix + ".pipeline_created", result.pipeline_created);
    report.Flag(prefix + ".draw_submitted", result.draw_submitted);
    report.Flag(prefix + ".pixels_match", result.pixels_match);
    report.Number(prefix + ".pixels_checked", result.pixels_checked);
    report.Number(prefix + ".max_channel_error", result.max_channel_error);
    if (texture) {
      report.Flag(prefix + ".cpu_decode_match", result.cpu_decode_match);
      report.Flag(prefix + ".texture_uploaded", result.texture_uploaded);
      report.Number(prefix + ".subresources_checked", result.subresources_checked);
    }
    if (!result.error.empty()) report.Text(prefix + ".error", result.error);
  }

void Publish(const Report& report) {
  const auto json = report.Json();
  __android_log_write(ANDROID_LOG_INFO, "BO2Diagnostic", json.c_str());
  auto* env = static_cast<JNIEnv*>(SDL_GetAndroidJNIEnv());
  auto activity = static_cast<jobject>(SDL_GetAndroidActivity());
  if (!env || !activity) return;
  jclass klass = env->GetObjectClass(activity);
  jmethodID callback = env->GetMethodID(klass, "onNativeReport", "(Ljava/lang/String;)V");
  if (callback) {
    jstring text = env->NewStringUTF(json.c_str());
    env->CallVoidMethod(activity, callback, text);
    env->DeleteLocalRef(text);
  }
  if (env->ExceptionCheck()) {
    env->ExceptionDescribe();
    env->ExceptionClear();
  }
  env->DeleteLocalRef(klass);
  env->DeleteLocalRef(activity);
}

void ProbeMemory(Report& report) {
  const long page = sysconf(_SC_PAGESIZE);
  report.Number("memory.host_page_bytes", page);
  report.Flag("memory.current_bo2_4k_layout_compatible", page == 4096);
  report.Text("memory.scope", "OS primitives only; no ReXGlue heap/MMIO/fault handler exercised");
  if (page <= 0) throw std::runtime_error("sysconf(_SC_PAGESIZE) failed");


  constexpr size_t arenaSize = 0x120000000ULL;
  void* arena = mmap(nullptr, arenaSize, PROT_NONE,
                     MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
  report.Flag("memory.reserve_4_5_gib_virtual", arena != MAP_FAILED);
  if (arena == MAP_FAILED) {
    report.Text("memory.reserve_error", std::strerror(errno));
    return;
  }
  const int fd = ASharedMemory_create("bo2-diagnostic", static_cast<size_t>(page));
  report.Flag("memory.asharedmemory_create", fd >= 0);
  bool aliases = fd >= 0;
  std::vector<volatile uint32_t*> mapped;
  const size_t offsets[] = {0x7F000000ULL, 0xA0000000ULL, 0xC0000000ULL,
                            0xE0000000ULL, 0x100000000ULL};
  if (fd >= 0) {
    for (size_t offset : offsets) {
      void* address = static_cast<char*>(arena) + offset;
      void* view = mmap(address, static_cast<size_t>(page), PROT_READ | PROT_WRITE,
                         MAP_SHARED | MAP_FIXED, fd, 0);
      if (view == MAP_FAILED) { aliases = false; break; }
      mapped.push_back(static_cast<volatile uint32_t*>(view));
    }
    if (aliases) {
      *mapped.front() = 0xB02A5468;
      for (auto* p : mapped) aliases = aliases && *p == 0xB02A5468;
      *mapped.back() = 0x12345678;
      for (auto* p : mapped) aliases = aliases && *p == 0x12345678;
    }
    if (!mapped.empty()) {
      auto* address = const_cast<uint32_t*>(mapped.front());
      const bool protectedOk = mprotect(address, static_cast<size_t>(page), PROT_NONE) == 0;
      const bool restoredOk = mprotect(address, static_cast<size_t>(page), PROT_READ | PROT_WRITE) == 0;
      report.Flag("memory.mprotect_roundtrip", protectedOk && restoredOk);
    }
    close(fd);
  }
  report.Flag("memory.shared_alias_roundtrip", aliases);
  report.Number("memory.committed_probe_bytes", fd >= 0 ? page : 0);
  munmap(arena, arenaSize);
}

void ProbeThreads(Report& report) {


  constexpr unsigned iterations = 20000;
  std::atomic<unsigned> ready{0}, ack{0};
  std::atomic<bool> cancel{false};
  uint64_t payload = 0;
  bool correct = true;
  const auto start = std::chrono::steady_clock::now();
  const auto deadline = start + std::chrono::seconds(5);
  auto waitFor = [&](const std::atomic<unsigned>& value, unsigned expected) {
    while (value.load(std::memory_order_acquire) != expected) {
      if (cancel.load(std::memory_order_relaxed)) return false;
      if (std::chrono::steady_clock::now() > deadline) {
        cancel.store(true, std::memory_order_relaxed);
        return false;
      }
      std::this_thread::yield();
    }
    return true;
  };
  std::thread consumer([&] {
    for (unsigned i = 1; i <= iterations; ++i) {
      if (!waitFor(ready, i)) return;
      if (payload != (uint64_t(i) ^ 0xB02A546800000000ULL)) correct = false;
      ack.store(i, std::memory_order_release);
    }
  });
  for (unsigned i = 1; i <= iterations; ++i) {
    if (!waitFor(ack, i - 1)) break;
    payload = uint64_t(i) ^ 0xB02A546800000000ULL;
    ready.store(i, std::memory_order_release);
  }
  const bool finished = waitFor(ack, iterations);
  consumer.join();
  report.Flag("threads.release_acquire_handoff", finished && correct);
  report.Number("threads.handoffs", ack.load());
  report.Number("threads.elapsed_ms", std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now() - start).count());
  report.Flag("threads.atomic_u64_lock_free", std::atomic<uint64_t>{}.is_lock_free());
  report.Text("threads.scope", "C++ release/acquire primitives; guest PPC ordering and fibers untested");
}

std::string Version(uint32_t value) {
  return std::to_string(VK_VERSION_MAJOR(value)) + "." +
      std::to_string(VK_VERSION_MINOR(value)) + "." + std::to_string(VK_VERSION_PATCH(value));
}

class VulkanProbe {
 public:
  explicit VulkanProbe(Report& report) : r_(report) {}
  ~VulkanProbe() {
    if (device_) {
      vkDeviceWaitIdle(device_);
      if (fence_) vkDestroyFence(device_, fence_, nullptr);
      if (acquired_) vkDestroySemaphore(device_, acquired_, nullptr);
      if (rendered_) vkDestroySemaphore(device_, rendered_, nullptr);
      if (pool_) vkDestroyCommandPool(device_, pool_, nullptr);
      for (auto f : framebuffers_) vkDestroyFramebuffer(device_, f, nullptr);
      for (auto v : views_) vkDestroyImageView(device_, v, nullptr);
      if (pass_) vkDestroyRenderPass(device_, pass_, nullptr);
      if (swapchain_) vkDestroySwapchainKHR(device_, swapchain_, nullptr);
      vkDestroyDevice(device_, nullptr);
    }
    if (surface_) vkDestroySurfaceKHR(instance_, surface_, nullptr);
    if (instance_) vkDestroyInstance(instance_, nullptr);
  }

  void Run(SDL_Window* window) {
    uint32_t loader = VK_API_VERSION_1_0;
    auto enumerateVersion = reinterpret_cast<PFN_vkEnumerateInstanceVersion>(
        vkGetInstanceProcAddr(VK_NULL_HANDLE, "vkEnumerateInstanceVersion"));
    if (enumerateVersion) Check(enumerateVersion(&loader), "enumerate_instance_version");
    r_.Text("vulkan.loader_api", Version(loader));
    if (loader < VK_API_VERSION_1_1) throw std::runtime_error("Vulkan loader < 1.1");
    uint32_t count = 0;
    const char* const* extensions = SDL_Vulkan_GetInstanceExtensions(&count);
    if (!extensions) throw std::runtime_error(SDL_GetError());
    VkApplicationInfo app{};
    app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app.pApplicationName = "BO2 Android Diagnostic";
    app.apiVersion = std::min(loader, uint32_t(VK_API_VERSION_1_2));
    VkInstanceCreateInfo create{};
    create.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    create.pApplicationInfo = &app;
    create.enabledExtensionCount = count;
    create.ppEnabledExtensionNames = extensions;
    Check(vkCreateInstance(&create, nullptr, &instance_), "create_instance");
    if (!SDL_Vulkan_CreateSurface(window, instance_, nullptr, &surface_))
      throw std::runtime_error(SDL_GetError());
    r_.Flag("vulkan.android_surface", true);
    Check(vkEnumeratePhysicalDevices(instance_, &count, nullptr), "physical_devices_count");
    r_.Number("vulkan.physical_devices", count);
    if (!count) throw std::runtime_error("No physical Vulkan devices");
    std::vector<VkPhysicalDevice> devices(count);
    Check(vkEnumeratePhysicalDevices(instance_, &count, devices.data()), "physical_devices");
    for (auto physical : devices) {
      uint32_t queueCount = 0;
      vkGetPhysicalDeviceQueueFamilyProperties(physical, &queueCount, nullptr);
      std::vector<VkQueueFamilyProperties> queues(queueCount);
      vkGetPhysicalDeviceQueueFamilyProperties(physical, &queueCount, queues.data());
      for (uint32_t i = 0; i < queueCount; ++i) {
        VkBool32 present = VK_FALSE;
        Check(vkGetPhysicalDeviceSurfaceSupportKHR(physical, i, surface_, &present), "surface_support");
        if (queues[i].queueCount && (queues[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) && present) {
          physical_ = physical;
          family_ = i;
          r_.Number("vulkan.queue_flags", queues[i].queueFlags);
          break;
        }
      }
      if (physical_) break;
    }
    if (!physical_) throw std::runtime_error("No combined graphics/present queue");
    VkPhysicalDeviceProperties properties{};
    vkGetPhysicalDeviceProperties(physical_, &properties);
    r_.Text("vulkan.device_name", properties.deviceName);
    r_.Text("vulkan.device_api", Version(properties.apiVersion));
    r_.Number("vulkan.vendor_id", properties.vendorID);
    r_.Number("vulkan.device_id", properties.deviceID);
    r_.Number("vulkan.driver_version_raw", properties.driverVersion);
    r_.Number("vulkan.max_storage_buffer_range", properties.limits.maxStorageBufferRange);
    r_.Number("vulkan.max_per_stage_sampled_images", properties.limits.maxPerStageDescriptorSampledImages);
    r_.Number("vulkan.max_descriptor_set_sampled_images", properties.limits.maxDescriptorSetSampledImages);
    r_.Number("vulkan.non_coherent_atom_size", properties.limits.nonCoherentAtomSize);
    r_.Number("vulkan.max_bound_descriptor_sets", properties.limits.maxBoundDescriptorSets);
    if (properties.apiVersion >= VK_API_VERSION_1_2) {
      VkPhysicalDeviceVulkan12Properties p12{};
      p12.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_PROPERTIES;
      VkPhysicalDeviceProperties2 p2{};
      p2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
      p2.pNext = &p12;
      vkGetPhysicalDeviceProperties2(physical_, &p2);
      r_.Text("vulkan.driver_name", p12.driverName);
      r_.Text("vulkan.driver_info", p12.driverInfo);
      r_.Number("vulkan.max_update_after_bind_descriptors_all_pools", p12.maxUpdateAfterBindDescriptorsInAllPools);
      r_.Number("vulkan.max_per_stage_update_after_bind_sampled_images", p12.maxPerStageDescriptorUpdateAfterBindSampledImages);
      r_.Number("vulkan.max_set_update_after_bind_sampled_images", p12.maxDescriptorSetUpdateAfterBindSampledImages);
      r_.Number("vulkan.max_per_stage_update_after_bind_samplers", p12.maxPerStageDescriptorUpdateAfterBindSamplers);
      r_.Number("vulkan.max_set_update_after_bind_samplers", p12.maxDescriptorSetUpdateAfterBindSamplers);
      r_.Number("vulkan.max_per_stage_update_after_bind_resources", p12.maxPerStageUpdateAfterBindResources);


      r_.Flag("bo2.descriptor_image_sampler_capacity_gate",
          p12.maxPerStageDescriptorUpdateAfterBindSampledImages >= 16704 &&
          p12.maxDescriptorSetUpdateAfterBindSampledImages >= 16704 &&
          p12.maxPerStageDescriptorUpdateAfterBindSamplers >= 512 &&
          p12.maxDescriptorSetUpdateAfterBindSamplers >= 512 &&
          p12.maxUpdateAfterBindDescriptorsInAllPools >= 17216 &&
          properties.limits.maxBoundDescriptorSets >= 5);
    }
    VkPhysicalDeviceMemoryProperties memory{};
    vkGetPhysicalDeviceMemoryProperties(physical_, &memory);
    for (uint32_t i = 0; i < memory.memoryHeapCount; ++i) {
      r_.Number("vulkan.heap_" + std::to_string(i) + "_bytes", memory.memoryHeaps[i].size);
      r_.Number("vulkan.heap_" + std::to_string(i) + "_flags", memory.memoryHeaps[i].flags);
    }
    VkPhysicalDeviceVulkan12Features f12{};
    f12.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
    VkPhysicalDeviceFeatures2 features{};
    features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
    if (properties.apiVersion >= VK_API_VERSION_1_2) features.pNext = &f12;
    vkGetPhysicalDeviceFeatures2(physical_, &features);
    const auto& f = features.features;
#define CORE_FEATURE(name) r_.Flag("vulkan.features." #name, f.name)
    CORE_FEATURE(independentBlend);
    CORE_FEATURE(fragmentStoresAndAtomics);
    CORE_FEATURE(vertexPipelineStoresAndAtomics);
    CORE_FEATURE(geometryShader);
    CORE_FEATURE(fillModeNonSolid);
    CORE_FEATURE(shaderInt64);
    CORE_FEATURE(shaderSampledImageArrayDynamicIndexing);
    CORE_FEATURE(textureCompressionBC);
    CORE_FEATURE(samplerAnisotropy);
    CORE_FEATURE(shaderClipDistance);
    CORE_FEATURE(depthClamp);
    CORE_FEATURE(pipelineStatisticsQuery);
#undef CORE_FEATURE
#define V12_FEATURE(name) r_.Flag("vulkan.features." #name, f12.name)
    V12_FEATURE(bufferDeviceAddress);
    V12_FEATURE(runtimeDescriptorArray);
    V12_FEATURE(descriptorBindingPartiallyBound);
    V12_FEATURE(descriptorBindingSampledImageUpdateAfterBind);
    V12_FEATURE(descriptorBindingUpdateUnusedWhilePending);
    V12_FEATURE(scalarBlockLayout);
    V12_FEATURE(uniformBufferStandardLayout);
    V12_FEATURE(samplerMirrorClampToEdge);
#undef V12_FEATURE

    const bool native = properties.apiVersion >= VK_API_VERSION_1_2 && f.shaderInt64 &&
        f.shaderSampledImageArrayDynamicIndexing && f12.bufferDeviceAddress &&
        f12.runtimeDescriptorArray && f12.descriptorBindingPartiallyBound &&
        f12.descriptorBindingSampledImageUpdateAfterBind &&
        f12.descriptorBindingUpdateUnusedWhilePending;
    r_.Flag("bo2.native_renderer_seven_feature_gate", native);
    r_.Text("bo2.feature_gate_scope", "Necessary Init features only; shader packs, formats and BO2 draws untested");
    r_.Flag("bo2.xenos_provider_three_feature_gate", f.independentBlend &&
        f.fragmentStoresAndAtomics && f.vertexPipelineStoresAndAtomics);
    r_.Flag("bo2.xenos_provider_default_geometry_fill_gate", f.geometryShader && f.fillModeNonSolid);
    const std::pair<VkFormat, const char*> formats[] = {
        {VK_FORMAT_R8G8B8A8_UNORM, "RGBA8"}, {VK_FORMAT_R16G16B16A16_SFLOAT, "RGBA16F"},
        {VK_FORMAT_D24_UNORM_S8_UINT, "D24S8"}, {VK_FORMAT_D32_SFLOAT_S8_UINT, "D32S8"},
        {VK_FORMAT_D32_SFLOAT, "D32F"},
        {VK_FORMAT_BC1_RGBA_UNORM_BLOCK, "BC1"}, {VK_FORMAT_BC3_UNORM_BLOCK, "BC3"},
        {VK_FORMAT_BC5_UNORM_BLOCK, "BC5"}};
    for (const auto& [format, name] : formats) {
      VkFormatProperties fp{};
      vkGetPhysicalDeviceFormatProperties(physical_, format, &fp);
      r_.Number(std::string("vulkan.formats.") + name + ".optimal_flags", fp.optimalTilingFeatures);
      r_.Flag(std::string("vulkan.formats.") + name + ".sampled", fp.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT);
      r_.Flag(std::string("vulkan.formats.") + name + ".color_attachment", fp.optimalTilingFeatures & VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT);
      r_.Flag(std::string("vulkan.formats.") + name + ".depth_attachment", fp.optimalTilingFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT);
    }
    Check(vkEnumerateDeviceExtensionProperties(physical_, nullptr, &count, nullptr), "device_extensions_count");
    std::vector<VkExtensionProperties> available(count);
    Check(vkEnumerateDeviceExtensionProperties(physical_, nullptr, &count, available.data()), "device_extensions");
    bool hasSwapchain = false;
    for (const auto& ext : available) {
      r_.Number(std::string("vulkan.extensions.") + ext.extensionName, ext.specVersion);
      hasSwapchain |= std::strcmp(ext.extensionName, VK_KHR_SWAPCHAIN_EXTENSION_NAME) == 0;
    }
    if (!hasSwapchain) throw std::runtime_error("VK_KHR_swapchain missing");
    Publish(r_);
    float priority = 1.0f;
    VkDeviceQueueCreateInfo queueInfo{};
    queueInfo.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    queueInfo.queueFamilyIndex = family_;
    queueInfo.queueCount = 1;
    queueInfo.pQueuePriorities = &priority;
    VkPhysicalDeviceFeatures enabled{};
    VkPhysicalDeviceVulkan12Features enabled12{};
    enabled12.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
    const bool descriptors = properties.apiVersion >= VK_API_VERSION_1_2 &&
        f.shaderSampledImageArrayDynamicIndexing && f12.runtimeDescriptorArray &&
        f12.descriptorBindingPartiallyBound && f12.descriptorBindingSampledImageUpdateAfterBind &&
        f12.descriptorBindingUpdateUnusedWhilePending;


    if (descriptors) {
      enabled.shaderSampledImageArrayDynamicIndexing = VK_TRUE;
      enabled.independentBlend = f.independentBlend;
      enabled12.runtimeDescriptorArray = VK_TRUE;
      enabled12.descriptorBindingPartiallyBound = VK_TRUE;
      enabled12.descriptorBindingSampledImageUpdateAfterBind = VK_TRUE;
      enabled12.descriptorBindingUpdateUnusedWhilePending = VK_TRUE;
    }
    const char* swapchainExtension = VK_KHR_SWAPCHAIN_EXTENSION_NAME;
    VkDeviceCreateInfo deviceInfo{};
    deviceInfo.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    deviceInfo.pNext = descriptors ? &enabled12 : nullptr;
    deviceInfo.pEnabledFeatures = &enabled;
    deviceInfo.queueCreateInfoCount = 1;
    deviceInfo.pQueueCreateInfos = &queueInfo;
    deviceInfo.enabledExtensionCount = 1;
    deviceInfo.ppEnabledExtensionNames = &swapchainExtension;
    Check(vkCreateDevice(physical_, &deviceInfo, nullptr, &device_), "create_device");
    r_.Flag("vulkan.native_feature_set_enabled", false);
    r_.Flag("vulkan.descriptor_feature_set_enabled", descriptors);
    r_.Flag("mali.shaderInt64_enabled", false);
    r_.Flag("mali.vertexPipelineStoresAndAtomics_enabled", false);
    vkGetDeviceQueue(device_, family_, 0, &queue_);
    Present(window);
    r_.Flag("vulkan.initialization_and_present", true);
    r_.Text("phase", "ubo_graphics_probe");
    r_.Text("mali.scope", "Independent UBO shaders and descriptor allocations; no BO2 shaders, textures, sampling or runtime");
    Publish(r_);
    r_.Flag("mali.probe_attempted", descriptors);
    r_.Flag("graphics.probe_attempted", descriptors);
    if (descriptors) {
      const auto probe = ProbeUboGraphics(physical_, device_, queue_, family_);
      r_.Flag("mali.bo2_descriptor_layouts_supported", probe.layout_support);
      r_.Flag("mali.bo2_descriptor_sets_allocated", probe.descriptor_allocation);
      r_.Flag("mali.ubo_pipeline_created", probe.pipeline_created);
      r_.Flag("mali.ubo_draw_submitted", probe.draw_submitted);
      r_.Flag("mali.ubo_pixels_match", probe.pixels_match);
      r_.Number("mali.ubo_pixels_checked", probe.pixels_checked);
      r_.Number("mali.ubo_max_channel_error", probe.max_channel_error);
      r_.Number("mali.ubo_first_pixel_rgba", probe.first_pixel_rgba);
      r_.Number("mali.ubo_expected_pixel_rgba", 0x3399CCFFULL);
      if (!probe.error.empty()) r_.Text("mali.error", probe.error);
      r_.Text("phase", "port_graphics_probe");
      r_.Text("graphics.scope", "Authored entry points with BO2 shared HLSL header/tfetch2D; BC UNORM to RGBA8; no game shaders, tiling, resources or native renderer integration");
      r_.Text("graphics.shared_header_sha256", PortProbeShaderHeaderSha256());
      r_.Flag("graphics.shaderInt64_enabled", false);
      r_.Flag("graphics.vertexPipelineStoresAndAtomics_enabled", false);
      r_.Flag("graphics.textureCompressionBC_enabled", false);
      r_.Flag("graphics.game_renderer_integrated", false);
      r_.Number("graphics.sampled_image_index", 7);
      r_.Number("graphics.sampler_index", 3);
      r_.Flag("graphics.nonzero_dynamic_ubo_offsets", true);
      Publish(r_);
      const auto portUbo = ProbePortUboGraphics(physical_, device_, queue_, family_);
      RecordGraphics(r_, "graphics.port_ubo", portUbo);
      bool allPassed = portUbo.pixels_match && portUbo.error.empty();
      using bo2zm::texture::BcFormat;
      const std::pair<BcFormat, const char*> cases[] = {
          {BcFormat::BC1, "bc1"}, {BcFormat::BC2, "bc2"}, {BcFormat::BC3, "bc3"},
          {BcFormat::BC4, "bc4"}, {BcFormat::BC5, "bc5"}};
      uint32_t totalPixels = portUbo.pixels_checked;
      for (const auto& [format, name] : cases) {
        r_.Text("phase", std::string("port_texture_") + name);
        Publish(r_);
        const auto texture = ProbePortBcGraphics(physical_, device_, queue_, family_, format);
        RecordGraphics(r_, std::string("graphics.") + name, texture, true);
        allPassed = allPassed && texture.cpu_decode_match && texture.texture_uploaded &&
                    texture.pixels_match && texture.error.empty();
        totalPixels += texture.pixels_checked;
        Publish(r_);
      }
      r_.Flag("graphics.passed", allPassed);
      r_.Number("graphics.total_pixels_checked", totalPixels);
    } else {
      r_.Text("mali.skip_reason", "Required descriptor-indexing feature set missing");
    }
  }


 private:
  void Check(VkResult result, const std::string& operation) {
    r_.Number("vulkan.results." + operation, result);
    if (result != VK_SUCCESS) throw std::runtime_error(operation + " VkResult=" + std::to_string(result));
  }

  void Present(SDL_Window* window) {
    VkSurfaceCapabilitiesKHR caps{};
    Check(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(physical_, surface_, &caps), "surface_capabilities");
    uint32_t count = 0;
    Check(vkGetPhysicalDeviceSurfaceFormatsKHR(physical_, surface_, &count, nullptr), "surface_formats_count");
    if (!count) throw std::runtime_error("No surface formats");
    std::vector<VkSurfaceFormatKHR> formats(count);
    Check(vkGetPhysicalDeviceSurfaceFormatsKHR(physical_, surface_, &count, formats.data()), "surface_formats");
    auto format = formats.front();
    for (const auto& candidate : formats) {
      if (candidate.format == VK_FORMAT_R8G8B8A8_UNORM || candidate.format == VK_FORMAT_B8G8R8A8_UNORM) {
        format = candidate;
        break;
      }
    }
    if (format.format == VK_FORMAT_UNDEFINED) format.format = VK_FORMAT_R8G8B8A8_UNORM;
    if (!(caps.supportedUsageFlags & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT))
      throw std::runtime_error("Surface has no color attachment usage");
    VkExtent2D extent = caps.currentExtent;
    if (extent.width == UINT32_MAX) {
      int w = 0, h = 0;
      SDL_GetWindowSizeInPixels(window, &w, &h);
      extent.width = std::clamp(uint32_t(std::max(1, w)), caps.minImageExtent.width, caps.maxImageExtent.width);
      extent.height = std::clamp(uint32_t(std::max(1, h)), caps.minImageExtent.height, caps.maxImageExtent.height);
    }
    if (!extent.width || !extent.height) throw std::runtime_error("Surface is temporarily zero sized; reopen app");
    auto composite = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    for (auto candidate : {VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR, VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR,
                          VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR, VK_COMPOSITE_ALPHA_POST_MULTIPLIED_BIT_KHR}) {
      if (caps.supportedCompositeAlpha & candidate) { composite = candidate; break; }
    }
    VkSwapchainCreateInfoKHR info{};
    info.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
    info.surface = surface_;
    info.minImageCount = caps.minImageCount + 1;
    if (caps.maxImageCount) info.minImageCount = std::min(info.minImageCount, caps.maxImageCount);
    info.imageFormat = format.format;
    info.imageColorSpace = format.colorSpace;
    info.imageExtent = extent;
    info.imageArrayLayers = 1;
    info.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    info.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    info.preTransform = caps.currentTransform;
    info.compositeAlpha = composite;
    info.presentMode = VK_PRESENT_MODE_FIFO_KHR;
    info.clipped = VK_TRUE;
    Check(vkCreateSwapchainKHR(device_, &info, nullptr, &swapchain_), "create_swapchain");
    r_.Number("vulkan.surface_width", extent.width);
    r_.Number("vulkan.surface_height", extent.height);
    Check(vkGetSwapchainImagesKHR(device_, swapchain_, &count, nullptr), "swapchain_images_count");
    if (!count) throw std::runtime_error("Empty swapchain");
    std::vector<VkImage> images(count);
    Check(vkGetSwapchainImagesKHR(device_, swapchain_, &count, images.data()), "swapchain_images");
    VkAttachmentDescription attachment{};
    attachment.format = format.format;
    attachment.samples = VK_SAMPLE_COUNT_1_BIT;
    attachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    attachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    attachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    attachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    attachment.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    VkAttachmentReference reference{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkSubpassDescription subpass{};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 1;
    subpass.pColorAttachments = &reference;
    VkSubpassDependency dependency{};
    dependency.srcSubpass = VK_SUBPASS_EXTERNAL;
    dependency.dstSubpass = 0;
    dependency.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dependency.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dependency.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    VkRenderPassCreateInfo passInfo{};
    passInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    passInfo.attachmentCount = 1;
    passInfo.pAttachments = &attachment;
    passInfo.subpassCount = 1;
    passInfo.pSubpasses = &subpass;
    passInfo.dependencyCount = 1;
    passInfo.pDependencies = &dependency;
    Check(vkCreateRenderPass(device_, &passInfo, nullptr, &pass_), "create_render_pass");
    for (auto image : images) {
      VkImageViewCreateInfo vi{};
      vi.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
      vi.image = image;
      vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
      vi.format = format.format;
      vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
      VkImageView view = VK_NULL_HANDLE;
      Check(vkCreateImageView(device_, &vi, nullptr, &view), "create_image_view");
      views_.push_back(view);
      VkFramebufferCreateInfo fi{};
      fi.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
      fi.renderPass = pass_;
      fi.attachmentCount = 1;
      fi.pAttachments = &views_.back();
      fi.width = extent.width;
      fi.height = extent.height;
      fi.layers = 1;
      VkFramebuffer framebuffer = VK_NULL_HANDLE;
      Check(vkCreateFramebuffer(device_, &fi, nullptr, &framebuffer), "create_framebuffer");
      framebuffers_.push_back(framebuffer);
    }
    VkCommandPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    poolInfo.queueFamilyIndex = family_;
    Check(vkCreateCommandPool(device_, &poolInfo, nullptr, &pool_), "create_command_pool");
    VkCommandBufferAllocateInfo ai{};
    ai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    ai.commandPool = pool_;
    ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    ai.commandBufferCount = 1;
    VkCommandBuffer commands = VK_NULL_HANDLE;
    Check(vkAllocateCommandBuffers(device_, &ai, &commands), "allocate_commands");
    VkSemaphoreCreateInfo si{};
    si.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
    Check(vkCreateSemaphore(device_, &si, nullptr, &acquired_), "create_acquire_semaphore");
    Check(vkCreateSemaphore(device_, &si, nullptr, &rendered_), "create_render_semaphore");
    VkFenceCreateInfo fenceInfo{};
    fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    Check(vkCreateFence(device_, &fenceInfo, nullptr, &fence_), "create_fence");
    uint32_t index = 0;
    const VkResult acquire = vkAcquireNextImageKHR(device_, swapchain_, 5000000000ULL, acquired_, VK_NULL_HANDLE, &index);
    r_.Number("vulkan.results.acquire_image", acquire);
    if (acquire != VK_SUCCESS && acquire != VK_SUBOPTIMAL_KHR) Check(acquire, "acquire_image");
    VkCommandBufferBeginInfo bi{};
    bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    Check(vkBeginCommandBuffer(commands, &bi), "begin_commands");
    VkClearValue clear{};
    clear.color.float32[0] = 0.03f;
    clear.color.float32[1] = 0.08f;
    clear.color.float32[2] = 0.14f;
    clear.color.float32[3] = 1.0f;
    VkRenderPassBeginInfo begin{};
    begin.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    begin.renderPass = pass_;
    begin.framebuffer = framebuffers_.at(index);
    begin.renderArea.extent = extent;
    begin.clearValueCount = 1;
    begin.pClearValues = &clear;
    vkCmdBeginRenderPass(commands, &begin, VK_SUBPASS_CONTENTS_INLINE);
    vkCmdEndRenderPass(commands);
    Check(vkEndCommandBuffer(commands), "end_commands");
    VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    VkSubmitInfo submit{};
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit.waitSemaphoreCount = 1;
    submit.pWaitSemaphores = &acquired_;
    submit.pWaitDstStageMask = &waitStage;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &commands;
    submit.signalSemaphoreCount = 1;
    submit.pSignalSemaphores = &rendered_;
    Check(vkQueueSubmit(queue_, 1, &submit, fence_), "queue_submit");
    Check(vkWaitForFences(device_, 1, &fence_, VK_TRUE, 5000000000ULL), "wait_gpu_fence");
    VkPresentInfoKHR present{};
    present.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
    present.waitSemaphoreCount = 1;
    present.pWaitSemaphores = &rendered_;
    present.swapchainCount = 1;
    present.pSwapchains = &swapchain_;
    present.pImageIndices = &index;
    const VkResult result = vkQueuePresentKHR(queue_, &present);
    r_.Number("vulkan.results.queue_present", result);
    if (result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR) Check(result, "queue_present");
    Check(vkQueueWaitIdle(queue_), "queue_wait_idle");
  }

  Report& r_;
  VkInstance instance_ = VK_NULL_HANDLE;
  VkSurfaceKHR surface_ = VK_NULL_HANDLE;
  VkPhysicalDevice physical_ = VK_NULL_HANDLE;
  VkDevice device_ = VK_NULL_HANDLE;
  VkQueue queue_ = VK_NULL_HANDLE;
  uint32_t family_ = 0;
  VkSwapchainKHR swapchain_ = VK_NULL_HANDLE;
  VkRenderPass pass_ = VK_NULL_HANDLE;
  VkCommandPool pool_ = VK_NULL_HANDLE;
  VkSemaphore acquired_ = VK_NULL_HANDLE, rendered_ = VK_NULL_HANDLE;
  VkFence fence_ = VK_NULL_HANDLE;
  std::vector<VkImageView> views_;
  std::vector<VkFramebuffer> framebuffers_;
};
}

int main(int, char**) {
  Report report;
  report.Text("phase", "native_loaded");
  report.Text("upstream.bo2_commit", "4bab074a60e8ceb608e669e54961d2d06983fc98");
  report.Text("upstream.android_reference_commit", "0a79f938ab839c187d25bd7bf48e51e30a6150be");
  report.Text("scope", "Standalone platform tests; runtime results are reported separately");
  report.Number("native.pointer_bits", sizeof(void*) * 8);
  report.Number("native.sdl_version", SDL_GetVersion());
  utsname kernel{};
  if (uname(&kernel) == 0) report.Text("native.kernel", std::string(kernel.release) + " " + kernel.machine);
  const bool video = SDL_Init(SDL_INIT_VIDEO);
  report.Flag("sdl.video_initialized", video);
  if (!video) report.Text("sdl.error", SDL_GetError());
  Publish(report);
  SDL_Window* window = nullptr;
  try {
    ProbeMemory(report);
    report.Text("phase", "memory_probed");
    Publish(report);
    ProbeThreads(report);
    report.Text("phase", "threads_probed");
    Publish(report);
    if (!video) throw std::runtime_error("SDL video initialization failed");
    window = SDL_CreateWindow("BO2 Diagnostic", 720, 1280, SDL_WINDOW_VULKAN);
    if (!window) throw std::runtime_error(SDL_GetError());
    report.Flag("sdl.vulkan_window", true);
    {
      VulkanProbe vulkan(report);
      vulkan.Run(window);
    }
#ifdef BO2_HAS_SDK_PROVIDER
    report.Text("phase", "sdk_provider_probe");
    Publish(report);
    const auto progress = [](void* context, const char* stage) {
      auto& r = *static_cast<Report*>(context); r.Text("phase", stage); Publish(r);
    };
    auto* env = static_cast<JNIEnv*>(SDL_GetAndroidJNIEnv());
    auto activity = static_cast<jobject>(SDL_GetAndroidActivity());
    bool runSdk = false;
    if (env && activity) {
      const auto cls = env->GetObjectClass(activity);
      const auto method = env->GetMethodID(cls, "shouldRunSdkProvider", "()Z");
      if (method) runSdk = env->CallBooleanMethod(activity, method);
      if (env->ExceptionCheck()) { env->ExceptionClear(); runSdk = false; }
      env->DeleteLocalRef(cls); env->DeleteLocalRef(activity);
    }
    if (runSdk) {
    report.Flag("sdk_renderer.probe_attempted", true);
    const auto sdk = ProbeSdkProvider(progress, &report);
    report.Text("sdk_renderer.scope", "Real SDK native provider/presenter; shared production BC mip converter and upload regions; authored shaders/textures; full renderer and game not linked/executed");
    report.Flag("sdk_renderer.provider_created", sdk.provider_created);
    report.Flag("sdk_renderer.presenter_created", sdk.presenter_created);
    report.Flag("sdk_renderer.ubo_only", sdk.ubo_only);
    report.Flag("sdk_renderer.shaderInt64_enabled", sdk.shader_int64_enabled);
    report.Flag("sdk_renderer.vertexPipelineStoresAndAtomics_enabled", sdk.vertex_stores_enabled);
    report.Flag("sdk_renderer.descriptor_features_enabled", sdk.descriptor_features_enabled);
    report.Flag("sdk_renderer.destroyed", sdk.destroyed);
    report.Flag("sdk_renderer.full_renderer_linked", false);
    report.Flag("sdk_renderer.game_entry_executed", false);
    report.Text("sdk_renderer.gpu", sdk.gpu);
    RecordGraphics(report, "sdk_renderer.ubo", sdk.ubo);
    const char* names[] = {"bc1", "bc2", "bc3", "bc4", "bc5"};
    for (unsigned i = 0; i < 5; ++i) RecordGraphics(report, std::string("sdk_renderer.") + names[i], sdk.textures[i], true);
    report.Flag("sdk_renderer.passed", sdk.passed);
    report.Number("sdk_renderer.pixels_checked", sdk.pixels_checked);
    report.Number("sdk_renderer.subresources_checked", sdk.subresources_checked);
    if (!sdk.error.empty()) report.Text("sdk_renderer.error", sdk.error);
    if (!sdk.sdk_messages.empty()) report.Text("sdk_renderer.sdk_warnings", sdk.sdk_messages);
    } else {
      report.Text("sdk_renderer.skip_reason", "SDK attempt interrupted previously; export previous_sdk_checkpoint; current SDK probe not executed");
      report.Flag("sdk_renderer.probe_attempted", false);
    }
#else
    report.Text("sdk_renderer.skip_reason", "Build without native runtime libraries");
#endif
    report.Text("phase", "diagnostic_complete");
    Publish(report);
  } catch (const std::exception& error) {
    report.Text("phase", "diagnostic_error");
    report.Text("error", error.what());
    Publish(report);
  }


  bool running = true;
  while (running) {
    SDL_Event event;
    if (SDL_WaitEventTimeout(&event, 250) && event.type == SDL_EVENT_QUIT) running = false;
  }
  if (window) SDL_DestroyWindow(window);
  SDL_Quit();
  return 0;
}
