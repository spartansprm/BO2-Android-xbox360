#pragma once
#include <cstdint>
#include <cstddef>
#define VK_NULL_HANDLE nullptr
#define VKAPI_PTR
typedef struct VkImage_T* VkImage; typedef struct VkImageView_T* VkImageView; typedef struct VkCommandBuffer_T* VkCommandBuffer;
typedef struct VkRenderPass_T* VkRenderPass; typedef struct VkFramebuffer_T* VkFramebuffer; typedef struct VkDeviceMemory_T* VkDeviceMemory;
typedef uint32_t VkFlags; typedef VkFlags VkImageAspectFlags; typedef VkFlags VkPipelineStageFlags; typedef VkFlags VkAccessFlags;
typedef VkFlags VkDependencyFlags;
enum VkFormat { VK_FORMAT_UNDEFINED=0, VK_FORMAT_R8G8B8A8_UNORM=37, VK_FORMAT_MAX_ENUM=0x7FFFFFFF };
enum VkImageLayout { VK_IMAGE_LAYOUT_UNDEFINED=0, VK_IMAGE_LAYOUT_GENERAL=1 };
enum VkStructureType { VK_STRUCTURE_TYPE_MEMORY_BARRIER=46, VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER=45, VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO=43 };
enum VkSubpassContents { VK_SUBPASS_CONTENTS_INLINE=0 };
#define VK_QUEUE_FAMILY_IGNORED (~0U)
enum : uint32_t { VK_IMAGE_ASPECT_COLOR_BIT=1, VK_IMAGE_ASPECT_DEPTH_BIT=2, VK_IMAGE_ASPECT_STENCIL_BIT=4 };
enum : uint32_t { VK_PIPELINE_STAGE_VERTEX_SHADER_BIT=8, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT=0x80, VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT=0x100,
  VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT=0x200, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT=0x400, VK_PIPELINE_STAGE_TRANSFER_BIT=0x1000 };
enum : uint32_t { VK_ACCESS_SHADER_READ_BIT=0x20, VK_ACCESS_COLOR_ATTACHMENT_READ_BIT=0x80, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT=0x100,
  VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT=0x200, VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT=0x400, VK_ACCESS_TRANSFER_READ_BIT=0x800,
  VK_ACCESS_TRANSFER_WRITE_BIT=0x1000 };
struct VkOffset2D { int32_t x, y; }; struct VkExtent2D { uint32_t width, height; }; struct VkRect2D { VkOffset2D offset; VkExtent2D extent; };
struct VkOffset3D { int32_t x,y,z; }; struct VkExtent3D { uint32_t width,height,depth; };
union VkClearColorValue { float float32[4]; int32_t int32[4]; uint32_t uint32[4]; };
struct VkClearDepthStencilValue { float depth; uint32_t stencil; };
union VkClearValue { VkClearColorValue color; VkClearDepthStencilValue depthStencil; };
struct VkClearAttachment { VkImageAspectFlags aspectMask; uint32_t colorAttachment; VkClearValue clearValue; };
struct VkClearRect { VkRect2D rect; uint32_t baseArrayLayer; uint32_t layerCount; };
struct VkMemoryBarrier { VkStructureType sType; const void* pNext; VkAccessFlags srcAccessMask, dstAccessMask; };
struct VkImageSubresourceRange { VkImageAspectFlags aspectMask; uint32_t a,b,c,d; };
struct VkImageMemoryBarrier { VkStructureType sType; const void* pNext; VkAccessFlags srcAccessMask, dstAccessMask; VkImageLayout oldLayout, newLayout;
  uint32_t srcQueueFamilyIndex, dstQueueFamilyIndex; VkImage image; VkImageSubresourceRange subresourceRange; };
struct VkImageSubresourceLayers { VkImageAspectFlags a; uint32_t b,c,d; };
struct VkImageCopy { VkImageSubresourceLayers s; VkOffset3D so; VkImageSubresourceLayers d; VkOffset3D dof; VkExtent3D extent; };
struct VkRenderPassBeginInfo { VkStructureType sType; const void* pNext; VkRenderPass renderPass; VkFramebuffer framebuffer; VkRect2D renderArea;
  uint32_t clearValueCount; const VkClearValue* pClearValues; };
struct Dfn {
  void (*vkCmdPipelineBarrier)(VkCommandBuffer, VkPipelineStageFlags, VkPipelineStageFlags, VkDependencyFlags, uint32_t, const VkMemoryBarrier*,
                               uint32_t, const void*, uint32_t, const VkImageMemoryBarrier*);
  void (*vkCmdClearColorImage)(VkCommandBuffer, VkImage, VkImageLayout, const VkClearColorValue*, uint32_t, const VkImageSubresourceRange*);
  void (*vkCmdClearAttachments)(VkCommandBuffer, uint32_t, const VkClearAttachment*, uint32_t, const VkClearRect*);
  void (*vkCmdBeginRenderPass)(VkCommandBuffer, const VkRenderPassBeginInfo*, VkSubpassContents);
};
