// SPDX-License-Identifier: GPL-3.0-only


#include "bo2zm_native_scene_sync.h"
#include "bo2zm_native_draw_batch.h"
#include <array>
#include <numeric>
#include <atomic>
#include <cmath>
#include <cstring>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <vector>
static std::atomic<unsigned> errors{},warnings{},hazards{};
static void require(bool value,const char* message){if(!value)throw std::runtime_error(message);}
static void check(VkResult value){require(value==VK_SUCCESS,"Vulkan call failed");}
VKAPI_ATTR VkBool32 VKAPI_CALL message(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
    VkDebugUtilsMessageTypeFlagsEXT,const VkDebugUtilsMessengerCallbackDataEXT* data,void*) {
  if(severity&VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT)++errors;
  if(severity&VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT)++warnings;
  if((data->pMessageIdName&&std::strstr(data->pMessageIdName,"SYNC-HAZARD"))||std::strstr(data->pMessage,"hazard detected"))++hazards;
  if(errors<=3)std::cerr<<data->pMessage<<'\n';return VK_FALSE;
}
struct Image {VkImage image{}, depth{};VkDeviceMemory memory{}, depth_memory{};VkImageView view{}, depth_view{};VkFramebuffer frame{};};
int main(int argc,char** argv)try {
  require(argc==3,"Arguments: batch.vert.spv batch.frag.spv");
  const char* layer="VK_LAYER_KHRONOS_validation";const char* extensions[]={VK_EXT_DEBUG_UTILS_EXTENSION_NAME,VK_EXT_VALIDATION_FEATURES_EXTENSION_NAME,VK_EXT_LAYER_SETTINGS_EXTENSION_NAME};
  VkValidationFeatureEnableEXT sync=VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT;
  VkValidationFeaturesEXT validation{VK_STRUCTURE_TYPE_VALIDATION_FEATURES_EXT};validation.enabledValidationFeatureCount=1;validation.pEnabledValidationFeatures=&sync;
  VkBool32 shader_accesses=VK_TRUE;
  VkLayerSettingEXT setting{layer,"syncval_shader_accesses_heuristic",VK_LAYER_SETTING_TYPE_BOOL32_EXT,1,&shader_accesses};
  VkLayerSettingsCreateInfoEXT settings{VK_STRUCTURE_TYPE_LAYER_SETTINGS_CREATE_INFO_EXT};settings.pNext=&validation;settings.settingCount=1;settings.pSettings=&setting;
  VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};app.apiVersion=VK_API_VERSION_1_2;
  VkInstanceCreateInfo ic{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};ic.pNext=&settings;ic.pApplicationInfo=&app;ic.enabledLayerCount=1;ic.ppEnabledLayerNames=&layer;ic.enabledExtensionCount=3;ic.ppEnabledExtensionNames=extensions;
  VkInstance instance{};check(vkCreateInstance(&ic,nullptr,&instance));
  VkDebugUtilsMessengerCreateInfoEXT debug{VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT};debug.messageSeverity=VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT|VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT;debug.messageType=VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT|VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;debug.pfnUserCallback=message;
  VkDebugUtilsMessengerEXT messenger{};check(reinterpret_cast<PFN_vkCreateDebugUtilsMessengerEXT>(vkGetInstanceProcAddr(instance,"vkCreateDebugUtilsMessengerEXT"))(instance,&debug,nullptr,&messenger));
  uint32_t count{};check(vkEnumeratePhysicalDevices(instance,&count,nullptr));require(count,"No device");std::vector<VkPhysicalDevice> physicals(count);check(vkEnumeratePhysicalDevices(instance,&count,physicals.data()));auto physical=physicals[0];
  vkGetPhysicalDeviceQueueFamilyProperties(physical,&count,nullptr);std::vector<VkQueueFamilyProperties> families(count);vkGetPhysicalDeviceQueueFamilyProperties(physical,&count,families.data());uint32_t family=0;while(family<count&&!(families[family].queueFlags&VK_QUEUE_GRAPHICS_BIT))++family;require(family<count,"No graphics queue");
  float priority=1;VkDeviceQueueCreateInfo qi{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};qi.queueFamilyIndex=family;qi.queueCount=1;qi.pQueuePriorities=&priority;
  VkDeviceCreateInfo dc{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};dc.queueCreateInfoCount=1;dc.pQueueCreateInfos=&qi;VkDevice device{};check(vkCreateDevice(physical,&dc,nullptr,&device));VkQueue queue{};vkGetDeviceQueue(device,family,0,&queue);
  VkPhysicalDeviceMemoryProperties memory{};vkGetPhysicalDeviceMemoryProperties(physical,&memory);
  auto allocate=[&](VkMemoryRequirements req,VkMemoryPropertyFlags flags){uint32_t type=0;while(type<memory.memoryTypeCount&&(!(req.memoryTypeBits&(1u<<type))||(memory.memoryTypes[type].propertyFlags&flags)!=flags))++type;require(type<memory.memoryTypeCount,"Memory type missing");VkMemoryAllocateInfo a{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};a.allocationSize=req.size;a.memoryTypeIndex=type;VkDeviceMemory m{};check(vkAllocateMemory(device,&a,nullptr,&m));return m;};
  constexpr uint32_t width=32,height=32,pixels=width*height,frame_bytes=pixels*8,frames=12;
  VkAttachmentDescription attachments[2]{};
  for(unsigned i=0;i<2;++i){auto&a=attachments[i];a.format=i?VK_FORMAT_D32_SFLOAT:VK_FORMAT_R8G8B8A8_UNORM;
    a.samples=VK_SAMPLE_COUNT_1_BIT;a.loadOp=VK_ATTACHMENT_LOAD_OP_CLEAR;a.storeOp=VK_ATTACHMENT_STORE_OP_STORE;
    a.initialLayout=VK_IMAGE_LAYOUT_UNDEFINED;a.finalLayout=VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;}
  VkAttachmentReference color{0,VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL},depth{1,VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
  VkSubpassDescription sub{};sub.pipelineBindPoint=VK_PIPELINE_BIND_POINT_GRAPHICS;
  sub.colorAttachmentCount=1;sub.pColorAttachments=&color;sub.pDepthStencilAttachment=&depth;
  auto deps=bo2zm::native::scene::ImageDependencies();
  VkRenderPassCreateInfo rp{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};rp.attachmentCount=2;rp.pAttachments=attachments;
  rp.subpassCount=1;rp.pSubpasses=&sub;rp.dependencyCount=uint32_t(deps.size());rp.pDependencies=deps.data();
  VkRenderPass pass{};check(vkCreateRenderPass(device,&rp,nullptr,&pass));
  Image image;
  for(unsigned a=0;a<2;++a){VkImageCreateInfo ic{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};ic.imageType=VK_IMAGE_TYPE_2D;
    ic.format=attachments[a].format;ic.extent={width,height,1};ic.mipLevels=ic.arrayLayers=1;ic.samples=VK_SAMPLE_COUNT_1_BIT;
    ic.tiling=VK_IMAGE_TILING_OPTIMAL;ic.usage=VK_IMAGE_USAGE_TRANSFER_SRC_BIT|(a?VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT:VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT);
    auto& im=a?image.depth:image.image;auto& mem=a?image.depth_memory:image.memory;auto& view=a?image.depth_view:image.view;
    check(vkCreateImage(device,&ic,nullptr,&im));VkMemoryRequirements req;vkGetImageMemoryRequirements(device,im,&req);
    mem=allocate(req,0);check(vkBindImageMemory(device,im,mem,0));VkImageViewCreateInfo iv{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    iv.image=im;iv.viewType=VK_IMAGE_VIEW_TYPE_2D;iv.format=ic.format;
    iv.subresourceRange={a?VK_IMAGE_ASPECT_DEPTH_BIT:VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};check(vkCreateImageView(device,&iv,nullptr,&view));}
  VkImageView views[]={image.view,image.depth_view};VkFramebufferCreateInfo fb{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
  fb.renderPass=pass;fb.attachmentCount=2;fb.pAttachments=views;fb.width=width;fb.height=height;fb.layers=1;
  check(vkCreateFramebuffer(device,&fb,nullptr,&image.frame));
  auto shader=[&](const char* path){std::ifstream f(path,std::ios::binary|std::ios::ate);require(bool(f),"Missing shader");
    size_t bytes=f.tellg();std::vector<uint32_t>w(bytes/4);f.seekg(0);f.read(reinterpret_cast<char*>(w.data()),bytes);
    require(bool(f)&&bo2zm::native::draw_batch::ShaderSafe(w),"Test shader not certified");
    VkShaderModuleCreateInfo mc{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};mc.codeSize=bytes;mc.pCode=w.data();
    VkShaderModule m{};check(vkCreateShaderModule(device,&mc,nullptr,&m));return m;};
  VkShaderModule vs=shader(argv[1]),fs=shader(argv[2]);
  VkPipelineLayoutCreateInfo lc{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};VkPipelineLayout layout{};
  check(vkCreatePipelineLayout(device,&lc,nullptr,&layout));
  VkPipelineShaderStageCreateInfo stages[2]={{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO},{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO}};
  stages[0].stage=VK_SHADER_STAGE_VERTEX_BIT;stages[0].module=vs;stages[1].stage=VK_SHADER_STAGE_FRAGMENT_BIT;stages[1].module=fs;
  stages[0].pName=stages[1].pName="main";
  VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
  VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};ia.topology=VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
  VkViewport viewport{0,0,float(width),float(height),0,1};VkRect2D rect{{0,0},{width,height}};
  VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};vp.viewportCount=vp.scissorCount=1;vp.pViewports=&viewport;vp.pScissors=&rect;
  VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};rs.polygonMode=VK_POLYGON_MODE_FILL;rs.lineWidth=1;
  VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};ms.rasterizationSamples=VK_SAMPLE_COUNT_1_BIT;
  VkPipelineDepthStencilStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};ds.depthTestEnable=ds.depthWriteEnable=VK_TRUE;ds.depthCompareOp=VK_COMPARE_OP_LESS;
  VkPipelineColorBlendAttachmentState ba{};ba.colorWriteMask=15;
  VkPipelineColorBlendStateCreateInfo bs{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};bs.attachmentCount=1;bs.pAttachments=&ba;
  VkDynamicState states[]={VK_DYNAMIC_STATE_SCISSOR};VkPipelineDynamicStateCreateInfo dynamic{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
  dynamic.dynamicStateCount=1;dynamic.pDynamicStates=states;
  VkGraphicsPipelineCreateInfo gp{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};gp.stageCount=2;gp.pStages=stages;
  gp.pVertexInputState=&vi;gp.pInputAssemblyState=&ia;gp.pViewportState=&vp;gp.pRasterizationState=&rs;gp.pMultisampleState=&ms;
  gp.pDepthStencilState=&ds;gp.pColorBlendState=&bs;gp.pDynamicState=&dynamic;gp.layout=layout;gp.renderPass=pass;
  VkPipeline pipelines[2]{};check(vkCreateGraphicsPipelines(device,VK_NULL_HANDLE,1,&gp,nullptr,&pipelines[0]));
  ba.blendEnable=VK_TRUE;ba.srcColorBlendFactor=VK_BLEND_FACTOR_SRC_ALPHA;ba.dstColorBlendFactor=VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
  ba.colorBlendOp=VK_BLEND_OP_ADD;ba.srcAlphaBlendFactor=VK_BLEND_FACTOR_ONE;ba.dstAlphaBlendFactor=VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;ba.alphaBlendOp=VK_BLEND_OP_ADD;
  ds.depthWriteEnable=VK_FALSE;ds.depthCompareOp=VK_COMPARE_OP_ALWAYS;check(vkCreateGraphicsPipelines(device,VK_NULL_HANDLE,1,&gp,nullptr,&pipelines[1]));
  auto buffer=[&](VkDeviceSize bytes,VkBufferUsageFlags usage,VkBuffer& b,VkDeviceMemory& m){
    VkBufferCreateInfo bc{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};bc.size=bytes;bc.usage=usage;
    check(vkCreateBuffer(device,&bc,nullptr,&b));VkMemoryRequirements req;vkGetBufferMemoryRequirements(device,b,&req);
    m=allocate(req,VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);check(vkBindBufferMemory(device,b,m,0));};
  VkBuffer readback{},indices{};VkDeviceMemory read_memory{},index_memory{};
  buffer(frames*frame_bytes,VK_BUFFER_USAGE_TRANSFER_DST_BIT,readback,read_memory);
  buffer(3*256*4,VK_BUFFER_USAGE_INDEX_BUFFER_BIT,indices,index_memory);
  void* mapped{};check(vkMapMemory(device,index_memory,0,VK_WHOLE_SIZE,0,&mapped));
  auto index_data=static_cast<uint32_t*>(mapped);std::iota(index_data,index_data+3*256,0u);vkUnmapMemory(device,index_memory);
  VkCommandPoolCreateInfo pc{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};pc.queueFamilyIndex=family;
  VkCommandPool pool{};check(vkCreateCommandPool(device,&pc,nullptr,&pool));
  VkCommandBufferAllocateInfo ca{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};ca.commandPool=pool;ca.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY;ca.commandBufferCount=1;
  VkCommandBuffer cb{};check(vkAllocateCommandBuffers(device,&ca,&cb));VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};check(vkBeginCommandBuffer(cb,&begin));
  uint64_t saved=0,requested=0,emitted=0;
  for(uint32_t frame=0;frame<frames;++frame){
    const bool batching=frame%2;const uint32_t scenario=frame/2;
    VkClearValue clear[2]{};clear[0].color.float32[3]=1;clear[1].depthStencil.depth=1;
    VkRenderPassBeginInfo p{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};p.renderPass=pass;p.framebuffer=image.frame;p.renderArea=rect;p.clearValueCount=2;p.pClearValues=clear;
    bo2zm::native::draw_batch::Coalescer batch;
    auto emit=[&](const bo2zm::native::draw_batch::Draw& d){if(d.indexed)vkCmdDrawIndexed(cb,d.count,d.instances,d.first,d.vertex_offset,d.first_instance);else vkCmdDraw(cb,d.count,d.instances,d.first,d.first_instance);};
    vkCmdBeginRenderPass(cb,&p,VK_SUBPASS_CONTENTS_INLINE);vkCmdBindIndexBuffer(cb,indices,0,VK_INDEX_TYPE_UINT32);
    for(uint32_t mode=0;mode<2;++mode){batch.Boundary(emit);vkCmdBindPipeline(cb,VK_PIPELINE_BIND_POINT_GRAPHICS,pipelines[mode]);
      vkCmdSetScissor(cb,0,1,&rect);
      for(uint32_t t=0;t<256;++t){

        if(t==32||t==192){batch.Boundary(emit);VkRect2D clip{{0,0},{t==32?width/2:width,height}};vkCmdSetScissor(cb,0,1,&clip);}
        if(t==96)batch.Flush(emit);
        const bool indexed=scenario%2;
        const uint32_t instances=scenario==4?2:1;
        const bool certified=scenario!=5;
        batch.Push({indexed,3,instances,t*3,indexed?int32_t(scenario):0,scenario%3},batching&&certified,emit);
      }
    }
    batch.Boundary(emit);vkCmdEndRenderPass(cb);
    require(batch.counters.requested==batch.counters.emitted+batch.counters.merged,"Counter conservation");
    if(batching){requested+=batch.counters.requested;emitted+=batch.counters.emitted;saved+=batch.counters.merged;
      require((scenario>=4)==(batch.counters.merged==0),"Instances/unsafe shader exclusion");}
    VkBufferImageCopy copy{};copy.bufferOffset=frame*frame_bytes;copy.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1};copy.imageExtent={width,height,1};
    vkCmdCopyImageToBuffer(cb,image.image,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,readback,1,&copy);
    copy.bufferOffset+=pixels*4;copy.imageSubresource.aspectMask=VK_IMAGE_ASPECT_DEPTH_BIT;
    vkCmdCopyImageToBuffer(cb,image.depth,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,readback,1,&copy);
  }
  VkMemoryBarrier host{VK_STRUCTURE_TYPE_MEMORY_BARRIER};host.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;host.dstAccessMask=VK_ACCESS_HOST_READ_BIT;
  vkCmdPipelineBarrier(cb,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_HOST_BIT,0,1,&host,0,nullptr,0,nullptr);
  check(vkEndCommandBuffer(cb));VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};submit.commandBufferCount=1;submit.pCommandBuffers=&cb;
  check(vkQueueSubmit(queue,1,&submit,VK_NULL_HANDLE));check(vkQueueWaitIdle(queue));
  check(vkMapMemory(device,read_memory,0,VK_WHOLE_SIZE,0,&mapped));auto data=static_cast<uint8_t*>(mapped);uint64_t hash=1469598103934665603ull;
  for(uint32_t s=0;s<frames/2;++s){auto baseline=data+2*s*frame_bytes;auto batched=baseline+frame_bytes;
    require(std::memcmp(baseline,batched,frame_bytes)==0,"Batched color/depth differs from original");
    for(uint32_t b=0;b<frame_bytes;++b){hash^=batched[b];hash*=1099511628211ull;}}
  vkUnmapMemory(device,read_memory);
  vkDestroyCommandPool(device,pool,nullptr);vkDestroyBuffer(device,readback,nullptr);vkFreeMemory(device,read_memory,nullptr);
  vkDestroyBuffer(device,indices,nullptr);vkFreeMemory(device,index_memory,nullptr);for(auto p:pipelines)vkDestroyPipeline(device,p,nullptr);
  vkDestroyPipelineLayout(device,layout,nullptr);vkDestroyShaderModule(device,vs,nullptr);vkDestroyShaderModule(device,fs,nullptr);
  vkDestroyFramebuffer(device,image.frame,nullptr);vkDestroyImageView(device,image.view,nullptr);vkDestroyImageView(device,image.depth_view,nullptr);
  vkDestroyImage(device,image.image,nullptr);vkDestroyImage(device,image.depth,nullptr);vkFreeMemory(device,image.memory,nullptr);vkFreeMemory(device,image.depth_memory,nullptr);
  vkDestroyRenderPass(device,pass,nullptr);vkDestroyDevice(device,nullptr);
  reinterpret_cast<PFN_vkDestroyDebugUtilsMessengerEXT>(vkGetInstanceProcAddr(instance,"vkDestroyDebugUtilsMessengerEXT"))(instance,messenger,nullptr);vkDestroyInstance(instance,nullptr);
  require(!errors&&!warnings&&!hazards,"Vulkan validation failed");
  std::cout<<"PASS: "<<frames/2<<" paired baseline/batched color+depth images; draws "<<requested<<" -> "<<emitted<<" ("<<saved<<" merged); fnv64="<<hash<<"; validation errors="<<errors<<", warnings="<<warnings<<", hazards="<<hazards<<"; synthetic only, not BO2 FPS\n";
}catch(const std::exception&e){std::cerr<<e.what()<<'\n';return 1;}
