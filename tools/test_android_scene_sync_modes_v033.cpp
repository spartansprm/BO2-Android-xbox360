// SPDX-License-Identifier: GPL-3.0-only


#include "app/src/native/render/bo2zm_native_scene_sync.h"
#include <cstdlib>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <string_view>
using bo2zm::native::scene::ImageDependencies;
using bo2zm::native::scene::SelectImageDependencies;
static void expect(bool cond,const char* message){
 if(!cond)throw std::runtime_error(message);
}
static void contains(const char* file,std::string_view token){
 std::ifstream f(file);
 if(!f)throw std::runtime_error(std::string("missing file: ")+file);
 const std::string text{std::istreambuf_iterator<char>(f),std::istreambuf_iterator<char>()};
 if(text.find(token)==std::string::npos)throw std::runtime_error(std::string("missing token: ")+std::string(token));
}
int main(){
 try{
  for(int mode=0;mode<=6;++mode){
   const auto deps=ImageDependencies(mode);
   for(bool converted : {false,true}){
    VkRenderPassCreateInfo info{};
    info.sType=VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    SelectImageDependencies(info,deps,mode,converted);
    const int expected=(mode==6||(mode==4&&converted)||(mode==5&&!converted))?0:
                       (mode==1||mode==2)?1:2;
    expect(info.dependencyCount==static_cast<unsigned>(expected),"incorrect Vulkan dependency count");
    expect((info.pDependencies==nullptr)==(expected==0),"incorrect null dependency pointer");
    if(expected)expect(info.pDependencies==deps.data()+(mode==2?1:0),"wrong dependency direction");
   }
  }
  const auto original=ImageDependencies(0);
  expect(original[0].srcStageMask==VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,"baseline incoming changed");
  expect(original[0].dstStageMask==VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT,"baseline incoming destination changed");
  expect(original[1].dstStageMask==VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,"baseline outgoing changed");
  expect(original[0].srcAccessMask==(VK_ACCESS_MEMORY_READ_BIT|VK_ACCESS_MEMORY_WRITE_BIT),"baseline access changed");
  const auto scoped=ImageDependencies(3);
  expect(scoped[0].srcStageMask==VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,"scoped incoming producer must remain global");
  expect((scoped[0].dstStageMask&VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT)!=0,"scoped color stage missing");
  expect((scoped[0].dstStageMask&VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT)!=0,"scoped fragment stage missing");
  expect((scoped[0].dstStageMask&VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT)!=0,"scoped depth stage missing");
  expect((scoped[0].dstAccessMask&VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT)!=0,"depth writes missing");
  expect((scoped[0].dstAccessMask&VK_ACCESS_SHADER_READ_BIT)!=0,"sampled read missing");
  expect(scoped[1].dstStageMask==VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,"scoped outgoing consumer narrowed unsafely");
  contains("android/app/src/main/java/org/bo2port/diagnostic/GameActivity.java","getIntExtra(\"scene_pass_sync_mode\"");
  contains("android/game/entry.cpp","scene_pass_sync_strategy");
  std::puts("v0.33: Vulkan mode selection and original baseline checks PASS");
  return 0;
 }catch(const std::exception&e){std::fprintf(stderr,"FAILED: %s\n",e.what());return 1;}
}
