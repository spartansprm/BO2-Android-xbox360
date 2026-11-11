// SPDX-License-Identifier: GPL-3.0-only


#include "game_shader_probe.h"
#include "bo2zm_shader_library.h"
#include "bo2zm_shader_index.h"
#include "port_probe_shaders.h"
#include <atomic>
#include <cstring>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <unistd.h>

namespace fs=std::filesystem;
void require(bool b,const char* msg){if(!b)throw std::runtime_error(msg);}
std::atomic<unsigned> errors{0},warnings{0};
VKAPI_ATTR VkBool32 VKAPI_CALL message(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
    VkDebugUtilsMessageTypeFlagsEXT,const VkDebugUtilsMessengerCallbackDataEXT* data,void*) {
  if(severity&VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT){++errors;std::cerr<<data->pMessage<<'\n';}
  else if(severity&VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT)++warnings;
  return VK_FALSE;
}
struct Context {
  VkInstance instance{};VkDebugUtilsMessengerEXT messenger{};VkDevice device{};VkPhysicalDevice physical{};
  ~Context(){if(device)vkDestroyDevice(device,nullptr);if(messenger)reinterpret_cast<PFN_vkDestroyDebugUtilsMessengerEXT>(vkGetInstanceProcAddr(instance,"vkDestroyDebugUtilsMessengerEXT"))(instance,messenger,nullptr);if(instance)vkDestroyInstance(instance,nullptr);}
  bool init() {
    const char* layer="VK_LAYER_KHRONOS_validation";const char* extension=VK_EXT_DEBUG_UTILS_EXTENSION_NAME;
    VkApplicationInfo app{};app.sType=VK_STRUCTURE_TYPE_APPLICATION_INFO;app.apiVersion=VK_API_VERSION_1_2;
    VkInstanceCreateInfo c{};c.sType=VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;c.pApplicationInfo=&app;c.enabledLayerCount=1;c.ppEnabledLayerNames=&layer;c.enabledExtensionCount=1;c.ppEnabledExtensionNames=&extension;
    require(vkCreateInstance(&c,nullptr,&instance)==VK_SUCCESS,"Vulkan instance/validation layer unavailable");
    VkDebugUtilsMessengerCreateInfoEXT m{};m.sType=VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT;m.messageSeverity=VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT|VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT;m.messageType=VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT|VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;m.pfnUserCallback=message;
    auto make=reinterpret_cast<PFN_vkCreateDebugUtilsMessengerEXT>(vkGetInstanceProcAddr(instance,"vkCreateDebugUtilsMessengerEXT"));require(make&&make(instance,&m,nullptr,&messenger)==VK_SUCCESS,"Cannot create validation messenger");
    uint32_t count=0;vkEnumeratePhysicalDevices(instance,&count,nullptr);if(!count)return false;std::vector<VkPhysicalDevice> all(count);vkEnumeratePhysicalDevices(instance,&count,all.data());physical=all[0];
    VkPhysicalDeviceVulkan12Features f12{};f12.sType=VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
    VkPhysicalDeviceFeatures2 f{};f.sType=VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;f.pNext=&f12;vkGetPhysicalDeviceFeatures2(physical,&f);
    if(!f.features.shaderSampledImageArrayDynamicIndexing||!f12.runtimeDescriptorArray||!f12.scalarBlockLayout||!f12.descriptorBindingPartiallyBound||!f12.descriptorBindingSampledImageUpdateAfterBind||!f12.descriptorBindingUpdateUnusedWhilePending)return false;
    VkPhysicalDeviceProperties properties{};vkGetPhysicalDeviceProperties(physical,&properties);std::cout<<"ICD: "<<properties.deviceName<<'\n';
    count=0;vkGetPhysicalDeviceQueueFamilyProperties(physical,&count,nullptr);std::vector<VkQueueFamilyProperties> queues(count);vkGetPhysicalDeviceQueueFamilyProperties(physical,&count,queues.data());uint32_t family=UINT32_MAX;
    for(uint32_t i=0;i<count;++i)if(queues[i].queueCount&&(queues[i].queueFlags&VK_QUEUE_GRAPHICS_BIT)){family=i;break;}
    if(family==UINT32_MAX)return false;
    VkPhysicalDeviceFeatures features{};features.shaderSampledImageArrayDynamicIndexing=VK_TRUE;
    VkPhysicalDeviceVulkan12Features enabled{};enabled.sType=VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
    enabled.scalarBlockLayout=enabled.runtimeDescriptorArray=enabled.descriptorBindingPartiallyBound=enabled.descriptorBindingSampledImageUpdateAfterBind=enabled.descriptorBindingUpdateUnusedWhilePending=VK_TRUE;
    float priority=1;VkDeviceQueueCreateInfo q{};q.sType=VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;q.queueFamilyIndex=family;q.queueCount=1;q.pQueuePriorities=&priority;
    VkDeviceCreateInfo d{};d.sType=VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;d.pNext=&enabled;d.pEnabledFeatures=&features;d.queueCreateInfoCount=1;d.pQueueCreateInfos=&q;
    require(vkCreateDevice(physical,&d,nullptr,&device)==VK_SUCCESS,"Create device failed");return true;
  }
};
std::vector<uint8_t> bytes(const fs::path& path){std::ifstream f(path,std::ios::binary|std::ios::ate);require(bool(f)&&f.tellg()>0,"Cannot read test file");std::vector<uint8_t> data(size_t(f.tellg()));f.seekg(0);f.read(reinterpret_cast<char*>(data.data()),data.size());require(bool(f),"Short test file read");return data;}
void save(const fs::path& path,std::span<const uint8_t> data){std::ofstream f(path,std::ios::binary);f.write(reinterpret_cast<const char*>(data.data()),data.size());require(bool(f),"Test file write failed");}
void makePack(const fs::path& path,bool legacy=false) {
  std::vector<bo2zm::shaderlib::Shader> shaders;
  for(bool vertex:{false,true}) {
    bo2zm::shaderlib::Shader s;s.original.resize(140);
    auto be=[&](size_t at,uint32_t value){for(unsigned i=0;i<4;++i)s.original[at+i]=uint8_t(value>>(24-8*i));};
    be(0,0x102A1100|uint32_t(vertex));be(4,128);be(8,12);be(16,80);be(24,36);be(40,12);if(!vertex)be(64,1);
    std::span<const uint32_t> code=vertex?std::span<const uint32_t>(bo2portprobe::kPipelineVertex):std::span<const uint32_t>(bo2portprobe::kPipelineFragment);
    s.spirv.assign(code.begin(),code.end());if(legacy)s.spirv.insert(s.spirv.begin()+5,{0x00020011,11});shaders.push_back(std::move(s));
  }
  save(path,bo2zm::shaderlib::PackShaders(std::move(shaders)));
  bo2zm::shaderlib::ShaderLibrary library;auto info=library.Open(path);bo2zm::native::ShaderIndex index;bo2zm::native::BuildShaderIndex(library,path,info,index);
  save(bo2zm::native::ShaderIndexPath(path),bo2zm::native::EncodeShaderIndex(index,info));
}
int main(int argc,char** argv) try {
  require(argc<=2,"Optional argument: private shader pack");Context context;
  if(!context.init()){std::cout<<"SKIP: Vulkan descriptor/scalar feature set unavailable\n";return 77;}
  fs::path temp=fs::temp_directory_path()/("bo2-game-shader-probe-"+std::to_string(getpid()));fs::create_directories(temp);
  struct Remove{fs::path path;~Remove(){fs::remove_all(path);}}remove{temp};
  auto progress=[](const GameShaderProbeResult& r){if(r.phase=="game_shaders_create_pipeline"&&r.current%100==0)std::cout<<"pipeline "<<r.current<<'/'<<r.expected<<'\n';};
  auto pack=temp/"authored.spvpack";makePack(pack);
  auto result=ProbeGameShaderPipelines(context.physical,context.device,pack,progress);
  if(!result.passed)std::cerr<<result.phase<<": "<<result.error<<'\n';
  require(result.passed&&result.pipelines_created==2&&result.vertex==1&&result.pixel==1&&result.index_loaded&&result.pack_verified&&result.objects_destroyed,"Authored pack failed");
  auto idx=bo2zm::native::ShaderIndexPath(pack);auto broken=bytes(idx);broken.back()^=1;save(idx,broken);
  result=ProbeGameShaderPipelines(context.physical,context.device,pack,progress);
  require(!result.passed&&!result.index_loaded&&result.modules_created==0,"Corrupt index was accepted");
  makePack(pack,true);result=ProbeGameShaderPipelines(context.physical,context.device,pack,progress);
  require(!result.passed&&result.modules_created==0&&result.error=="Shader violates UBO-only profile","Int64 module was accepted");
  if(argc==2) {
    result=ProbeGameShaderPipelines(context.physical,context.device,argv[1],progress);
    std::cout<<"real_pack: passed="<<result.passed<<" modules="<<result.modules_created<<" pipelines="<<result.pipelines_created<<" vertex="<<result.vertex<<" pixel="<<result.pixel<<" pack="<<result.pack_verified<<" destroyed="<<result.objects_destroyed<<" ms="<<result.elapsed_ms<<" phase="<<result.phase<<" error="<<result.error<<'\n';
    require(result.passed,"Real game pack failed");
  }
  std::cout<<"validation_errors="<<errors<<" validation_warnings="<<warnings<<"; authored/negative cases passed\n";
  require(errors==0,"Vulkan validation errors");return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
