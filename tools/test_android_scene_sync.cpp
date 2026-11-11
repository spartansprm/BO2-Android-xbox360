// SPDX-License-Identifier: GPL-3.0-only


#include "bo2zm_native_scene_sync.h"
#include "bo2zm_native_gpu_marks.h"
#include "bo2zm_native_pass_profile.h"
#include "bo2zm_native_pixel_probe.h"
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
  require(argc==3||argc==4,"Arguments: vertex.spv fragment.spv [negative control or --gpu-marks-detailed|--gpu-marks-endpoints]");
  const std::string mode=argc==4?argv[3]:"corrected";
  const bool negative_pass=mode=="--without-pass-dependencies";
  const bool negative_transfer=mode=="--without-transfer-barriers";
  const bool negative=negative_pass||negative_transfer;
  const bool detailed_marks=mode=="--gpu-marks-detailed";
  const bool pixel_probe=mode=="--pixel-probe";
  const bool pass_profile=mode=="--pass-profile";
  const bool timing_marks=detailed_marks||mode=="--gpu-marks-endpoints"||pixel_probe||pass_profile;
  require(argc==3||negative||timing_marks,"Unknown option");
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
  auto pass=[&](bool source,bool reload=false) {
    VkAttachmentDescription attachments[2]{};
    for(unsigned i=0;i<(source?2u:1u);++i) {
      auto& a=attachments[i];
      a.format=i?VK_FORMAT_D32_SFLOAT:source?VK_FORMAT_A2B10G10R10_UNORM_PACK32:VK_FORMAT_R8G8B8A8_UNORM;
      a.samples=VK_SAMPLE_COUNT_1_BIT;
      a.loadOp=reload?VK_ATTACHMENT_LOAD_OP_LOAD:VK_ATTACHMENT_LOAD_OP_CLEAR;
      a.storeOp=VK_ATTACHMENT_STORE_OP_STORE;
      a.stencilLoadOp=VK_ATTACHMENT_LOAD_OP_DONT_CARE;
      a.stencilStoreOp=VK_ATTACHMENT_STORE_OP_DONT_CARE;
      a.initialLayout=a.finalLayout=VK_IMAGE_LAYOUT_GENERAL;
    }
    VkAttachmentReference color{0,VK_IMAGE_LAYOUT_GENERAL},depth{1,VK_IMAGE_LAYOUT_GENERAL};
    VkSubpassDescription sub{};sub.pipelineBindPoint=VK_PIPELINE_BIND_POINT_GRAPHICS;
    sub.colorAttachmentCount=1;sub.pColorAttachments=&color;sub.pDepthStencilAttachment=source?&depth:nullptr;
    const auto deps=bo2zm::native::scene::ImageDependencies();
    VkRenderPassCreateInfo p{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
    p.attachmentCount=source?2:1;p.pAttachments=attachments;p.subpassCount=1;p.pSubpasses=&sub;
    p.dependencyCount=negative_pass?0:uint32_t(deps.size());p.pDependencies=deps.data();
    VkRenderPass result{};check(vkCreateRenderPass(device,&p,nullptr,&result));return result;
  };
  VkRenderPass producer=pass(true),reload=pass(true,true),consumer=pass(false);
  auto image=[&](bool source) {
    Image result;
    auto create=[&](bool depth,VkImage& handle,VkDeviceMemory& mem,VkImageView& view) {
      VkImageCreateInfo i{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};i.imageType=VK_IMAGE_TYPE_2D;
      i.format=depth?VK_FORMAT_D32_SFLOAT:source?VK_FORMAT_A2B10G10R10_UNORM_PACK32:VK_FORMAT_R8G8B8A8_UNORM;
      i.extent={8,8,1};i.mipLevels=1;i.arrayLayers=1;i.samples=VK_SAMPLE_COUNT_1_BIT;i.tiling=VK_IMAGE_TILING_OPTIMAL;
      i.usage=(depth?VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT:VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT|VK_IMAGE_USAGE_SAMPLED_BIT)|VK_IMAGE_USAGE_TRANSFER_SRC_BIT|VK_IMAGE_USAGE_TRANSFER_DST_BIT;
      check(vkCreateImage(device,&i,nullptr,&handle));VkMemoryRequirements req;vkGetImageMemoryRequirements(device,handle,&req);
      mem=allocate(req,0);check(vkBindImageMemory(device,handle,mem,0));
      VkImageViewCreateInfo v{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};v.image=handle;v.viewType=VK_IMAGE_VIEW_TYPE_2D;v.format=i.format;
      v.subresourceRange={depth?VK_IMAGE_ASPECT_DEPTH_BIT:VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
      check(vkCreateImageView(device,&v,nullptr,&view));
    };
    create(false,result.image,result.memory,result.view);
    if(source)create(true,result.depth,result.depth_memory,result.depth_view);
    const VkImageView views[]={result.view,result.depth_view};
    VkFramebufferCreateInfo f{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};f.renderPass=source?producer:consumer;
    f.attachmentCount=source?2:1;f.pAttachments=views;f.width=f.height=8;f.layers=1;
    check(vkCreateFramebuffer(device,&f,nullptr,&result.frame));return result;
  };
  Image sources[]={image(true),image(true),image(true)},destination=image(false);
  VkSamplerCreateInfo sc{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};sc.magFilter=sc.minFilter=VK_FILTER_NEAREST;sc.mipmapMode=VK_SAMPLER_MIPMAP_MODE_NEAREST;sc.addressModeU=sc.addressModeV=sc.addressModeW=VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;VkSampler sampler{};check(vkCreateSampler(device,&sc,nullptr,&sampler));
  VkDescriptorSetLayoutBinding binding{0,VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,1,VK_SHADER_STAGE_FRAGMENT_BIT,nullptr};VkDescriptorSetLayoutCreateInfo sl{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};sl.bindingCount=1;sl.pBindings=&binding;VkDescriptorSetLayout layout{};check(vkCreateDescriptorSetLayout(device,&sl,nullptr,&layout));VkDescriptorPoolSize size{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,3};VkDescriptorPoolCreateInfo dp{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};dp.maxSets=3;dp.poolSizeCount=1;dp.pPoolSizes=&size;VkDescriptorPool descriptors{};check(vkCreateDescriptorPool(device,&dp,nullptr,&descriptors));VkDescriptorSetLayout layouts[]={layout,layout,layout};VkDescriptorSetAllocateInfo sa{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};sa.descriptorPool=descriptors;sa.descriptorSetCount=3;sa.pSetLayouts=layouts;VkDescriptorSet sets[3];check(vkAllocateDescriptorSets(device,&sa,sets));for(unsigned i=0;i<3;++i){VkDescriptorImageInfo info{sampler,sources[i].view,VK_IMAGE_LAYOUT_GENERAL};VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};w.dstSet=sets[i];w.dstBinding=0;w.descriptorCount=1;w.descriptorType=VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;w.pImageInfo=&info;vkUpdateDescriptorSets(device,1,&w,0,nullptr);}
  auto shader=[&](const char* path){std::ifstream f(path,std::ios::binary|std::ios::ate);require(bool(f),"Shader missing");auto n=f.tellg();require(n>0&&n%4==0,"Invalid shader size");std::vector<uint32_t> code(size_t(n)/4);f.seekg(0);f.read(reinterpret_cast<char*>(code.data()),n);VkShaderModuleCreateInfo s{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};s.codeSize=size_t(n);s.pCode=code.data();VkShaderModule m{};check(vkCreateShaderModule(device,&s,nullptr,&m));return m;};
  VkShaderModule vs=shader(argv[1]),ps=shader(argv[2]);VkPipelineShaderStageCreateInfo stages[2]={{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO},{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO}};stages[0].stage=VK_SHADER_STAGE_VERTEX_BIT;stages[0].module=vs;stages[1].stage=VK_SHADER_STAGE_FRAGMENT_BIT;stages[1].module=ps;for(auto& s:stages)s.pName="main";
  VkPipelineLayoutCreateInfo pl{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};pl.setLayoutCount=1;pl.pSetLayouts=&layout;VkPipelineLayout pipeline_layout{};check(vkCreatePipelineLayout(device,&pl,nullptr,&pipeline_layout));VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};ia.topology=VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;VkViewport viewport{0,0,8,8,0,1};VkRect2D rect{{0,0},{8,8}};VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};vp.viewportCount=vp.scissorCount=1;vp.pViewports=&viewport;vp.pScissors=&rect;VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};rs.polygonMode=VK_POLYGON_MODE_FILL;rs.lineWidth=1;VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};ms.rasterizationSamples=VK_SAMPLE_COUNT_1_BIT;VkPipelineColorBlendAttachmentState blend{};blend.colorWriteMask=15;VkPipelineColorBlendStateCreateInfo bs{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};bs.attachmentCount=1;bs.pAttachments=&blend;VkGraphicsPipelineCreateInfo gp{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};gp.stageCount=2;gp.pStages=stages;gp.pVertexInputState=&vi;gp.pInputAssemblyState=&ia;gp.pViewportState=&vp;gp.pRasterizationState=&rs;gp.pMultisampleState=&ms;gp.pColorBlendState=&bs;gp.layout=pipeline_layout;gp.renderPass=consumer;VkPipeline pipeline{};check(vkCreateGraphicsPipelines(device,VK_NULL_HANDLE,1,&gp,nullptr,&pipeline));


  VkDynamicState scissor_dynamic=VK_DYNAMIC_STATE_SCISSOR;
  VkPipelineDynamicStateCreateInfo dynamic{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
  dynamic.dynamicStateCount=1;dynamic.pDynamicStates=&scissor_dynamic;gp.pDynamicState=&dynamic;
  VkPipeline probe_pipeline{};
  if(pixel_probe)check(vkCreateGraphicsPipelines(device,VK_NULL_HANDLE,1,&gp,nullptr,&probe_pipeline));
  constexpr unsigned frames=180,frame_bytes=8*8*8;VkBufferCreateInfo bc{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};bc.size=frames*frame_bytes;bc.usage=VK_BUFFER_USAGE_TRANSFER_DST_BIT;VkBuffer buffer{};check(vkCreateBuffer(device,&bc,nullptr,&buffer));VkMemoryRequirements req;vkGetBufferMemoryRequirements(device,buffer,&req);VkDeviceMemory readback=allocate(req,VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);check(vkBindBufferMemory(device,buffer,readback,0));
  VkCommandPoolCreateInfo pc{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};pc.queueFamilyIndex=family;VkCommandPool pool{};check(vkCreateCommandPool(device,&pc,nullptr,&pool));VkCommandBufferAllocateInfo ca{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};ca.commandPool=pool;ca.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY;ca.commandBufferCount=1;VkCommandBuffer cb{};check(vkAllocateCommandBuffers(device,&ca,&cb));VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};check(vkBeginCommandBuffer(cb,&begin));
  auto transfer=[&] {if(!negative_transfer)bo2zm::native::scene::BeforeTransfer(vkCmdPipelineBarrier,cb);};
  auto prepare=[&](VkImage image,VkImageAspectFlags aspect) {
    VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};b.oldLayout=VK_IMAGE_LAYOUT_UNDEFINED;
    b.newLayout=VK_IMAGE_LAYOUT_GENERAL;b.srcQueueFamilyIndex=b.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;
    b.image=image;b.subresourceRange={aspect,0,1,0,1};b.dstAccessMask=VK_ACCESS_MEMORY_READ_BIT|VK_ACCESS_MEMORY_WRITE_BIT;
    vkCmdPipelineBarrier(cb,VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,0,0,nullptr,0,nullptr,1,&b);
  };
  for(const auto& i:sources){prepare(i.image,VK_IMAGE_ASPECT_COLOR_BIT);prepare(i.depth,VK_IMAGE_ASPECT_DEPTH_BIT);}
  prepare(destination.image,VK_IMAGE_ASPECT_COLOR_BIT);
  VkQueryPool timestamps{};
  std::vector<std::vector<uint8_t>> frame_marks(frames);
  std::vector<bo2zm::native::pass_profile::Job> pass_jobs(pass_profile?frames:0);
  uint64_t queries_reset=0;
  if(timing_marks) {
    require(families[family].timestampValidBits>0,"Queue has no GPU timestamp support");
    VkQueryPoolCreateInfo pool_info{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
    pool_info.queryType=VK_QUERY_TYPE_TIMESTAMP;
    pool_info.queryCount=frames*bo2zm::native::timing::kMarksPerSlot;
    check(vkCreateQueryPool(device,&pool_info,nullptr,&timestamps));
  }
  for(unsigned f=0;f<frames;++f) {
    const uint32_t query_base=f*bo2zm::native::timing::kMarksPerSlot;
    bo2zm::native::pass_profile::Job inactive;
    auto& job=pass_profile?pass_jobs[f]:inactive;
    if(pass_profile){job.observe=true;job.sampled=(f+1)%bo2zm::native::pass_profile::kPeriod==0;}
    const bool category_marks=detailed_marks||job.sampled;
    auto mark=[&](uint8_t category){if(timing_marks &&
        (job.active==bo2zm::native::pass_profile::kNone || frame_marks[f].size()+2<bo2zm::native::timing::kMarksPerSlot))
      bo2zm::native::timing::Category(vkCmdWriteTimestamp,cb,timestamps,query_base,frame_marks[f],category,category_marks,true);};
    auto profile_begin=[&](uint32_t category,uint64_t target){
      bo2zm::native::pass_profile::Meta meta{category,8,8,0,target};
      if(job.Begin(meta,uint32_t(frame_marks[f].size())))bo2zm::native::timing::Category(vkCmdWriteTimestamp,cb,
        timestamps,query_base,frame_marks[f],uint8_t(category),true,true,true);};
    auto profile_end=[&]{if(job.End(uint32_t(frame_marks[f].size())))bo2zm::native::timing::Category(vkCmdWriteTimestamp,cb,
        timestamps,query_base,frame_marks[f],0,true,true,true);};
    if(timing_marks){auto count=bo2zm::native::timing::ResetCount(category_marks);
      queries_reset+=count;vkCmdResetQueryPool(cb,timestamps,query_base,count);}
    mark(0);mark(2);
    const unsigned slot=f%3;VkClearValue clear[2]{};
    for(unsigned c=0;c<3;++c)clear[0].color.float32[c]=float((f*(73+18*c)+31*c)%256)/255;
    clear[0].color.float32[3]=1;clear[1].depthStencil={0.25f+float(f%4)*0.125f,0};
    VkRenderPassBeginInfo p{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};p.renderPass=producer;
    p.framebuffer=sources[slot].frame;p.renderArea=rect;p.clearValueCount=2;p.pClearValues=clear;
    profile_begin(2,100);vkCmdBeginRenderPass(cb,&p,VK_SUBPASS_CONTENTS_INLINE);vkCmdEndRenderPass(cb);profile_end();

    mark(2);
    p.renderPass=reload;profile_begin(2,100);vkCmdBeginRenderPass(cb,&p,VK_SUBPASS_CONTENTS_INLINE);vkCmdEndRenderPass(cb);profile_end();
    p.renderPass=consumer;p.framebuffer=destination.frame;p.clearValueCount=1;
    VkClearValue output_clear{};output_clear.color.float32[3]=1;
    if(pixel_probe)p.pClearValues=&output_clear;
    mark(5);
    profile_begin(8,200);
    vkCmdBeginRenderPass(cb,&p,VK_SUBPASS_CONTENTS_INLINE);
    vkCmdBindPipeline(cb,VK_PIPELINE_BIND_POINT_GRAPHICS,pixel_probe?probe_pipeline:pipeline);
    if(pixel_probe) {
      const bool clipped=f%4==1||f%4==2;
      VkRect2D scissor=clipped?VkRect2D{{2,3},{6,5}}:rect;
      if(f%8==5)scissor.extent.width=0;
      bo2zm::native::pixel_probe::Clip(scissor,clipped);
      vkCmdSetScissor(cb,0,1,&scissor);
    }
    vkCmdBindDescriptorSets(cb,VK_PIPELINE_BIND_POINT_GRAPHICS,pipeline_layout,0,1,&sets[slot],0,nullptr);
    job.Draw(1001,2002,3);vkCmdDraw(cb,3,1,0,0);vkCmdEndRenderPass(cb);profile_end();
    VkBufferImageCopy read{};read.bufferOffset=f*frame_bytes;read.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1};read.imageExtent={8,8,1};
    mark(6);
    transfer();vkCmdCopyImageToBuffer(cb,destination.image,VK_IMAGE_LAYOUT_GENERAL,buffer,1,&read);
    read.bufferOffset+=8*8*4;read.imageSubresource.aspectMask=VK_IMAGE_ASPECT_DEPTH_BIT;
    transfer();vkCmdCopyImageToBuffer(cb,sources[slot].depth,VK_IMAGE_LAYOUT_GENERAL,buffer,1,&read);

    VkImageCopy copy{};copy.srcSubresource=copy.dstSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1};copy.extent={8,8,1};
    transfer();vkCmdCopyImage(cb,sources[slot].image,VK_IMAGE_LAYOUT_GENERAL,destination.image,VK_IMAGE_LAYOUT_GENERAL,1,&copy);
    VkClearColorValue poison{};const VkImageSubresourceRange color_range{VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
    mark(7);
    transfer();vkCmdClearColorImage(cb,sources[slot].image,VK_IMAGE_LAYOUT_GENERAL,&poison,1,&color_range);
    transfer();vkCmdClearColorImage(cb,destination.image,VK_IMAGE_LAYOUT_GENERAL,&poison,1,&color_range);
    const VkClearDepthStencilValue far{1,0};const VkImageSubresourceRange depth_range{VK_IMAGE_ASPECT_DEPTH_BIT,0,1,0,1};
    transfer();vkCmdClearDepthStencilImage(cb,sources[slot].depth,VK_IMAGE_LAYOUT_GENERAL,&far,1,&depth_range);
    if(timing_marks)bo2zm::native::timing::End(vkCmdWriteTimestamp,cb,timestamps,query_base,frame_marks[f]);
  }
  VkMemoryBarrier host{VK_STRUCTURE_TYPE_MEMORY_BARRIER};host.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;host.dstAccessMask=VK_ACCESS_HOST_READ_BIT;vkCmdPipelineBarrier(cb,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_HOST_BIT,0,1,&host,0,nullptr,0,nullptr);check(vkEndCommandBuffer(cb));VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};submit.commandBufferCount=1;submit.pCommandBuffers=&cb;check(vkQueueSubmit(queue,1,&submit,VK_NULL_HANDLE));check(vkQueueWaitIdle(queue));
  void* bytes{};check(vkMapMemory(device,readback,0,VK_WHOLE_SIZE,0,&bytes));unsigned pixels=0,depth_pixels=0;
  uint64_t pixel_hash=14695981039346656037ull;
  if(!negative)for(size_t i=0;i<frames*frame_bytes;++i){pixel_hash^=static_cast<uint8_t*>(bytes)[i];pixel_hash*=1099511628211ull;}
  for(unsigned f=0;f<frames;++f)for(unsigned pixel=0;pixel<64;++pixel) {
    auto* p=static_cast<uint8_t*>(bytes)+f*frame_bytes+pixel*4;
    const bool clipped=pixel_probe&&(f%4==1||f%4==2);
    const bool visible=!clipped||(f%8!=5&&pixel==3*8+2);
    for(unsigned c=0;c<4;++c){int expected=c==3?255:visible?(f*(73+18*c)+31*c)%256:0;if(!negative)require(std::abs(int(p[c])-expected)<=1,"Color readback mismatch");}
    float z;std::memcpy(&z,static_cast<uint8_t*>(bytes)+f*frame_bytes+8*8*4+pixel*4,4);
    if(!negative)require(z==0.25f+float(f%4)*0.125f,"Depth readback mismatch");
    ++pixels;++depth_pixels;
  }
  vkUnmapMemory(device,readback);
  if(probe_pipeline)vkDestroyPipeline(device,probe_pipeline,nullptr);
  uint64_t timestamp_count=0;
  bo2zm::native::pass_profile::Totals profile_totals;
  if(timing_marks) {
    for(unsigned f=0;f<frames;++f) {
      auto& marks=frame_marks[f];
      if(!pass_profile)require(marks.size()==(detailed_marks?6:2),"Unexpected GPU mark count");
      else require(pass_jobs[f].sampled?marks.size()>2:marks.size()==2,"Sample cadence changed");
      require(marks.back()==bo2zm::native::timing::kEnd,"Missing final GPU mark");
      std::vector<uint64_t> values(marks.size());
      check(vkGetQueryPoolResults(device,timestamps,f*bo2zm::native::timing::kMarksPerSlot,uint32_t(values.size()),
          values.size()*sizeof(uint64_t),values.data(),sizeof(uint64_t),VK_QUERY_RESULT_64_BIT));
      for(size_t i=1;i<values.size();++i)require(values[i]>=values[i-1],"GPU timestamps went backwards");
      if(pass_profile){VkPhysicalDeviceProperties props;vkGetPhysicalDeviceProperties(physical,&props);
        require(bo2zm::native::pass_profile::Add(profile_totals,pass_jobs[f],values,marks,props.limits.timestampPeriod,true,true),"Pass sample rejected");}
      timestamp_count+=values.size();
    }
    if(pass_profile){
      require(profile_totals.sampled_jobs==frames/64 && profile_totals.baseline_jobs==frames-frames/64,"Wrong sampled/baseline counts");
      require(profile_totals.groups==3 && profile_totals.covered_gpu_ns<=profile_totals.sampled_gpu_ns,"Pass coverage invalid");
      require(!profile_totals.failed_jobs && !profile_totals.truncated_jobs,"Failed or truncated test sample");
    }
    vkDestroyQueryPool(device,timestamps,nullptr);
  }
  vkDestroyCommandPool(device,pool,nullptr);vkDestroyBuffer(device,buffer,nullptr);vkFreeMemory(device,readback,nullptr);vkDestroyPipeline(device,pipeline,nullptr);vkDestroyPipelineLayout(device,pipeline_layout,nullptr);vkDestroyShaderModule(device,vs,nullptr);vkDestroyShaderModule(device,ps,nullptr);vkDestroyDescriptorPool(device,descriptors,nullptr);vkDestroyDescriptorSetLayout(device,layout,nullptr);vkDestroySampler(device,sampler,nullptr);auto destroy=[&](Image i){vkDestroyFramebuffer(device,i.frame,nullptr);vkDestroyImageView(device,i.view,nullptr);vkDestroyImage(device,i.image,nullptr);vkFreeMemory(device,i.memory,nullptr);if(i.depth){vkDestroyImageView(device,i.depth_view,nullptr);vkDestroyImage(device,i.depth,nullptr);vkFreeMemory(device,i.depth_memory,nullptr);}};for(auto i:sources)destroy(i);destroy(destination);vkDestroyRenderPass(device,producer,nullptr);vkDestroyRenderPass(device,reload,nullptr);vkDestroyRenderPass(device,consumer,nullptr);vkDestroyDevice(device,nullptr);reinterpret_cast<PFN_vkDestroyDebugUtilsMessengerEXT>(vkGetInstanceProcAddr(instance,"vkDestroyDebugUtilsMessengerEXT"))(instance,messenger,nullptr);vkDestroyInstance(instance,nullptr);
  if(negative)require(hazards>0,"Negative control failed to detect missing synchronization");else require(errors==0&&warnings==0,"Validation messages in corrected path");
  std::cout<<mode<<": "<<frames<<" frames, "<<pixels<<" color pixels, "<<depth_pixels<<" depth pixels; errors="<<errors<<", warnings="<<warnings<<", hazards="<<hazards;
  if(!negative)std::cout<<"; pixels_fnv64="<<pixel_hash;
  if(timing_marks)std::cout<<"; timestamp_writes="<<timestamp_count<<"; queries_reset="<<queries_reset;
  if(pass_profile)std::cout<<"; sampled_jobs="<<profile_totals.sampled_jobs<<"; baseline_jobs="<<profile_totals.baseline_jobs<<"; pass_groups="<<profile_totals.groups;
  std::cout<<'\n';return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
