// SPDX-License-Identifier: GPL-3.0-only
#include "game_shader_probe.h"
#include "bo2zm_shader_library.h"
#include "bo2zm_shader_index.h"
#include "bo2zm_native_shader_profile.h"
#include "port_probe_shaders.h"
#include <algorithm>
#include <array>
#include <chrono>
#include <fstream>
#include <map>
#include <stdexcept>

namespace {
void require(bool ok,const char* message) {if(!ok)throw std::runtime_error(message);}
void check(VkResult status,GameShaderProbeResult& r,const char* message) {
  r.vk_result=status;require(status==VK_SUCCESS,message);
}
struct Module {
  VkDevice device;VkShaderModule handle=VK_NULL_HANDLE;
  ~Module(){if(handle)vkDestroyShaderModule(device,handle,nullptr);}
  void make(std::span<const uint32_t> words,GameShaderProbeResult& r) {
    require(bo2zm::native::UboOnlySpirv(words),"Shader violates UBO-only profile");
    VkShaderModuleCreateInfo c{};c.sType=VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    c.codeSize=words.size_bytes();c.pCode=words.data();
    check(vkCreateShaderModule(device,&c,nullptr,&handle),r,"Driver rejected shader module");
  }
};
struct Objects {
  VkDevice device;std::array<VkDescriptorSetLayout,5> sets{};
  VkPipelineLayout layout=VK_NULL_HANDLE;VkRenderPass pass=VK_NULL_HANDLE;
  VkPipelineCache cache=VK_NULL_HANDLE;
  ~Objects() {
    if(cache)vkDestroyPipelineCache(device,cache,nullptr);
    if(pass)vkDestroyRenderPass(device,pass,nullptr);
    if(layout)vkDestroyPipelineLayout(device,layout,nullptr);
    for(auto set:sets)if(set)vkDestroyDescriptorSetLayout(device,set,nullptr);
  }
  void make(GameShaderProbeResult& r) {
    constexpr uint32_t capacities[]={16384,64,256,512};
    const VkShaderStageFlags stages=VK_SHADER_STAGE_VERTEX_BIT|VK_SHADER_STAGE_FRAGMENT_BIT;
    const VkDescriptorBindingFlags flags=VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT|
        VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT|VK_DESCRIPTOR_BINDING_UPDATE_UNUSED_WHILE_PENDING_BIT;
    for(uint32_t i=0;i<4;++i) {
      VkDescriptorSetLayoutBinding binding{0,i==3?VK_DESCRIPTOR_TYPE_SAMPLER:VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,capacities[i],stages,nullptr};
      VkDescriptorSetLayoutBindingFlagsCreateInfo f{};f.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO;f.bindingCount=1;f.pBindingFlags=&flags;
      VkDescriptorSetLayoutCreateInfo c{};c.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;c.pNext=&f;
      c.flags=VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT;c.bindingCount=1;c.pBindings=&binding;
      VkDescriptorSetLayoutSupport support{};support.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_SUPPORT;
      vkGetDescriptorSetLayoutSupport(device,&c,&support);require(support.supported,"Texture descriptor layout unsupported");
      check(vkCreateDescriptorSetLayout(device,&c,nullptr,&sets[i]),r,"Create texture descriptor layout failed");
    }
    std::array<VkDescriptorSetLayoutBinding,3> bindings{};
    for(uint32_t i=0;i<3;++i)bindings[i]={i,VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC,1,stages,nullptr};
    VkDescriptorSetLayoutCreateInfo s{};s.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;s.bindingCount=3;s.pBindings=bindings.data();
    check(vkCreateDescriptorSetLayout(device,&s,nullptr,&sets[4]),r,"Create UBO layout failed");
    VkPipelineLayoutCreateInfo l{};l.sType=VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;l.setLayoutCount=5;l.pSetLayouts=sets.data();
    check(vkCreatePipelineLayout(device,&l,nullptr,&layout),r,"Create UBO pipeline layout failed");
    std::array<VkAttachmentDescription,5> attachments{};
    for(uint32_t i=0;i<5;++i) {
      auto& a=attachments[i];a.format=i==4?VK_FORMAT_D32_SFLOAT:VK_FORMAT_R8G8B8A8_UNORM;a.samples=VK_SAMPLE_COUNT_1_BIT;
      a.loadOp=VK_ATTACHMENT_LOAD_OP_DONT_CARE;a.storeOp=VK_ATTACHMENT_STORE_OP_DONT_CARE;
      a.stencilLoadOp=VK_ATTACHMENT_LOAD_OP_DONT_CARE;a.stencilStoreOp=VK_ATTACHMENT_STORE_OP_DONT_CARE;
      a.initialLayout=VK_IMAGE_LAYOUT_UNDEFINED;a.finalLayout=i==4?VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL:VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    }
    std::array<VkAttachmentReference,4> color{};
    for(uint32_t i=0;i<4;++i)color[i]={i,VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkAttachmentReference depth{4,VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
    VkSubpassDescription sub{};sub.pipelineBindPoint=VK_PIPELINE_BIND_POINT_GRAPHICS;sub.colorAttachmentCount=4;sub.pColorAttachments=color.data();sub.pDepthStencilAttachment=&depth;
    VkRenderPassCreateInfo p{};p.sType=VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;p.attachmentCount=5;p.pAttachments=attachments.data();p.subpassCount=1;p.pSubpasses=&sub;
    check(vkCreateRenderPass(device,&p,nullptr,&pass),r,"Create compile-test render pass failed");
    VkPipelineCacheCreateInfo c{};c.sType=VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO;
    check(vkCreatePipelineCache(device,&c,nullptr,&cache),r,"Create temporary pipeline cache failed");
  }
};


std::vector<VkVertexInputAttributeDescription> inputs(std::span<const uint32_t> w) {
  struct Type {uint32_t kind=0,element=0,count=0,sign=0;};
  std::map<uint32_t,Type> types;std::map<uint32_t,uint32_t> locations,pointers;
  for(size_t i=5;i<w.size();) {
    const uint32_t n=w[i]>>16,op=w[i]&65535;require(n&&n<=w.size()-i,"Invalid shader instruction");const auto* a=w.data()+i+1;
    if(op==71&&n==4&&a[1]==30)locations[a[0]]=a[2];
    else if(op==21&&n==4)types[a[0]]={op,a[1],1,a[2]};
    else if(op==22&&n==3)types[a[0]]={op,a[1],1,0};
    else if(op==23&&n==4)types[a[0]]={op,a[1],a[2],0};
    else if(op==32&&n==4)types[a[0]]={op,a[2],a[1],0};
    else if(op==59&&n>=4&&a[2]==1)pointers[a[1]]=a[0];
    i+=n;
  }
  std::vector<VkVertexInputAttributeDescription> out;
  for(auto [id,location]:locations) {
    if(!pointers.contains(id))continue;
    require(location<16,"Vertex input location exceeds compile-test layout");
    auto ptr=types.at(pointers.at(id));require(ptr.kind==32,"Input is not a pointer");
    auto t=types.at(ptr.element);uint32_t count=1;
    if(t.kind==23){count=t.count;t=types.at(t.element);}
    require(count>=1&&count<=4&&t.element==32&&(t.kind==21||t.kind==22),"Unsupported vertex input type");
    constexpr VkFormat formats[3][4]={{VK_FORMAT_R32_SFLOAT,VK_FORMAT_R32G32_SFLOAT,VK_FORMAT_R32G32B32_SFLOAT,VK_FORMAT_R32G32B32A32_SFLOAT},
      {VK_FORMAT_R32_UINT,VK_FORMAT_R32G32_UINT,VK_FORMAT_R32G32B32_UINT,VK_FORMAT_R32G32B32A32_UINT},
      {VK_FORMAT_R32_SINT,VK_FORMAT_R32G32_SINT,VK_FORMAT_R32G32B32_SINT,VK_FORMAT_R32G32B32A32_SINT}};
    out.push_back({location,0,formats[t.kind==22?0:t.sign?2:1][count-1],location*16});
  }
  return out;
}

void pipeline(Objects& objects,VkShaderModule vs,VkShaderModule ps,
              std::span<const uint32_t> vertex,GameShaderProbeResult& r) {
  const uint32_t bits=1u<<8;const VkSpecializationMapEntry map{0,0,sizeof(bits)};
  const VkSpecializationInfo spec{1,&map,sizeof(bits),&bits};
  std::array<VkPipelineShaderStageCreateInfo,2> stages{};
  for(unsigned i=0;i<2;++i) {auto& s=stages[i];s.sType=VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;s.stage=i?VK_SHADER_STAGE_FRAGMENT_BIT:VK_SHADER_STAGE_VERTEX_BIT;s.module=i?ps:vs;s.pName="main";}
  stages[r.stage=="vertex"?0:1].pSpecializationInfo=&spec;
  auto attributes=inputs(vertex);VkVertexInputBindingDescription binding{0,256,VK_VERTEX_INPUT_RATE_VERTEX};
  VkPipelineVertexInputStateCreateInfo v{};v.sType=VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
  v.vertexBindingDescriptionCount=attributes.empty()?0:1;v.pVertexBindingDescriptions=&binding;v.vertexAttributeDescriptionCount=uint32_t(attributes.size());v.pVertexAttributeDescriptions=attributes.data();
  VkPipelineInputAssemblyStateCreateInfo a{};a.sType=VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;a.topology=VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
  VkViewport viewport{0,0,16,16,0,1};VkRect2D scissor{{0,0},{16,16}};
  VkPipelineViewportStateCreateInfo view{};view.sType=VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;view.viewportCount=1;view.pViewports=&viewport;view.scissorCount=1;view.pScissors=&scissor;
  VkPipelineRasterizationStateCreateInfo raster{};raster.sType=VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;raster.polygonMode=VK_POLYGON_MODE_FILL;raster.cullMode=VK_CULL_MODE_NONE;raster.lineWidth=1;
  VkPipelineMultisampleStateCreateInfo m{};m.sType=VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;m.rasterizationSamples=VK_SAMPLE_COUNT_1_BIT;
  VkPipelineDepthStencilStateCreateInfo d{};d.sType=VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;d.depthCompareOp=VK_COMPARE_OP_ALWAYS;
  std::array<VkPipelineColorBlendAttachmentState,4> blend{};for(auto& b:blend)b.colorWriteMask=15;
  VkPipelineColorBlendStateCreateInfo b{};b.sType=VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;b.attachmentCount=4;b.pAttachments=blend.data();
  VkGraphicsPipelineCreateInfo c{};c.sType=VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;c.stageCount=2;c.pStages=stages.data();c.pVertexInputState=&v;c.pInputAssemblyState=&a;c.pViewportState=&view;c.pRasterizationState=&raster;c.pMultisampleState=&m;c.pDepthStencilState=&d;c.pColorBlendState=&b;c.layout=objects.layout;c.renderPass=objects.pass;
  VkPipeline handle=VK_NULL_HANDLE;auto status=vkCreateGraphicsPipelines(objects.device,objects.cache,1,&c,nullptr,&handle);
  if(handle)vkDestroyPipeline(objects.device,handle,nullptr);
  check(status,r,"Driver rejected game shader pipeline");
}
}

GameShaderProbeResult ProbeGameShaderPipelines(VkPhysicalDevice physical,VkDevice device,
    const std::filesystem::path& pack,const GameShaderProgress& progress) {
  GameShaderProbeResult r;const auto start=std::chrono::steady_clock::now();
  auto phase=[&](const char* name){r.phase=name;progress(r);};
  try {
    phase("game_shaders_read_index");
    require(std::filesystem::file_size(pack)<=uint64_t(2)*1024*1024*1024,"Shader pack exceeds diagnostic budget");
    bo2zm::shaderlib::ShaderLibrary library;const auto info=library.Open(pack);r.expected=info.amount;
    require(r.expected>0&&r.expected<=65536,"Unsupported diagnostic shader count");
    std::ifstream f(bo2zm::native::ShaderIndexPath(pack),std::ios::binary|std::ios::ate);
    require(bool(f)&&f.tellg()>0&&f.tellg()<=8*1024*1024,"Missing or oversized shader index");
    std::vector<uint8_t> bytes(size_t(f.tellg()));f.seekg(0);f.read(reinterpret_cast<char*>(bytes.data()),bytes.size());require(bool(f),"Short shader index read");
    bo2zm::native::ShaderIndex index;if(const char* why=bo2zm::native::DecodeShaderIndex(bytes,info,index))throw std::runtime_error(why);
    require(index.entries.size()==r.expected&&index.stats.ignored==0,"Index has unreadable/missing entries");r.index_loaded=true;
    VkPhysicalDeviceProperties properties{};vkGetPhysicalDeviceProperties(physical,&properties);const auto& l=properties.limits;
    require(l.maxBoundDescriptorSets>=5&&l.maxVertexOutputComponents>=88&&l.maxFragmentInputComponents>=88&&l.maxColorAttachments>=4&&l.maxVertexInputAttributes>=16&&l.maxVertexInputBindingStride>=256,"Pipeline diagnostic exceeds device limits");
    {
      phase("game_shaders_create_layouts");Objects objects{device};objects.make(r);
      Module vertex{device},pixel{device};vertex.make(bo2portprobe::kPipelineVertex,r);pixel.make(bo2portprobe::kPipelineFragment,r);
      library.Scan(pack,[&](uint32_t number,const bo2zm::shaderlib::Shader& shader) {
        r.current=number;r.stage=shader.vertices?"vertex":"pixel";
        const auto& entry=index.entries.at(number);
        require(entry.container_hash==shader.hash&&entry.container_offset==shader.container_offset&&entry.container_size==shader.original.size()&&entry.spirv_hash==shader.spirv_hash&&entry.spirv_words==shader.spirv_words,"Index/pack entry mismatch");
        auto words=bo2zm::shaderlib::Spirv(shader);require(!words.empty(),"Shader payload/hash read failed");
        phase("game_shaders_create_module");Module module{device};module.make(words,r);++r.modules_created;
        phase("game_shaders_create_pipeline");
        pipeline(objects,shader.vertices?module.handle:vertex.handle,shader.vertices?pixel.handle:module.handle,
                 shader.vertices?std::span<const uint32_t>(words):std::span<const uint32_t>(bo2portprobe::kPipelineVertex),r);
        ++r.pipelines_created;if(shader.vertices)++r.vertex;else ++r.pixel;
      });
      r.pack_verified=true;phase("game_shaders_destroy_objects");
    }
    r.objects_destroyed=true;r.passed=r.pipelines_created==r.expected&&r.modules_created==r.expected;r.phase="game_shaders_complete";
  }catch(const std::exception& e){r.error=e.what();                                              }
  r.elapsed_ms=uint64_t(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now()-start).count());
  return r;
}
