#include <vulkan/vulkan.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <random>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

constexpr uint32_t kSize = 32;
constexpr uint32_t kHeaps[4] = {16384, 64, 256, 512};
constexpr float kSentinel = -12345.0f;

void Check(VkResult r, const char* what) {
  if (r != VK_SUCCESS) throw std::runtime_error(std::string(what) + " failed: " + std::to_string(int(r)));
}

std::vector<uint32_t> ReadWords(const std::string& path) {
  std::ifstream f(path, std::ios::binary);
  std::vector<char> bytes((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
  if (bytes.empty() || bytes.size() % 4) throw std::runtime_error("cannot read " + path);
  std::vector<uint32_t> words(bytes.size() / 4);
  std::memcpy(words.data(), bytes.data(), bytes.size());
  return words;
}

struct Context {
  VkInstance instance{};
  VkPhysicalDevice physical{};
  VkDevice device{};
  VkQueue queue{};
  uint32_t family = 0;
  bool has_xfb = false;
  VkPhysicalDeviceMemoryProperties memory{};

  uint32_t Type(uint32_t bits, VkMemoryPropertyFlags want) const {
    for (uint32_t i = 0; i < memory.memoryTypeCount; ++i)
      if ((bits & (1u << i)) && (memory.memoryTypes[i].propertyFlags & want) == want) return i;
    throw std::runtime_error("no memory type");
  }

  void Init() {
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.apiVersion = VK_API_VERSION_1_2;
    VkInstanceCreateInfo ii{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ii.pApplicationInfo = &app;
    Check(vkCreateInstance(&ii, nullptr, &instance), "vkCreateInstance");
    uint32_t n = 1;
    VkResult r = vkEnumeratePhysicalDevices(instance, &n, &physical);
    if (r != VK_SUCCESS && r != VK_INCOMPLETE) Check(r, "vkEnumeratePhysicalDevices");
    if (!n) throw std::runtime_error("no Vulkan device");
    vkGetPhysicalDeviceMemoryProperties(physical, &memory);
    float priority = 1;
    VkDeviceQueueCreateInfo qi{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    qi.queueFamilyIndex = family;
    qi.queueCount = 1;
    qi.pQueuePriorities = &priority;
    VkPhysicalDeviceVulkan12Features f12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
    f12.runtimeDescriptorArray = VK_TRUE;
    f12.descriptorBindingPartiallyBound = VK_TRUE;
    f12.scalarBlockLayout = VK_TRUE;
    f12.descriptorIndexing = VK_TRUE;

    VkPhysicalDeviceTransformFeedbackFeaturesEXT xfb{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TRANSFORM_FEEDBACK_FEATURES_EXT};
    VkPhysicalDeviceFeatures2 query{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
    query.pNext = &xfb;
    vkGetPhysicalDeviceFeatures2(physical, &query);
    has_xfb = xfb.transformFeedback == VK_TRUE;
    f12.pNext = has_xfb ? &xfb : nullptr;
    const char* extensions[] = {VK_EXT_TRANSFORM_FEEDBACK_EXTENSION_NAME};
    VkPhysicalDeviceFeatures2 f2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
    f2.pNext = &f12;
    f2.features.shaderSampledImageArrayDynamicIndexing = VK_TRUE;
    VkDeviceCreateInfo di{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    di.pNext = &f2;
    di.queueCreateInfoCount = 1;
    di.pQueueCreateInfos = &qi;
    di.enabledExtensionCount = has_xfb ? 1 : 0;
    di.ppEnabledExtensionNames = extensions;
    Check(vkCreateDevice(physical, &di, nullptr, &device), "vkCreateDevice");
    vkGetDeviceQueue(device, family, 0, &queue);
  }
};

struct Buffer {
  VkBuffer buffer{};
  VkDeviceMemory memory{};
  void* map = nullptr;
  void Make(Context& c, VkDeviceSize size, VkBufferUsageFlags usage) {
    VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bi.size = size;
    bi.usage = usage;
    Check(vkCreateBuffer(c.device, &bi, nullptr, &buffer), "vkCreateBuffer");
    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(c.device, buffer, &req);
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = c.Type(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    Check(vkAllocateMemory(c.device, &ai, nullptr, &memory), "vkAllocateMemory");
    vkBindBufferMemory(c.device, buffer, memory, 0);
    vkMapMemory(c.device, memory, 0, size, 0, &map);
  }
};

struct Image {
  VkImage image{};
  VkDeviceMemory memory{};
  VkImageView view{};
};

class Harness {
 public:
  explicit Harness(const std::vector<uint32_t>& vertex) {
    c_.Init();
    std::mt19937 rng(1234);
    std::uniform_real_distribution<float> u(-2.f, 2.f);
    MakeTarget();
    MakeTextures(rng);
    MakeLayouts();
    MakeConstants(rng);
    VkShaderModuleCreateInfo mi{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    mi.codeSize = vertex.size() * 4;
    mi.pCode = vertex.data();
    Check(vkCreateShaderModule(c_.device, &mi, nullptr, &vertex_), "vertex module");
    VkCommandPoolCreateInfo pi{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pi.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pi.queueFamilyIndex = c_.family;
    Check(vkCreateCommandPool(c_.device, &pi, nullptr, &pool_), "command pool");
    VkCommandBufferAllocateInfo ci{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    ci.commandPool = pool_;
    ci.commandBufferCount = 1;
    Check(vkAllocateCommandBuffers(c_.device, &ci, &cmd_), "command buffer");
    readback_.Make(c_, kSize * kSize * 16, VK_BUFFER_USAGE_TRANSFER_DST_BIT);

    vertices_float_.Make(c_, 16 * kVertices * 16, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);
    vertices_uint_.Make(c_, 16 * kVertices * 16, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);
    if (c_.has_xfb) {
      xfb_.Make(c_, kVertices * kMaxXfbStride, VK_BUFFER_USAGE_TRANSFORM_FEEDBACK_BUFFER_BIT_EXT);
      bind_xfb_ = reinterpret_cast<PFN_vkCmdBindTransformFeedbackBuffersEXT>(
          vkGetDeviceProcAddr(c_.device, "vkCmdBindTransformFeedbackBuffersEXT"));
      begin_xfb_ = reinterpret_cast<PFN_vkCmdBeginTransformFeedbackEXT>(
          vkGetDeviceProcAddr(c_.device, "vkCmdBeginTransformFeedbackEXT"));
      end_xfb_ = reinterpret_cast<PFN_vkCmdEndTransformFeedbackEXT>(
          vkGetDeviceProcAddr(c_.device, "vkCmdEndTransformFeedbackEXT"));
    }
    std::uniform_real_distribution<float> position(-1.5f, 1.5f);
    for (uint32_t i = 0; i < 16 * kVertices * 4; ++i) {
      static_cast<float*>(vertices_float_.map)[i] = (i % 4 == 3) ? 1.0f : position(rng);
      static_cast<uint32_t*>(vertices_uint_.map)[i] = uint32_t(rng() % 8);
    }
  }
  static constexpr uint32_t kVertices = 96;
  static constexpr uint32_t kMaxXfbStride = 16 * 20;
  bool has_xfb() const { return c_.has_xfb; }

  struct Input {
    uint32_t location;
    bool integer;
  };


  std::vector<float> Draw(const std::vector<uint32_t>& pixel, uint32_t spec,
                          const std::vector<uint32_t>* vertex = nullptr, const std::vector<Input>& inputs = {},
                          uint32_t xfb_stride = 0) {
    if (xfb_stride && !c_.has_xfb) return {};
    VkShaderModuleCreateInfo mi{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    mi.codeSize = pixel.size() * 4;
    mi.pCode = pixel.data();
    VkShaderModule module{}, vertex_module{};
    if (vkCreateShaderModule(c_.device, &mi, nullptr, &module) != VK_SUCCESS) return {};
    if (vertex) {
      mi.codeSize = vertex->size() * 4;
      mi.pCode = vertex->data();
      if (vkCreateShaderModule(c_.device, &mi, nullptr, &vertex_module) != VK_SUCCESS) {
        vkDestroyShaderModule(c_.device, module, nullptr);
        return {};
      }
    }
    VkSpecializationMapEntry entry{0, 0, 4};
    VkSpecializationInfo si{1, &entry, 4, &spec};
    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_VERTEX_BIT,
                 vertex ? vertex_module : vertex_, "main", vertex ? &si : nullptr};
    stages[1] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_FRAGMENT_BIT, module, "main", &si};
    std::vector<VkVertexInputBindingDescription> bindings;
    std::vector<VkVertexInputAttributeDescription> attributes;
    for (const Input& in : inputs) {
      bindings.push_back({uint32_t(bindings.size()), 16, VK_VERTEX_INPUT_RATE_VERTEX});
      attributes.push_back({in.location, bindings.back().binding,
                            in.integer ? VK_FORMAT_R32G32B32A32_UINT : VK_FORMAT_R32G32B32A32_SFLOAT, 0});
    }
    VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    vi.vertexBindingDescriptionCount = uint32_t(bindings.size());
    vi.pVertexBindingDescriptions = bindings.data();
    vi.vertexAttributeDescriptionCount = uint32_t(attributes.size());
    vi.pVertexAttributeDescriptions = attributes.data();
    VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkViewport viewport{0, 0, float(kSize), float(kSize), 0, 1};
    VkRect2D scissor{{0, 0}, {kSize, kSize}};
    VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    vp.viewportCount = 1;
    vp.pViewports = &viewport;
    vp.scissorCount = 1;
    vp.pScissors = &scissor;
    VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.rasterizerDiscardEnable = xfb_stride ? VK_TRUE : VK_FALSE;
    rs.cullMode = VK_CULL_MODE_NONE;
    rs.lineWidth = 1;
    VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineColorBlendAttachmentState blend{};
    blend.colorWriteMask = 0xF;
    VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    cb.attachmentCount = 1;
    cb.pAttachments = &blend;
    VkGraphicsPipelineCreateInfo gi{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    gi.stageCount = 2;
    gi.pStages = stages;
    gi.pVertexInputState = &vi;
    gi.pInputAssemblyState = &ia;
    gi.pViewportState = &vp;
    gi.pRasterizationState = &rs;
    gi.pMultisampleState = &ms;
    gi.pColorBlendState = &cb;
    gi.layout = layout_;
    gi.renderPass = pass_;
    VkPipeline pipeline{};
    const VkResult made = vkCreateGraphicsPipelines(c_.device, VK_NULL_HANDLE, 1, &gi, nullptr, &pipeline);
    vkDestroyShaderModule(c_.device, module, nullptr);
    if (vertex_module) vkDestroyShaderModule(c_.device, vertex_module, nullptr);
    if (made != VK_SUCCESS) return {};
    vkResetCommandBuffer(cmd_, 0);
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    vkBeginCommandBuffer(cmd_, &bi);
    VkClearValue clear{};
    clear.color.float32[0] = clear.color.float32[1] = clear.color.float32[2] = clear.color.float32[3] = kSentinel;
    VkRenderPassBeginInfo rp{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
    rp.renderPass = pass_;
    rp.framebuffer = framebuffer_;
    rp.renderArea = scissor;
    rp.clearValueCount = 1;
    rp.pClearValues = &clear;
    vkCmdBeginRenderPass(cmd_, &rp, VK_SUBPASS_CONTENTS_INLINE);
    vkCmdBindPipeline(cmd_, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
    vkCmdBindDescriptorSets(cmd_, VK_PIPELINE_BIND_POINT_GRAPHICS, layout_, 0, 5, sets_, 0, nullptr);
    for (uint32_t b = 0; b < inputs.size(); ++b) {
      const Buffer& source = inputs[b].integer ? vertices_uint_ : vertices_float_;
      const VkDeviceSize offset = VkDeviceSize(inputs[b].location) * kVertices * 16;
      vkCmdBindVertexBuffers(cmd_, b, 1, &source.buffer, &offset);
    }
    if (xfb_stride) {
      std::memset(xfb_.map, 0xFF, kVertices * kMaxXfbStride);
      const VkDeviceSize zero = 0, size = VkDeviceSize(kVertices) * xfb_stride;
      bind_xfb_(cmd_, 0, 1, &xfb_.buffer, &zero, &size);
      begin_xfb_(cmd_, 0, 0, nullptr, nullptr);
    }
    vkCmdDraw(cmd_, vertex ? kVertices : 3, 1, 0, 0);
    if (xfb_stride) end_xfb_(cmd_, 0, 0, nullptr, nullptr);
    vkCmdEndRenderPass(cmd_);
    if (xfb_stride) {
      VkMemoryBarrier host{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
      host.srcAccessMask = VK_ACCESS_TRANSFORM_FEEDBACK_WRITE_BIT_EXT;
      host.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
      vkCmdPipelineBarrier(cmd_, VK_PIPELINE_STAGE_TRANSFORM_FEEDBACK_BIT_EXT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &host,
                           0, nullptr, 0, nullptr);
    }
    VkBufferImageCopy copy{};
    copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    copy.imageExtent = {kSize, kSize, 1};
    vkCmdCopyImageToBuffer(cmd_, target_.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, readback_.buffer, 1, &copy);
    vkEndCommandBuffer(cmd_);
    VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &cmd_;
    Check(vkQueueSubmit(c_.queue, 1, &submit, VK_NULL_HANDLE), "submit");
    Check(vkQueueWaitIdle(c_.queue), "wait");
    vkDestroyPipeline(c_.device, pipeline, nullptr);
    if (xfb_stride) {
      std::vector<float> captured(size_t(kVertices) * xfb_stride / 4);
      std::memcpy(captured.data(), xfb_.map, captured.size() * 4);
      return captured;
    }
    std::vector<float> out(kSize * kSize * 4);
    std::memcpy(out.data(), readback_.map, out.size() * 4);
    return out;
  }

 private:
  void MakeImage(Image& img, VkImageType type, VkImageViewType view_type, VkFormat format, VkExtent3D extent,
                 uint32_t layers, VkImageUsageFlags usage, VkImageCreateFlags flags = 0) {
    VkImageCreateInfo ii{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ii.flags = flags;
    ii.imageType = type;
    ii.format = format;
    ii.extent = extent;
    ii.mipLevels = 1;
    ii.arrayLayers = layers;
    ii.samples = VK_SAMPLE_COUNT_1_BIT;
    ii.tiling = VK_IMAGE_TILING_OPTIMAL;
    ii.usage = usage;
    Check(vkCreateImage(c_.device, &ii, nullptr, &img.image), "vkCreateImage");
    VkMemoryRequirements req;
    vkGetImageMemoryRequirements(c_.device, img.image, &req);
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = c_.Type(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    Check(vkAllocateMemory(c_.device, &ai, nullptr, &img.memory), "image memory");
    vkBindImageMemory(c_.device, img.image, img.memory, 0);
    VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vi.image = img.image;
    vi.viewType = view_type;
    vi.format = format;
    vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, layers};
    Check(vkCreateImageView(c_.device, &vi, nullptr, &img.view), "vkCreateImageView");
  }

  void MakeTarget() {
    MakeImage(target_, VK_IMAGE_TYPE_2D, VK_IMAGE_VIEW_TYPE_2D, VK_FORMAT_R32G32B32A32_SFLOAT, {kSize, kSize, 1}, 1,
              VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
    VkAttachmentDescription at{};
    at.format = VK_FORMAT_R32G32B32A32_SFLOAT;
    at.samples = VK_SAMPLE_COUNT_1_BIT;
    at.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    at.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    at.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    at.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    at.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    at.finalLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    VkAttachmentReference ref{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkSubpassDescription sub{};
    sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    sub.colorAttachmentCount = 1;
    sub.pColorAttachments = &ref;
    VkSubpassDependency dep{};
    dep.srcSubpass = 0;
    dep.dstSubpass = VK_SUBPASS_EXTERNAL;
    dep.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dep.dstStageMask = VK_PIPELINE_STAGE_TRANSFER_BIT;
    dep.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    dep.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    VkRenderPassCreateInfo ri{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
    ri.attachmentCount = 1;
    ri.pAttachments = &at;
    ri.subpassCount = 1;
    ri.pSubpasses = &sub;
    ri.dependencyCount = 1;
    ri.pDependencies = &dep;
    Check(vkCreateRenderPass(c_.device, &ri, nullptr, &pass_), "render pass");
    VkFramebufferCreateInfo fi{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
    fi.renderPass = pass_;
    fi.attachmentCount = 1;
    fi.pAttachments = &target_.view;
    fi.width = fi.height = kSize;
    fi.layers = 1;
    Check(vkCreateFramebuffer(c_.device, &fi, nullptr, &framebuffer_), "framebuffer");
  }

  void Upload(Image& img, VkExtent3D extent, uint32_t layers, std::mt19937& rng) {
    const VkDeviceSize bytes = VkDeviceSize(extent.width) * extent.height * extent.depth * layers * 4;
    Buffer staging;
    staging.Make(c_, bytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
    auto* p = static_cast<uint8_t*>(staging.map);
    for (VkDeviceSize i = 0; i < bytes; ++i) p[i] = uint8_t(rng());
    VkCommandPool pool{};
    VkCommandPoolCreateInfo pi{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pi.queueFamilyIndex = c_.family;
    vkCreateCommandPool(c_.device, &pi, nullptr, &pool);
    VkCommandBuffer cmd{};
    VkCommandBufferAllocateInfo ci{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    ci.commandPool = pool;
    ci.commandBufferCount = 1;
    vkAllocateCommandBuffers(c_.device, &ci, &cmd);
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    vkBeginCommandBuffer(cmd, &bi);
    VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    b.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = img.image;
    b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, layers};
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0,
                         nullptr, 1, &b);
    VkBufferImageCopy copy{};
    copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, layers};
    copy.imageExtent = extent;
    vkCmdCopyBufferToImage(cmd, staging.buffer, img.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
    b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    b.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    b.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0,
                         nullptr, 1, &b);
    vkEndCommandBuffer(cmd);
    VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &cmd;
    vkQueueSubmit(c_.queue, 1, &submit, VK_NULL_HANDLE);
    vkQueueWaitIdle(c_.queue);
    vkDestroyCommandPool(c_.device, pool, nullptr);
  }


  void MakeTextures(std::mt19937& rng) {
    const VkImageUsageFlags usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    for (uint32_t v = 0; v < kVariants; ++v) {
      MakeImage(tex2d_[v], VK_IMAGE_TYPE_2D, VK_IMAGE_VIEW_TYPE_2D, VK_FORMAT_R8G8B8A8_UNORM, {16, 16, 1}, 1, usage);
      Upload(tex2d_[v], {16, 16, 1}, 1, rng);
      MakeImage(tex3d_[v], VK_IMAGE_TYPE_3D, VK_IMAGE_VIEW_TYPE_3D, VK_FORMAT_R8G8B8A8_UNORM, {8, 8, 8}, 1, usage);
      Upload(tex3d_[v], {8, 8, 8}, 1, rng);
      MakeImage(cube_[v], VK_IMAGE_TYPE_2D, VK_IMAGE_VIEW_TYPE_CUBE, VK_FORMAT_R8G8B8A8_UNORM, {8, 8, 1}, 6, usage,
                VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT);
      Upload(cube_[v], {8, 8, 1}, 6, rng);
      VkSamplerCreateInfo si{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
      si.magFilter = si.minFilter = (v & 1) ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
      si.addressModeU = si.addressModeV = si.addressModeW =
          (v & 2) ? VK_SAMPLER_ADDRESS_MODE_REPEAT : VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
      si.maxLod = 0;
      Check(vkCreateSampler(c_.device, &si, nullptr, &sampler_[v]), "sampler");
    }
  }

  void MakeLayouts() {
    const VkDescriptorType types[4] = {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
                                       VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, VK_DESCRIPTOR_TYPE_SAMPLER};
    VkDescriptorPoolSize sizes[2] = {{VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, kHeaps[0] + kHeaps[1] + kHeaps[2]},
                                     {VK_DESCRIPTOR_TYPE_SAMPLER, kHeaps[3]}};
    VkDescriptorPoolSize all[3] = {sizes[0], sizes[1], {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 3}};
    VkDescriptorPoolCreateInfo pi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    pi.maxSets = 5;
    pi.poolSizeCount = 3;
    pi.pPoolSizes = all;
    Check(vkCreateDescriptorPool(c_.device, &pi, nullptr, &descriptor_pool_), "descriptor pool");
    for (uint32_t i = 0; i < 4; ++i) {
      VkDescriptorSetLayoutBinding b{0, types[i], kHeaps[i], VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT};
      const VkDescriptorBindingFlags flag = VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT;
      VkDescriptorSetLayoutBindingFlagsCreateInfo fi{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO};
      fi.bindingCount = 1;
      fi.pBindingFlags = &flag;
      VkDescriptorSetLayoutCreateInfo li{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
      li.pNext = &fi;
      li.bindingCount = 1;
      li.pBindings = &b;
      Check(vkCreateDescriptorSetLayout(c_.device, &li, nullptr, &set_layouts_[i]), "heap layout");
    }
    VkDescriptorSetLayoutBinding ubo[3];
    for (uint32_t i = 0; i < 3; ++i)
      ubo[i] = {i, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT};
    VkDescriptorSetLayoutCreateInfo li{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    li.bindingCount = 3;
    li.pBindings = ubo;
    Check(vkCreateDescriptorSetLayout(c_.device, &li, nullptr, &set_layouts_[4]), "ubo layout");
    VkPipelineLayoutCreateInfo pl{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pl.setLayoutCount = 5;
    pl.pSetLayouts = set_layouts_;
    Check(vkCreatePipelineLayout(c_.device, &pl, nullptr, &layout_), "pipeline layout");
    VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    ai.descriptorPool = descriptor_pool_;
    ai.descriptorSetCount = 5;
    ai.pSetLayouts = set_layouts_;
    Check(vkAllocateDescriptorSets(c_.device, &ai, sets_), "descriptor sets");
    for (uint32_t i = 0; i < 4; ++i) {
      std::vector<VkDescriptorImageInfo> infos(kHeaps[i]);
      for (uint32_t k = 0; k < kHeaps[i]; ++k) {
        auto& info = infos[k];
        const uint32_t v = k % kVariants;
        const VkImageView views[3] = {tex2d_[v].view, tex3d_[v].view, cube_[v].view};
        info.sampler = i == 3 ? sampler_[v] : VK_NULL_HANDLE;
        info.imageView = i == 3 ? VK_NULL_HANDLE : views[i];
        info.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
      }
      VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
      w.dstSet = sets_[i];
      w.descriptorCount = kHeaps[i];
      w.descriptorType = types[i];
      w.pImageInfo = infos.data();
      vkUpdateDescriptorSets(c_.device, 1, &w, 0, nullptr);
    }
  }

  void MakeConstants(std::mt19937& rng) {
    std::uniform_real_distribution<float> u(-2.f, 2.f);
    const VkDeviceSize sizes[3] = {256 * 16, 256 * 16, 43 * 16};
    for (uint32_t i = 0; i < 3; ++i) {
      ubos_[i].Make(c_, sizes[i], VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT);
      auto* f = static_cast<float*>(ubos_[i].map);
      for (VkDeviceSize k = 0; k < sizes[i] / 4; ++k) f[k] = u(rng);
    }

    auto* s = static_cast<uint8_t*>(ubos_[2].map);
    const auto put_u = [&](uint32_t at, uint32_t v) { std::memcpy(s + at, &v, 4); };
    const auto put_f = [&](uint32_t at, float v) { std::memcpy(s + at, &v, 4); };
    for (uint32_t at = 0; at < 256; at += 4) put_u(at, uint32_t(rng() % 32));
    put_u(256, uint32_t(rng()));
    put_u(260, uint32_t(rng()));
    put_f(264, 0.5f);
    put_f(268, 0.5f);
    put_f(272, 0.5f);
    put_u(276, 3);
    put_f(280, 1.f);
    put_f(284, 1.f);
    put_f(288, 0.f);
    put_f(292, 0.f);
    for (uint32_t loc = 0; loc < 16; ++loc) put_u(296 + loc * 4, 0xFFF);
    for (uint32_t slot = 0; slot < 16; ++slot) {
      put_f(360 + slot * 8, 1.f / 16);
      put_f(364 + slot * 8, 1.f / 16);
      put_f(488 + slot * 4, 1.f);
      put_f(560 + slot * 8, 1.f);
      put_f(564 + slot * 8, 1.f);
    }
    put_f(552, 1.f);
    put_f(556, 1.f);
    VkDescriptorBufferInfo infos[3];
    VkWriteDescriptorSet w[3];
    for (uint32_t i = 0; i < 3; ++i) {
      infos[i] = {ubos_[i].buffer, 0, sizes[i]};
      w[i] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
      w[i].dstSet = sets_[4];
      w[i].dstBinding = i;
      w[i].descriptorCount = 1;
      w[i].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
      w[i].pBufferInfo = &infos[i];
    }
    vkUpdateDescriptorSets(c_.device, 3, w, 0, nullptr);
  }

  Context c_;
  static constexpr uint32_t kVariants = 4;
  Image target_, tex2d_[kVariants], tex3d_[kVariants], cube_[kVariants];
  VkSampler sampler_[kVariants]{};
  VkRenderPass pass_{};
  VkFramebuffer framebuffer_{};
  VkDescriptorPool descriptor_pool_{};
  VkDescriptorSetLayout set_layouts_[5]{};
  VkDescriptorSet sets_[5]{};
  VkPipelineLayout layout_{};
  Buffer ubos_[3], readback_, vertices_float_, vertices_uint_, xfb_;
  PFN_vkCmdBindTransformFeedbackBuffersEXT bind_xfb_ = nullptr;
  PFN_vkCmdBeginTransformFeedbackEXT begin_xfb_ = nullptr;
  PFN_vkCmdEndTransformFeedbackEXT end_xfb_ = nullptr;
  VkShaderModule vertex_{};
  VkCommandPool pool_{};
  VkCommandBuffer cmd_{};
};

bool Close(float a, float b) {
  if (std::isnan(a) || std::isnan(b)) return std::isnan(a) && std::isnan(b);
  if (std::isinf(a) || std::isinf(b)) return a == b;
  const float d = std::fabs(a - b);
  return d <= 1e-4f || d <= 1e-4f * std::max(std::fabs(a), std::fabs(b));
}

}

int main(int argc, char** argv) try {
  if (argc != 3) {
    std::fprintf(stderr, "usage: compare_pixels <vertex.spv> <list>\n");
    return 2;
  }
  Harness harness(ReadWords(argv[1]));

  const uint32_t specs[] = {0u, (1u << 1) | (3u << 16), (1u << 1) | (6u << 16), 1u << 9, 1u << 15, 1u << 19, 1u << 23};
  std::ifstream list(argv[2]);
  std::string line;
  uint32_t same = 0, different = 0, skipped = 0;
  while (std::getline(list, line)) {
    std::istringstream in(line);
    std::string name, a_path, b_path, kind, ps_path, input_list;
    if (!(in >> name >> a_path >> b_path)) continue;
    const auto a = ReadWords(a_path), b = ReadWords(b_path);


    in >> kind;
    const bool vertex_test = (kind == "vs" || kind == "xfb") && (in >> ps_path);
    uint32_t xfb_stride = 0;
    if (kind == "xfb") in >> xfb_stride;
    std::vector<Harness::Input> inputs;
    std::vector<uint32_t> fixed_pixel;
    if (vertex_test) {
      fixed_pixel = ReadWords(ps_path);
      if (in >> input_list) {
        std::istringstream items(input_list);
        std::string item;
        while (std::getline(items, item, ',')) {
          if (!item.empty()) inputs.push_back({uint32_t(std::stoul(item)), item.back() == 'u'});
        }
      }
    }
    float worst = 0;
    uint32_t pixels = 0, drawn = 0;
    bool skip = false;
    for (uint32_t spec : specs) {
      const auto x = vertex_test ? harness.Draw(fixed_pixel, spec, &a, inputs, xfb_stride) : harness.Draw(a, spec);
      const auto y = vertex_test ? harness.Draw(fixed_pixel, spec, &b, inputs, xfb_stride) : harness.Draw(b, spec);
      if (x.empty() || y.empty()) {
        skip = true;
        break;
      }
      for (size_t p = 0; p < x.size(); p += 4) {
        bool bad = false;
        for (int k = 0; k < 4; ++k) {
          if (!Close(x[p + k], y[p + k])) {
            bad = true;
            if (std::isfinite(x[p + k]) && std::isfinite(y[p + k])) worst = std::max(worst, std::fabs(x[p + k] - y[p + k]));
            else worst = INFINITY;
          }
        }
        pixels += bad;
        drawn += xfb_stride ? 1 : x[p] != kSentinel;
      }
    }
    const char* status = skip ? "skipped" : pixels ? "different" : "same";
    (skip ? skipped : pixels ? different : same)++;
    std::printf("%s\t%s\t%g\t%u\t%u\n", name.c_str(), status, worst, pixels, drawn);
  }
  std::fprintf(stderr, "same %u, different %u, skipped %u\n", same, different, skipped);
  return 0;
} catch (const std::exception& e) {
  std::fprintf(stderr, "error: %s\n", e.what());
  return 1;
}
