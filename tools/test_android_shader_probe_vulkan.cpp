// SPDX-License-Identifier: GPL-3.0-only


#include "bo2zm_native_scene_sync.h"
#include "bo2zm_native_draw_batch.h"
#include "bo2zm_native_shader_probe.h"
#include "bo2zm_native_gpu_marks.h"
#include "../app/src/native/shaders/bo2zm_probe_flat_ps.h"
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
  require(argc==5||argc==6,"Arguments: vertex opaque discard depth [--unmasked-vs|--unsafe-depth]");
  const std::string mode=argc==6?argv[5]:"corrected";
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
  constexpr uint32_t width=880,height=768,pixels=width*height,frame_bytes=pixels*8,frames=18;
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
    require(bool(f),"Shader read failed");VkShaderModuleCreateInfo mc{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};mc.codeSize=bytes;mc.pCode=w.data();
    VkShaderModule m{};check(vkCreateShaderModule(device,&mc,nullptr,&m));return m;};
  VkShaderModule vs=shader(argv[1]),opaque=shader(argv[2]),discard=shader(argv[3]),depth_shader=shader(argv[4]),flat{};
  VkShaderModuleCreateInfo fm{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};fm.codeSize=sizeof(kSpirvBo2zmProbeFlat);fm.pCode=kSpirvBo2zmProbeFlat;
  require(bo2zm::native::shader_probe::CoverageSafe(kSpirvBo2zmProbeFlat),"Flat module writes coverage/depth");check(vkCreateShaderModule(device,&fm,nullptr,&flat));
  VkPipelineLayoutCreateInfo lc{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};VkPipelineLayout layout{};check(vkCreatePipelineLayout(device,&lc,nullptr,&layout));
  VkPipelineShaderStageCreateInfo stages[2]={{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO},{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO}};
  stages[0].stage=VK_SHADER_STAGE_VERTEX_BIT;stages[0].module=vs;stages[1].stage=VK_SHADER_STAGE_FRAGMENT_BIT;
  stages[0].pName=stages[1].pName="main";
  VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
  VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};ia.topology=VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
  VkViewport viewport{0,0,float(width),float(height),0,1};VkRect2D rect{{0,0},{width,height}};
  VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};vp.viewportCount=vp.scissorCount=1;vp.pViewports=&viewport;vp.pScissors=&rect;
  VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};rs.polygonMode=VK_POLYGON_MODE_FILL;rs.lineWidth=1;
  VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};ms.rasterizationSamples=VK_SAMPLE_COUNT_1_BIT;
  VkPipelineDepthStencilStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};ds.depthTestEnable=ds.depthWriteEnable=VK_TRUE;ds.depthCompareOp=VK_COMPARE_OP_LESS;
  VkPipelineColorBlendAttachmentState ba{};ba.colorWriteMask=15;
  ba.srcColorBlendFactor=VK_BLEND_FACTOR_SRC_ALPHA;ba.dstColorBlendFactor=VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;ba.colorBlendOp=VK_BLEND_OP_ADD;
  ba.srcAlphaBlendFactor=VK_BLEND_FACTOR_ONE;ba.dstAlphaBlendFactor=VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;ba.alphaBlendOp=VK_BLEND_OP_ADD;
  VkPipelineColorBlendStateCreateInfo bs{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};bs.attachmentCount=1;bs.pAttachments=&ba;
  VkGraphicsPipelineCreateInfo gp{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};gp.stageCount=2;gp.pStages=stages;
  gp.pVertexInputState=&vi;gp.pInputAssemblyState=&ia;gp.pViewportState=&vp;gp.pRasterizationState=&rs;gp.pMultisampleState=&ms;
  gp.pDepthStencilState=&ds;gp.pColorBlendState=&bs;gp.layout=layout;gp.renderPass=pass;
  VkPipeline pipelines[6]{};
  const VkSpecializationMapEntry map{0,0,sizeof(uint32_t)};
  for(uint32_t i=0;i<6;++i){
    uint32_t spec=256u|(i>=4?bo2zm::native::shader_probe::kFlat:0);
    if(i!=5)spec=bo2zm::native::shader_probe::VertexSpecialization(spec);
    VkSpecializationInfo info{1,&map,sizeof(spec),&spec};stages[0].pSpecializationInfo=&info;
    stages[1].module=i==1?discard:i==2?depth_shader:i>=4?flat:opaque;
    ba.blendEnable=i==3;ds.depthWriteEnable=i==3?VK_FALSE:VK_TRUE;
    check(vkCreateGraphicsPipelines(device,VK_NULL_HANDLE,1,&gp,nullptr,&pipelines[i]));
  }
  VkBufferCreateInfo bc{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};bc.size=frames*frame_bytes;bc.usage=VK_BUFFER_USAGE_TRANSFER_DST_BIT;
  VkBuffer buffer{};check(vkCreateBuffer(device,&bc,nullptr,&buffer));VkMemoryRequirements req;vkGetBufferMemoryRequirements(device,buffer,&req);
  VkDeviceMemory readback=allocate(req,VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);check(vkBindBufferMemory(device,buffer,readback,0));
  VkCommandPoolCreateInfo pc{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};pc.queueFamilyIndex=family;VkCommandPool pool{};check(vkCreateCommandPool(device,&pc,nullptr,&pool));
  VkCommandBufferAllocateInfo ca{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};ca.commandPool=pool;ca.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY;ca.commandBufferCount=1;
  VkCommandBuffer cb{};check(vkAllocateCommandBuffers(device,&ca,&cb));VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};check(vkBeginCommandBuffer(cb,&begin));
  VkQueryPoolCreateInfo qc{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};qc.queryType=VK_QUERY_TYPE_TIMESTAMP;qc.queryCount=frames*4;
  VkQueryPool queries{};check(vkCreateQueryPool(device,&qc,nullptr,&queries));
  uint64_t replacements=0,fallbacks=0;std::vector<std::vector<uint8_t>> marks(frames);
  for(uint32_t frame=0;frame<frames;++frame){
    const bool treatment=frame%2;const uint32_t scenario=frame/2;
    uint32_t formats[]={37,0,0,0,129};
    const bool target=bo2zm::native::shader_probe::Target(bo2zm::native::shader_probe::kTarget+(scenario==4),3,width,height,formats);
    const auto reason=bo2zm::native::shader_probe::Eligible(true,scenario==2?17:1,scenario==1,scenario==8,scenario==3,false,scenario!=7);
    const bool ready=scenario!=6;
    bool replaced=treatment && scenario!=5 && target && reason==bo2zm::native::shader_probe::Reason::kEligible && ready;
    if(mode=="--unsafe-depth" && scenario==2 && treatment)replaced=true;
    fallbacks+=treatment&&target&&reason==bo2zm::native::shader_probe::Reason::kEligible&&!ready;
    replacements+=replaced;
    const uint32_t original=scenario==1||scenario==8?1:scenario==2?2:scenario==3?3:0;
    const auto pipeline=replaced?pipelines[mode=="--unmasked-vs"?5:4]:pipelines[original];
    vkCmdResetQueryPool(cb,queries,frame*4,4);
    bo2zm::native::timing::Category(vkCmdWriteTimestamp,cb,queries,frame*4,marks[frame],0,false,true);
    bo2zm::native::timing::Category(vkCmdWriteTimestamp,cb,queries,frame*4,marks[frame],2,true,true,true);
    VkClearValue clear[2]{};clear[0].color.float32[3]=1;clear[1].depthStencil.depth=1;
    VkRenderPassBeginInfo p{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};p.renderPass=pass;p.framebuffer=image.frame;p.renderArea=rect;p.clearValueCount=2;p.pClearValues=clear;
    vkCmdBeginRenderPass(cb,&p,VK_SUBPASS_CONTENTS_INLINE);vkCmdBindPipeline(cb,VK_PIPELINE_BIND_POINT_GRAPHICS,pipeline);vkCmdDraw(cb,3,1,0,0);vkCmdEndRenderPass(cb);
    bo2zm::native::timing::Category(vkCmdWriteTimestamp,cb,queries,frame*4,marks[frame],0,true,true,true);
    VkBufferImageCopy copy{};copy.bufferOffset=frame*frame_bytes;copy.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1};copy.imageExtent={width,height,1};
    vkCmdCopyImageToBuffer(cb,image.image,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,buffer,1,&copy);
    copy.bufferOffset+=pixels*4;copy.imageSubresource.aspectMask=VK_IMAGE_ASPECT_DEPTH_BIT;vkCmdCopyImageToBuffer(cb,image.depth,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,buffer,1,&copy);
    bo2zm::native::timing::End(vkCmdWriteTimestamp,cb,queries,frame*4,marks[frame]);require(marks[frame].size()==4,"Target timestamp budget");
  }
  VkMemoryBarrier host{VK_STRUCTURE_TYPE_MEMORY_BARRIER};host.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;host.dstAccessMask=VK_ACCESS_HOST_READ_BIT;
  vkCmdPipelineBarrier(cb,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_HOST_BIT,0,1,&host,0,nullptr,0,nullptr);check(vkEndCommandBuffer(cb));
  VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};submit.commandBufferCount=1;submit.pCommandBuffers=&cb;check(vkQueueSubmit(queue,1,&submit,VK_NULL_HANDLE));check(vkQueueWaitIdle(queue));
  uint64_t invalid_depth_pairs=0;void* bytes{};check(vkMapMemory(device,readback,0,VK_WHOLE_SIZE,0,&bytes));auto data=static_cast<uint8_t*>(bytes);
  for(uint32_t scenario=0;scenario<frames/2;++scenario){auto a=data+2*scenario*frame_bytes;auto b=a+frame_bytes;
    bool same_depth=std::memcmp(a+pixels*4,b+pixels*4,pixels*4)==0;invalid_depth_pairs+=!same_depth;
    if(mode=="corrected")require(same_depth,"Depth or geometry coverage changed");
    if(scenario==0 && mode!="--unmasked-vs"){
      require(std::memcmp(a,b,pixels*4)!=0,"Flat treatment did not change color");
      for(uint32_t p=0;p<pixels;++p)require(b[p*4]==255&&b[p*4+1]==0&&b[p*4+2]==255&&b[p*4+3]==255,"Unexpected flat pixel");
    }else if(!(mode=="--unsafe-depth"&&scenario==2) && !(mode=="--unmasked-vs"&&scenario==0))
      require(std::memcmp(a,b,pixels*4)==0,"Excluded or restored draw changed color");
  }
  vkUnmapMemory(device,readback);
  for(uint32_t f=0;f<frames;++f){uint64_t ts[4];check(vkGetQueryPoolResults(device,queries,f*4,4,sizeof(ts),ts,sizeof(uint64_t),VK_QUERY_RESULT_64_BIT));
    require(std::is_sorted(ts,ts+4),"Timestamp ordering");require(ts[2]-ts[1]<=ts[3]-ts[0],"Target exceeds whole job");}
  vkDestroyQueryPool(device,queries,nullptr);vkDestroyCommandPool(device,pool,nullptr);vkDestroyBuffer(device,buffer,nullptr);vkFreeMemory(device,readback,nullptr);
  for(auto p:pipelines)vkDestroyPipeline(device,p,nullptr);vkDestroyPipelineLayout(device,layout,nullptr);
  for(auto m:{vs,opaque,discard,depth_shader,flat})vkDestroyShaderModule(device,m,nullptr);
  vkDestroyFramebuffer(device,image.frame,nullptr);vkDestroyImageView(device,image.view,nullptr);vkDestroyImageView(device,image.depth_view,nullptr);
  vkDestroyImage(device,image.image,nullptr);vkDestroyImage(device,image.depth,nullptr);vkFreeMemory(device,image.memory,nullptr);vkFreeMemory(device,image.depth_memory,nullptr);vkDestroyRenderPass(device,pass,nullptr);
  vkDestroyDevice(device,nullptr);reinterpret_cast<PFN_vkDestroyDebugUtilsMessengerEXT>(vkGetInstanceProcAddr(instance,"vkDestroyDebugUtilsMessengerEXT"))(instance,messenger,nullptr);vkDestroyInstance(instance,nullptr);
  require(!errors&&!warnings&&!hazards,"Vulkan validation failed");
  if(mode=="corrected")require(replacements==1&&fallbacks==1&&!invalid_depth_pairs,"Incorrect replacement/fallback selection");
  else require(invalid_depth_pairs>0,"Negative control did not detect corrupted depth/coverage");
  std::cout<<mode<<": PASS, "<<frames/2<<" paired 880x768 scenarios, depth mismatches="<<invalid_depth_pairs
    <<", 4 queries/job, errors="<<errors<<", warnings="<<warnings<<", hazards="<<hazards<<"; synthetic only\n";
}catch(const std::exception&e){std::cerr<<e.what()<<'\n';return 1;}
