// SPDX-License-Identifier: GPL-3.0-only
#include "graphics_probe.h"
#include "probe_shaders.h"
#include "port_probe_shaders.h"
#include "texture_tests/bc_fixtures.h"
#include "bo2zm_native_texture_upload.h"
#include "bo2zm_native_shader_profile.h"
#include <algorithm>
#include <array>
#include <cstring>
#include <stdexcept>
#include <vector>

namespace {
constexpr uint32_t kSide = 16;
constexpr VkDeviceSize kReadbackBytes = kSide * kSide * 4;
enum class Profile { Independent, PortUbo, PortTexture, PortMipTexture };

void Check(VkResult result, const char* operation) {
  if (result != VK_SUCCESS)
    throw std::runtime_error(std::string(operation) + " VkResult=" + std::to_string(result));
}

struct Buffer {
  VkBuffer handle = VK_NULL_HANDLE;
  VkDeviceMemory memory = VK_NULL_HANDLE;
  bool coherent = false;
};

class Probe {
 public:
  Probe(VkPhysicalDevice physical, VkDevice device, VkQueue queue, uint32_t family,
        GraphicsProbeResult& result, Profile profile = Profile::Independent,
        bo2zm::texture::BcFormat format = bo2zm::texture::BcFormat::BC1,
        uint32_t mip = 0, uint32_t layer = 0,
        bo2zm::texture::BcStorage storage = bo2zm::texture::BcStorage::Compact)
      : physical_(physical), device_(device), queue_(queue), family_(family), result_(result),
        profile_(profile), bc_format_(format), storage_(storage), selected_mip_(mip), selected_layer_(layer) {
    vkGetPhysicalDeviceMemoryProperties(physical_, &memory_);
  }
  ~Probe() {
    if (submitted_) vkDeviceWaitIdle(device_);
    if (fence_) vkDestroyFence(device_, fence_, nullptr);
    if (commands_) vkDestroyCommandPool(device_, commands_, nullptr);
    if (pipeline_) vkDestroyPipeline(device_, pipeline_, nullptr);
    if (pipeline_layout_) vkDestroyPipelineLayout(device_, pipeline_layout_, nullptr);
    for (auto shader : shaders_) if (shader) vkDestroyShaderModule(device_, shader, nullptr);
    if (framebuffer_) vkDestroyFramebuffer(device_, framebuffer_, nullptr);
    if (pass_) vkDestroyRenderPass(device_, pass_, nullptr);
    if (image_view_) vkDestroyImageView(device_, image_view_, nullptr);
    if (image_) vkDestroyImage(device_, image_, nullptr);
    if (image_memory_) vkFreeMemory(device_, image_memory_, nullptr);
    if (sampler_) vkDestroySampler(device_, sampler_, nullptr);
    if (sampled_view_) vkDestroyImageView(device_, sampled_view_, nullptr);
    if (sampled_image_) vkDestroyImage(device_, sampled_image_, nullptr);
    if (sampled_memory_) vkFreeMemory(device_, sampled_memory_, nullptr);
    for (auto pool : pools_) if (pool) vkDestroyDescriptorPool(device_, pool, nullptr);
    for (auto layout : layouts_) if (layout) vkDestroyDescriptorSetLayout(device_, layout, nullptr);
    for (const auto& buffer : {constants_, readback_, texture_upload_}) {
      if (buffer.handle) vkDestroyBuffer(device_, buffer.handle, nullptr);
      if (buffer.memory) vkFreeMemory(device_, buffer.memory, nullptr);
    }
  }

  void Run() {
    VkPhysicalDeviceProperties properties{};
    vkGetPhysicalDeviceProperties(physical_, &properties);
    if (properties.limits.maxUniformBufferRange < 4096 || properties.limits.maxBoundDescriptorSets < 5)
      throw std::runtime_error("Insufficient UBO range or bound descriptor sets");
    const VkDeviceSize alignment = std::max<VkDeviceSize>(16, properties.limits.minUniformBufferOffsetAlignment);
    stride_ = ((4096 + alignment - 1) / alignment) * alignment;

    for (size_t i = 0; i < bank_offsets_.size(); ++i)
      bank_offsets_[i] = stride_ * (profile_ == Profile::Independent ? i : 2 * i + 1);
    const VkDeviceSize constantBytes = stride_ * 6;
    CreateBuffer(constantBytes, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, constants_);
    CreateBuffer(kReadbackBytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT, readback_);
    void* mapped = nullptr;
    Check(vkMapMemory(device_, constants_.memory, 0, VK_WHOLE_SIZE, 0, &mapped), "map constants");
    std::memset(mapped, 0, static_cast<size_t>(constantBytes));
    const float vertices[12] = {-1, -1, 0, 1, 3, -1, 0, 1, -1, 3, 0, 1};
    const float color[4] = {0.2f, 0.6f, 0.8f, 1.0f};
    std::memcpy(static_cast<char*>(mapped) + bank_offsets_[0], vertices, sizeof(vertices));
    if (TextureMode()) {
      const float white[4] = {1, 1, 1, 1};
      std::memcpy(static_cast<char*>(mapped) + bank_offsets_[1], white, sizeof(white));
      auto* shared = static_cast<char*>(mapped) + bank_offsets_[2];
      const uint32_t imageIndex = 7, samplerIndex = 3;
      const float invSize[2] = {1.0f / std::max(kSide >> selected_mip_, 1u), 1.0f / std::max(kSide >> selected_mip_, 1u)}, scale = 1, coordScale[2] = {1, 1};
      std::memcpy(shared, &imageIndex, 4);
      std::memcpy(shared + 192, &samplerIndex, 4);
      std::memcpy(shared + 360, invSize, sizeof(invSize));
      std::memcpy(shared + 488, &scale, 4);
      std::memcpy(shared + 560, coordScale, sizeof(coordScale));
    } else {
      std::memcpy(static_cast<char*>(mapped) + bank_offsets_[1], color, sizeof(color));
    }
    VkResult flush = VK_SUCCESS;
    if (!constants_.coherent) {
      VkMappedMemoryRange range{};
      range.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE;
      range.memory = constants_.memory;
      range.size = VK_WHOLE_SIZE;
      flush = vkFlushMappedMemoryRanges(device_, 1, &range);
    }
    vkUnmapMemory(device_, constants_.memory);
    Check(flush, "flush constants");
    CreateDescriptors();
    if (TextureMode()) CreateSampledTexture();
    CreateTarget();
    CreatePipeline();
    Draw();
    ReadPixels();
  }

 private:
  bool TextureMode() const { return profile_ == Profile::PortTexture || profile_ == Profile::PortMipTexture; }

  uint32_t MemoryType(uint32_t bits, VkMemoryPropertyFlags required,
                      VkMemoryPropertyFlags preferred) const {
    for (auto flags : {required | preferred, required}) {
      for (uint32_t i = 0; i < memory_.memoryTypeCount; ++i)
        if ((bits & (1u << i)) && (memory_.memoryTypes[i].propertyFlags & flags) == flags) return i;
    }
    throw std::runtime_error("No compatible Vulkan memory type");
  }

  void CreateBuffer(VkDeviceSize bytes, VkBufferUsageFlags usage, Buffer& output) {
    VkBufferCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    info.size = bytes;
    info.usage = usage;
    info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    Check(vkCreateBuffer(device_, &info, nullptr, &output.handle), "create host buffer");
    VkMemoryRequirements requirements{};
    vkGetBufferMemoryRequirements(device_, output.handle, &requirements);
    VkMemoryAllocateInfo allocation{};
    allocation.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocation.allocationSize = requirements.size;
    allocation.memoryTypeIndex = MemoryType(requirements.memoryTypeBits,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT, VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    output.coherent = (memory_.memoryTypes[allocation.memoryTypeIndex].propertyFlags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) != 0;
    Check(vkAllocateMemory(device_, &allocation, nullptr, &output.memory), "allocate host buffer");
    Check(vkBindBufferMemory(device_, output.handle, output.memory, 0), "bind host buffer");
  }

  void CreateDescriptors() {
    constexpr uint32_t capacities[4] = {16384, 64, 256, 512};
    const VkDescriptorBindingFlags bindingFlags = VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT |
        VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT | VK_DESCRIPTOR_BINDING_UPDATE_UNUSED_WHILE_PENDING_BIT;
    for (uint32_t i = 0; i < 4; ++i) {
      VkDescriptorSetLayoutBinding binding{};
      binding.descriptorType = i == 3 ? VK_DESCRIPTOR_TYPE_SAMPLER : VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
      binding.descriptorCount = capacities[i];
      binding.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
      VkDescriptorSetLayoutBindingFlagsCreateInfo flags{};
      flags.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO;
      flags.bindingCount = 1;
      flags.pBindingFlags = &bindingFlags;
      VkDescriptorSetLayoutCreateInfo info{};
      info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
      info.pNext = &flags;
      info.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT;
      info.bindingCount = 1;
      info.pBindings = &binding;
      VkDescriptorSetLayoutSupport support{};
      support.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_SUPPORT;
      vkGetDescriptorSetLayoutSupport(device_, &info, &support);
      if (!support.supported) throw std::runtime_error("BO2 descriptor layout " + std::to_string(i) + " unsupported");
      Check(vkCreateDescriptorSetLayout(device_, &info, nullptr, &layouts_[i]), "create BO2 texture layout");
    }
    result_.layout_support = true;
    VkDescriptorPoolSize sizes[2] = {
        {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 16384 + 64 + 256}, {VK_DESCRIPTOR_TYPE_SAMPLER, 512}};
    VkDescriptorPoolCreateInfo pool{};
    pool.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    pool.flags = VK_DESCRIPTOR_POOL_CREATE_UPDATE_AFTER_BIND_BIT;
    pool.maxSets = 4;
    pool.poolSizeCount = 2;
    pool.pPoolSizes = sizes;
    Check(vkCreateDescriptorPool(device_, &pool, nullptr, &pools_[0]), "create BO2 texture descriptor pool");
    VkDescriptorSetAllocateInfo allocate{};
    allocate.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    allocate.descriptorPool = pools_[0];
    allocate.descriptorSetCount = 4;
    allocate.pSetLayouts = layouts_.data();
    Check(vkAllocateDescriptorSets(device_, &allocate, sets_.data()), "allocate BO2 texture descriptors");
    std::array<VkDescriptorSetLayoutBinding, 3> bindings{};
    for (uint32_t i = 0; i < 3; ++i) {
      bindings[i].binding = i;
      bindings[i].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
      bindings[i].descriptorCount = 1;
      bindings[i].stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    }
    VkDescriptorSetLayoutCreateInfo layout{};
    layout.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layout.bindingCount = 3;
    layout.pBindings = bindings.data();
    Check(vkCreateDescriptorSetLayout(device_, &layout, nullptr, &layouts_[4]), "create BO2 UBO layout");
    VkDescriptorPoolSize uboSize{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 3};
    pool.flags = 0;
    pool.maxSets = 1;
    pool.poolSizeCount = 1;
    pool.pPoolSizes = &uboSize;
    Check(vkCreateDescriptorPool(device_, &pool, nullptr, &pools_[1]), "create UBO descriptor pool");
    allocate.descriptorPool = pools_[1];
    allocate.descriptorSetCount = 1;
    allocate.pSetLayouts = &layouts_[4];
    Check(vkAllocateDescriptorSets(device_, &allocate, &sets_[4]), "allocate UBO descriptor set");
    std::array<VkDescriptorBufferInfo, 3> buffers{};
    std::array<VkWriteDescriptorSet, 3> writes{};
    for (uint32_t i = 0; i < 3; ++i) {
      buffers[i] = {constants_.handle, profile_ == Profile::Independent ? bank_offsets_[i] : 0,
                    i == 2 ? 688u : 4096u};
      writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
      writes[i].dstSet = sets_[4];
      writes[i].dstBinding = i;
      writes[i].descriptorCount = 1;
      writes[i].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
      writes[i].pBufferInfo = &buffers[i];
    }
    vkUpdateDescriptorSets(device_, 3, writes.data(), 0, nullptr);
    result_.descriptor_allocation = true;
  }

  void CreateSampledTexture() {
    texture_mips_ = profile_ == Profile::PortMipTexture ? 5 : 1;
    texture_layers_ = profile_ == Profile::PortMipTexture ? 6 : 1;
    texture_base_ = profile_ == Profile::PortMipTexture ? 128 : 0;
    const auto fixture = bo2test::MakeBcMipFixture(bc_format_, kSide, kSide, texture_layers_, texture_mips_);
    bo2zm::texture::BcMipUpload converted;
    if (!bo2zm::texture::ConvertBcMips(bc_format_, kSide, kSide, texture_layers_, texture_mips_, fixture.blocks, converted,
                                     bo2zm::texture::kBcUploadLimit, storage_))
      throw std::runtime_error("BC mip conversion failed");
    const size_t channels = bo2zm::texture::BcPixelBytes(bc_format_, storage_);
    for (uint32_t n = 0; n < texture_mips_; ++n) for (uint32_t layer = 0; layer < texture_layers_; ++layer) {
      const auto& golden = fixture.images[n * texture_layers_ + layer].rgba;
      const size_t offset = converted.layout.pixel_offsets[n] + layer * converted.layout.pixel_layer_bytes[n];
      for (size_t p = 0; p < golden.size() / 4; ++p) for (size_t c = 0; c < channels; ++c)
        if (converted.pixels[offset + p * channels + c] != golden[p * 4 + c])
          throw std::runtime_error("BC mip conversion disagrees with golden pixels");
    }
    result_.cpu_decode_match = true;
    std::array<uint32_t, 16> offsets{};
    for (uint32_t n = 0; n < texture_mips_; ++n) offsets[n] = uint32_t(converted.layout.pixel_offsets[n]);
    if (!bo2zm::texture::TextureCopyRegions(kSide, kSide, texture_layers_, 1, texture_mips_, offsets,
                                            texture_base_, texture_copies_, 4))
      throw std::runtime_error("Invalid BC mip upload copy regions");
    const auto& selected = fixture.images[selected_mip_ * texture_layers_ + selected_layer_];
    const uint32_t w = std::max(kSide >> selected_mip_, 1u), h = std::max(kSide >> selected_mip_, 1u);
    expected_.resize(kReadbackBytes);

    for (uint32_t y = 0; y < kSide; ++y) for (uint32_t x = 0; x < kSide; ++x)
      for (unsigned c = 0; c < 4; ++c)
        expected_[(y*kSide+x)*4+c] = selected.rgba[((((2*y+1)*h)/(2*kSide))*w+((2*x+1)*w)/(2*kSide))*4+c];

    for (size_t i = 0; i < expected_.size(); i += 4) {
      if (bc_format_ == bo2zm::texture::BcFormat::BC4)
        expected_[i + 1] = expected_[i + 2] = expected_[i + 3] = expected_[i];
      else if (bc_format_ == bo2zm::texture::BcFormat::BC5)
        expected_[i + 2] = expected_[i + 3] = expected_[i + 1];
    }
    VkFormatProperties properties{};
    const VkFormat host_format = bo2zm::texture::BcHostFormat(bc_format_, storage_);
    vkGetPhysicalDeviceFormatProperties(physical_, host_format, &properties);
    constexpr auto required = VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT |
        VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT;
    if ((properties.optimalTilingFeatures & required) != required)
      throw std::runtime_error("BC host sampled/filter/transfer-dst format unsupported");
    CreateBuffer(converted.pixels.size() + texture_base_, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, texture_upload_);
    void* mapped = nullptr;
    Check(vkMapMemory(device_, texture_upload_.memory, 0, VK_WHOLE_SIZE, 0, &mapped), "map decoded BC upload");
    std::memset(mapped, 0xa5, size_t(texture_base_));
    std::memcpy(static_cast<uint8_t*>(mapped) + texture_base_, converted.pixels.data(), converted.pixels.size());
    VkResult flush = VK_SUCCESS;
    if (!texture_upload_.coherent) {
      VkMappedMemoryRange range{};
      range.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE;
      range.memory = texture_upload_.memory; range.size = VK_WHOLE_SIZE;
      flush = vkFlushMappedMemoryRanges(device_, 1, &range);
    }
    vkUnmapMemory(device_, texture_upload_.memory);
    Check(flush, "flush decoded BC upload");
    VkImageCreateInfo image{};
    image.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    image.imageType = VK_IMAGE_TYPE_2D;
    image.format = host_format;
    image.extent = {kSide, kSide, 1};
    image.flags = texture_layers_ == 6 ? VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT : 0;
    image.mipLevels = texture_mips_; image.arrayLayers = texture_layers_; image.samples = VK_SAMPLE_COUNT_1_BIT;
    image.tiling = VK_IMAGE_TILING_OPTIMAL;
    image.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    image.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    Check(vkCreateImage(device_, &image, nullptr, &sampled_image_), "create decoded BC image");
    VkMemoryRequirements requirements{};
    vkGetImageMemoryRequirements(device_, sampled_image_, &requirements);
    VkMemoryAllocateInfo allocation{};
    allocation.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocation.allocationSize = requirements.size;
    allocation.memoryTypeIndex = MemoryType(requirements.memoryTypeBits, 0, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    Check(vkAllocateMemory(device_, &allocation, nullptr, &sampled_memory_), "allocate decoded BC image");
    Check(vkBindImageMemory(device_, sampled_image_, sampled_memory_, 0), "bind decoded BC image");
    VkImageViewCreateInfo view{};
    view.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    view.image = sampled_image_; view.viewType = VK_IMAGE_VIEW_TYPE_2D; view.format = image.format;
    view.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, selected_mip_, 1, selected_layer_, 1};
    if (bc_format_ == bo2zm::texture::BcFormat::BC4)
      view.components = {VK_COMPONENT_SWIZZLE_R, VK_COMPONENT_SWIZZLE_R, VK_COMPONENT_SWIZZLE_R, VK_COMPONENT_SWIZZLE_R};
    else if (bc_format_ == bo2zm::texture::BcFormat::BC5)
      view.components = {VK_COMPONENT_SWIZZLE_R, VK_COMPONENT_SWIZZLE_G, VK_COMPONENT_SWIZZLE_G, VK_COMPONENT_SWIZZLE_G};
    Check(vkCreateImageView(device_, &view, nullptr, &sampled_view_), "create decoded BC swizzled view");
    VkSamplerCreateInfo sampler{};
    sampler.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    sampler.magFilter = sampler.minFilter = VK_FILTER_NEAREST;
    sampler.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    sampler.addressModeU = sampler.addressModeV = sampler.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    Check(vkCreateSampler(device_, &sampler, nullptr, &sampler_), "create decoded BC sampler");
    const VkDescriptorImageInfo imageInfo{VK_NULL_HANDLE, sampled_view_, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    const VkDescriptorImageInfo samplerInfo{sampler_, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_UNDEFINED};
    std::array<VkWriteDescriptorSet, 2> writes{};
    for (auto& write : writes) { write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; write.descriptorCount = 1; }
    writes[0].dstSet = sets_[0]; writes[0].dstArrayElement = 7;
    writes[0].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE; writes[0].pImageInfo = &imageInfo;
    writes[1].dstSet = sets_[3]; writes[1].dstArrayElement = 3;
    writes[1].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER; writes[1].pImageInfo = &samplerInfo;
    vkUpdateDescriptorSets(device_, 2, writes.data(), 0, nullptr);
  }

  void CreateTarget() {
    VkImageCreateInfo image{};
    image.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    image.imageType = VK_IMAGE_TYPE_2D;
    image.format = VK_FORMAT_R8G8B8A8_UNORM;
    image.extent = {kSide, kSide, 1};
    image.mipLevels = 1;
    image.arrayLayers = 1;
    image.samples = VK_SAMPLE_COUNT_1_BIT;
    image.tiling = VK_IMAGE_TILING_OPTIMAL;
    image.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    image.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    Check(vkCreateImage(device_, &image, nullptr, &image_), "create offscreen image");
    VkMemoryRequirements requirements{};
    vkGetImageMemoryRequirements(device_, image_, &requirements);
    VkMemoryAllocateInfo allocation{};
    allocation.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocation.allocationSize = requirements.size;
    allocation.memoryTypeIndex = MemoryType(requirements.memoryTypeBits, 0, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    Check(vkAllocateMemory(device_, &allocation, nullptr, &image_memory_), "allocate offscreen image");
    Check(vkBindImageMemory(device_, image_, image_memory_, 0), "bind offscreen image");
    VkImageViewCreateInfo view{};
    view.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    view.image = image_;
    view.viewType = VK_IMAGE_VIEW_TYPE_2D;
    view.format = image.format;
    view.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    Check(vkCreateImageView(device_, &view, nullptr, &image_view_), "create offscreen view");
    VkAttachmentDescription attachment{};
    attachment.format = image.format;
    attachment.samples = VK_SAMPLE_COUNT_1_BIT;
    attachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    attachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    attachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    attachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    attachment.finalLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    VkAttachmentReference reference{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkSubpassDescription subpass{};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 1;
    subpass.pColorAttachments = &reference;
    std::array<VkSubpassDependency, 2> dependencies{};
    dependencies[0].srcSubpass = VK_SUBPASS_EXTERNAL;
    dependencies[0].dstSubpass = 0;
    dependencies[0].srcStageMask = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
    dependencies[0].dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dependencies[0].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    dependencies[1].srcSubpass = 0;
    dependencies[1].dstSubpass = VK_SUBPASS_EXTERNAL;
    dependencies[1].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dependencies[1].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    dependencies[1].dstStageMask = VK_PIPELINE_STAGE_TRANSFER_BIT;
    dependencies[1].dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    VkRenderPassCreateInfo pass{};
    pass.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    pass.attachmentCount = 1;
    pass.pAttachments = &attachment;
    pass.subpassCount = 1;
    pass.pSubpasses = &subpass;
    pass.dependencyCount = static_cast<uint32_t>(dependencies.size());
    pass.pDependencies = dependencies.data();
    Check(vkCreateRenderPass(device_, &pass, nullptr, &pass_), "create offscreen render pass");
    VkFramebufferCreateInfo framebuffer{};
    framebuffer.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
    framebuffer.renderPass = pass_;
    framebuffer.attachmentCount = 1;
    framebuffer.pAttachments = &image_view_;
    framebuffer.width = kSide;
    framebuffer.height = kSide;
    framebuffer.layers = 1;
    Check(vkCreateFramebuffer(device_, &framebuffer, nullptr, &framebuffer_), "create offscreen framebuffer");
  }

  void CreatePipeline() {
    VkPipelineLayoutCreateInfo layout{};
    layout.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layout.setLayoutCount = static_cast<uint32_t>(layouts_.size());
    layout.pSetLayouts = layouts_.data();
    Check(vkCreatePipelineLayout(device_, &layout, nullptr, &pipeline_layout_), "create BO2-size pipeline layout");
    const uint32_t* code[] = {bo2probe::kVertex, bo2probe::kFragment};
    size_t sizes[] = {sizeof(bo2probe::kVertex), sizeof(bo2probe::kFragment)};
    if (profile_ == Profile::PortUbo) {
      code[0] = bo2portprobe::kVertex; code[1] = bo2portprobe::kFragment;
      sizes[0] = sizeof(bo2portprobe::kVertex); sizes[1] = sizeof(bo2portprobe::kFragment);
    } else if (TextureMode()) {
      code[0] = bo2portprobe::kTextureVertex; code[1] = bo2portprobe::kTextureFragment;
      sizes[0] = sizeof(bo2portprobe::kTextureVertex); sizes[1] = sizeof(bo2portprobe::kTextureFragment);
    }
    const uint32_t specializationBits = (1u << 8) | (1u << 9);
    const VkSpecializationMapEntry entry{0, 0, sizeof(specializationBits)};
    const VkSpecializationInfo specialization{1, &entry, sizeof(specializationBits), &specializationBits};
    std::array<VkPipelineShaderStageCreateInfo, 2> stages{};
    for (size_t i = 0; i < stages.size(); ++i) {
      VkShaderModuleCreateInfo shader{};
      shader.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
      shader.codeSize = sizes[i];
      shader.pCode = code[i];
      if (profile_ != Profile::Independent && !bo2zm::native::UboOnlySpirv({shader.pCode, shader.codeSize / 4}))
        throw std::runtime_error("Port probe shader violates UBO-only profile");
      Check(vkCreateShaderModule(device_, &shader, nullptr, &shaders_[i]), "create UBO-only shader module");
      stages[i].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
      stages[i].stage = i == 0 ? VK_SHADER_STAGE_VERTEX_BIT : VK_SHADER_STAGE_FRAGMENT_BIT;
      stages[i].module = shaders_[i];
      stages[i].pName = "main";
      if (TextureMode()) stages[i].pSpecializationInfo = &specialization;
    }
    VkPipelineVertexInputStateCreateInfo vertex{};
    vertex.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    VkPipelineInputAssemblyStateCreateInfo assembly{};
    assembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkViewport viewport{0, 0, float(kSide), float(kSide), 0, 1};
    VkRect2D scissor{{0, 0}, {kSide, kSide}};
    VkPipelineViewportStateCreateInfo view{};
    view.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    view.viewportCount = 1;
    view.pViewports = &viewport;
    view.scissorCount = 1;
    view.pScissors = &scissor;
    VkPipelineRasterizationStateCreateInfo raster{};
    raster.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    raster.polygonMode = VK_POLYGON_MODE_FILL;
    raster.cullMode = VK_CULL_MODE_NONE;
    raster.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    raster.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo multisample{};
    multisample.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineColorBlendAttachmentState attachment{};
    attachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
        VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    VkPipelineColorBlendStateCreateInfo blend{};
    blend.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    blend.attachmentCount = 1;
    blend.pAttachments = &attachment;
    VkGraphicsPipelineCreateInfo pipeline{};
    pipeline.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    pipeline.stageCount = static_cast<uint32_t>(stages.size());
    pipeline.pStages = stages.data();
    pipeline.pVertexInputState = &vertex;
    pipeline.pInputAssemblyState = &assembly;
    pipeline.pViewportState = &view;
    pipeline.pRasterizationState = &raster;
    pipeline.pMultisampleState = &multisample;
    pipeline.pColorBlendState = &blend;
    pipeline.layout = pipeline_layout_;
    pipeline.renderPass = pass_;
    Check(vkCreateGraphicsPipelines(device_, VK_NULL_HANDLE, 1, &pipeline, nullptr, &pipeline_), "create UBO-only graphics pipeline");
    result_.pipeline_created = true;
  }

  void Draw() {
    VkCommandPoolCreateInfo pool{};
    pool.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    pool.queueFamilyIndex = family_;
    Check(vkCreateCommandPool(device_, &pool, nullptr, &commands_), "create graphics command pool");
    VkCommandBufferAllocateInfo allocate{};
    allocate.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    allocate.commandPool = commands_;
    allocate.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocate.commandBufferCount = 1;
    VkCommandBuffer command = VK_NULL_HANDLE;
    Check(vkAllocateCommandBuffers(device_, &allocate, &command), "allocate graphics command buffer");
    VkCommandBufferBeginInfo begin{};
    begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    Check(vkBeginCommandBuffer(command, &begin), "begin graphics command buffer");
    if (TextureMode()) {
      VkImageMemoryBarrier barrier{};
      barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
      barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
      barrier.image = sampled_image_; barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, texture_mips_, 0, texture_layers_};
      barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED; barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
      barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
      vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                           0, 0, nullptr, 0, nullptr, 1, &barrier);
      vkCmdCopyBufferToImage(command, texture_upload_.handle, sampled_image_,
                             VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, texture_mips_, texture_copies_.data());
      barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL; barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
      barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT; barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
      vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                           0, 0, nullptr, 0, nullptr, 1, &barrier);
    }
    VkClearValue clear{};
    VkRenderPassBeginInfo pass{};
    pass.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    pass.renderPass = pass_;
    pass.framebuffer = framebuffer_;
    pass.renderArea.extent = {kSide, kSide};
    pass.clearValueCount = 1;
    pass.pClearValues = &clear;
    vkCmdBeginRenderPass(command, &pass, VK_SUBPASS_CONTENTS_INLINE);
    vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_);
    uint32_t dynamicOffsets[3] = {0, 0, 0};
    if (profile_ != Profile::Independent)
      for (size_t i = 0; i < bank_offsets_.size(); ++i) dynamicOffsets[i] = uint32_t(bank_offsets_[i]);
    vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_layout_,
        0, static_cast<uint32_t>(sets_.size()), sets_.data(), 3, dynamicOffsets);
    vkCmdDraw(command, 3, 1, 0, 0);
    vkCmdEndRenderPass(command);
    VkBufferImageCopy copy{};
    copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    copy.imageExtent = {kSide, kSide, 1};
    vkCmdCopyImageToBuffer(command, image_, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, readback_.handle, 1, &copy);
    VkBufferMemoryBarrier barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
    barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.buffer = readback_.handle;
    barrier.size = VK_WHOLE_SIZE;
    vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT,
        0, 0, nullptr, 1, &barrier, 0, nullptr);
    Check(vkEndCommandBuffer(command), "end graphics command buffer");
    VkFenceCreateInfo fence{};
    fence.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    Check(vkCreateFence(device_, &fence, nullptr, &fence_), "create graphics fence");
    VkSubmitInfo submit{};
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &command;
    Check(vkQueueSubmit(queue_, 1, &submit, fence_), "submit UBO draw");
    submitted_ = true;
    result_.draw_submitted = true;
    Check(vkWaitForFences(device_, 1, &fence_, VK_TRUE, 5000000000ULL), "wait for UBO draw");
    result_.texture_uploaded = TextureMode();
    result_.subresources_checked = TextureMode() ? 1 : 0;
  }

  void ReadPixels() {
    void* mapped = nullptr;
    Check(vkMapMemory(device_, readback_.memory, 0, VK_WHOLE_SIZE, 0, &mapped), "map readback");
    VkResult invalidate = VK_SUCCESS;
    if (!readback_.coherent) {
      VkMappedMemoryRange range{};
      range.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE;
      range.memory = readback_.memory;
      range.size = VK_WHOLE_SIZE;
      invalidate = vkInvalidateMappedMemoryRanges(device_, 1, &range);
    }
    if (invalidate == VK_SUCCESS) {
      const auto* bytes = static_cast<const uint8_t*>(mapped);
      const uint8_t expected[4] = {51, 153, 204, 255};
      result_.first_pixel_rgba = (uint32_t(bytes[0]) << 24) | (uint32_t(bytes[1]) << 16) |
          (uint32_t(bytes[2]) << 8) | uint32_t(bytes[3]);
      for (uint32_t i = 0; i < kReadbackBytes; ++i) {
        const int difference = int(bytes[i]) - int(TextureMode() ? expected_[i] : expected[i % 4]);
        result_.max_channel_error = std::max(result_.max_channel_error,
            uint32_t(difference < 0 ? -difference : difference));
      }
      result_.pixels_checked = kSide * kSide;
      result_.pixels_match = result_.max_channel_error <= 1;
    }
    vkUnmapMemory(device_, readback_.memory);
    Check(invalidate, "invalidate readback");
    if (!result_.pixels_match) throw std::runtime_error("Graphics readback did not match expected RGBA");
  }

  VkPhysicalDevice physical_;
  VkDevice device_;
  VkQueue queue_;
  uint32_t family_;
  GraphicsProbeResult& result_;
  Profile profile_;
  bo2zm::texture::BcFormat bc_format_;
  bo2zm::texture::BcStorage storage_;
  uint32_t selected_mip_, selected_layer_;
  uint32_t texture_mips_ = 1, texture_layers_ = 1;
  VkDeviceSize texture_base_ = 0;
  std::array<VkBufferImageCopy, 16> texture_copies_{};
  std::vector<uint8_t> expected_;
  VkPhysicalDeviceMemoryProperties memory_{};
  VkDeviceSize stride_ = 0;
  Buffer constants_, readback_, texture_upload_;
  std::array<VkDeviceSize, 3> bank_offsets_{};
  std::array<VkDescriptorSetLayout, 5> layouts_{};
  std::array<VkDescriptorSet, 5> sets_{};
  std::array<VkDescriptorPool, 2> pools_{};
  std::array<VkShaderModule, 2> shaders_{};
  VkPipelineLayout pipeline_layout_ = VK_NULL_HANDLE;
  VkPipeline pipeline_ = VK_NULL_HANDLE;
  VkImage image_ = VK_NULL_HANDLE;
  VkDeviceMemory image_memory_ = VK_NULL_HANDLE;
  VkImageView image_view_ = VK_NULL_HANDLE;
  VkImage sampled_image_ = VK_NULL_HANDLE;
  VkDeviceMemory sampled_memory_ = VK_NULL_HANDLE;
  VkImageView sampled_view_ = VK_NULL_HANDLE;
  VkSampler sampler_ = VK_NULL_HANDLE;
  VkRenderPass pass_ = VK_NULL_HANDLE;
  VkFramebuffer framebuffer_ = VK_NULL_HANDLE;
  VkCommandPool commands_ = VK_NULL_HANDLE;
  VkFence fence_ = VK_NULL_HANDLE;
  bool submitted_ = false;
};
}

GraphicsProbeResult ProbeUboGraphics(VkPhysicalDevice physical, VkDevice device,
                                     VkQueue queue, uint32_t family) {
  GraphicsProbeResult result;
  try {
    Probe probe(physical, device, queue, family, result);
    probe.Run();
  } catch (const std::exception& error) {
    result.error = error.what();
  }
  return result;
}

GraphicsProbeResult ProbePortUboGraphics(VkPhysicalDevice physical, VkDevice device,
                                         VkQueue queue, uint32_t family) {
  GraphicsProbeResult result;
  try { Probe probe(physical, device, queue, family, result, Profile::PortUbo); probe.Run(); }
  catch (const std::exception& error) { result.error = error.what(); }
  return result;
}

GraphicsProbeResult ProbePortBcGraphics(VkPhysicalDevice physical, VkDevice device,
                                        VkQueue queue, uint32_t family, bo2zm::texture::BcFormat format,
                                        bo2zm::texture::BcStorage storage) {
  GraphicsProbeResult result;
  try { Probe probe(physical, device, queue, family, result, Profile::PortTexture, format, 0, 0, storage); probe.Run(); }
  catch (const std::exception& error) { result.error = error.what(); }
  return result;
}

const char* PortProbeShaderHeaderSha256() { return bo2portprobe::kCommonSha256; }

GraphicsProbeResult ProbePortBcMipGraphics(VkPhysicalDevice physical, VkDevice device,
                                           VkQueue queue, uint32_t family, bo2zm::texture::BcFormat format,
                                           bo2zm::texture::BcStorage storage) {
  GraphicsProbeResult total;
  total.layout_support = total.descriptor_allocation = total.pipeline_created = total.draw_submitted =
      total.pixels_match = total.cpu_decode_match = total.texture_uploaded = true;
  for (uint32_t n = 0; n < 5; ++n) for (uint32_t layer = 0; layer < 6; ++layer) {
    GraphicsProbeResult part;
    try { Probe probe(physical, device, queue, family, part, Profile::PortMipTexture, format, n, layer, storage); probe.Run(); }
    catch (const std::exception& error) { part.error = error.what(); }
    total.layout_support &= part.layout_support; total.descriptor_allocation &= part.descriptor_allocation;
    total.pipeline_created &= part.pipeline_created; total.draw_submitted &= part.draw_submitted;
    total.pixels_match &= part.pixels_match; total.cpu_decode_match &= part.cpu_decode_match;
    total.texture_uploaded &= part.texture_uploaded;
    total.pixels_checked += part.pixels_checked; total.subresources_checked += part.subresources_checked;
    total.max_channel_error = std::max(total.max_channel_error, part.max_channel_error);
    if (!part.error.empty()) {
      total.error = "mip " + std::to_string(n) + " layer " + std::to_string(layer) + ": " + part.error;
      return total;
    }
  }
  return total;
}
