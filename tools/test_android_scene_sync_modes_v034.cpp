// SPDX-License-Identifier: GPL-3.0-only
#include "app/src/native/render/bo2zm_native_scene_sync.h"
#include "app/src/native/render/bo2zm_native_scene_precise_patch.h"
#include <cstdio>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <string_view>
using bo2zm::native::scene::ImageDependencies;
using bo2zm::native::scene::SelectImageDependencies;
static void ensure(bool ok,const char* m){if(!ok)throw std::runtime_error(m);}
static void has(const char* file,std::string_view what){
 std::ifstream in(file);
 ensure(bool(in),"source file missing");
 std::string s{std::istreambuf_iterator<char>(in),std::istreambuf_iterator<char>()};
 ensure(s.find(what)!=std::string::npos,"source contract missing");
}
int main(){
 try{
  for(int mode=0;mode<=10;++mode){
   const auto deps=ImageDependencies(mode);
   for(bool converted:{false,true})for(bool depth:{false,true}){
    VkRenderPassCreateInfo rp{};
    rp.sType=VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    SelectImageDependencies(rp,deps,mode,converted,depth);
    unsigned expected=2;
    if(mode==6||(mode==4&&converted)||(mode==5&&!converted)||
       (!converted&&mode==9&&!depth)||(!converted&&mode==10&&depth))expected=0;
    else if(mode==1||mode==2||(!converted&&(mode==7||mode==8)))expected=1;
    ensure(rp.dependencyCount==expected,"unexpected dependency count for variant");
    ensure((rp.pDependencies==nullptr)==(expected==0),"dependency pointer must match count");
    if(expected){
      unsigned offset=(mode==2||(!converted&&mode==7))?1:0;
      ensure(rp.pDependencies==deps.data()+offset,"selected incorrect dependency direction");
    }
   }
  }
  const auto stable=ImageDependencies(0);
  ensure(stable[0].srcStageMask==VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,"original incoming stage changed");
  ensure(stable[0].dstStageMask==VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT,"original incoming scope changed");
  ensure(stable[1].dstStageMask==VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,"original outgoing stage changed");
  ensure(stable[0].srcAccessMask==(VK_ACCESS_MEMORY_READ_BIT|VK_ACCESS_MEMORY_WRITE_BIT),"original access masks changed");

  namespace patch = bo2zm::native::scene::precise_patch;
  ensure(patch::kDepDefaultMask==95,"precise patch default mask changed");
  const auto precise=patch::PassDependenciesFor(true,95);
  const auto fallback=patch::PassDependenciesFor(false,95);
  ensure(precise.count==9 && fallback.count==2,"precise patch dependency count changed");
  for(unsigned i=0;i<precise.count;++i){
    ensure(precise.items[i].dependencyFlags==0,"precise patch must not use BY_REGION");
    ensure((precise.items[i].srcStageMask&VK_PIPELINE_STAGE_ALL_COMMANDS_BIT)==0 &&
           (precise.items[i].dstStageMask&VK_PIPELINE_STAGE_ALL_COMMANDS_BIT)==0,
           "precise patch must not use ALL_COMMANDS");
    ensure((precise.items[i].srcStageMask&VK_PIPELINE_STAGE_VERTEX_SHADER_BIT)==0 &&
           (precise.items[i].dstStageMask&VK_PIPELINE_STAGE_VERTEX_SHADER_BIT)==0,
           "precise patch mask95 must leave vertex stage out");
  }
  ensure(patch::PassDependenciesFor(true,127).count==9,"vertex-enabled mask must preserve hazard classes");
  has("app/src/native/render/bo2zm_native_targets.cpp",
      "REXCVAR_DEFINE_BOOL(bo2zm_native_scene_sync_precise, true");
  has("android/app/src/main/java/org/bo2port/diagnostic/GameActivity.java",
      "--bo2zm_native_scene_sync_mask=");
  has("app/src/native/render/bo2zm_native_draws.cpp","formats[4] != 0");
  has("android/game/entry.cpp","scene_pass_sync_strategy");
  std::puts("0.34 Vulkan dependency selection and original-mode regression PASS");
  return 0;
 }catch(const std::exception&e){std::fprintf(stderr,"FAILED: %s\\n",e.what());return 1;}
}
