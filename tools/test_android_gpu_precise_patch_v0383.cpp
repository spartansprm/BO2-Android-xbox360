// SPDX-License-Identifier: GPL-3.0-only

#include "app/src/native/render/bo2zm_native_scene_precise_patch.h"
#include <array>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <string_view>
using namespace bo2zm::native::scene::precise_patch;
static void check(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }
static void contains(const char* path, std::string_view needle) {
  std::ifstream input(path);
  check(bool(input), "missing source file");
  std::string content{std::istreambuf_iterator<char>(input),std::istreambuf_iterator<char>()};
  check(content.find(needle) != std::string::npos, "source wiring missing");
}
struct Barrier {
  VkPipelineStageFlags producer=0, consumer=0;
  uint32_t memory_count=0, image_count=0, times=0;
  VkMemoryBarrier memory{};
} recorded;
static VKAPI_ATTR void VKAPI_CALL record_barrier(
    VkCommandBuffer, VkPipelineStageFlags producer, VkPipelineStageFlags consumer,
    VkDependencyFlags, uint32_t memory_count, const VkMemoryBarrier* memory,
    uint32_t, const VkBufferMemoryBarrier*,
    uint32_t image_count, const VkImageMemoryBarrier*) {
  recorded.producer=producer;
  recorded.consumer=consumer;
  recorded.memory_count=memory_count;
  recorded.image_count=image_count;
  recorded.memory = memory_count ? memory[0] : VkMemoryBarrier{};
  ++recorded.times;
}
int main() {
  try {
    check(kDepDefaultMask==95, "uploaded default mask must be 95");
    const auto legacy=PassDependenciesFor(false,95);
    check(legacy.count==2, "legacy must have two dependencies");
    check(legacy.items[0].srcStageMask==VK_PIPELINE_STAGE_ALL_COMMANDS_BIT &&
          legacy.items[0].dstStageMask==VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT &&
          legacy.items[1].dstStageMask==VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
          "precise=false must preserve legacy synchronization");

    const auto precise=PassDependenciesFor(true,95);
    check(precise.count==9, "mask 95 should select nine per-hazard dependencies");
    for (uint32_t i=0;i<precise.count;++i) {
      const auto& dep=precise.items[i];
      check(dep.srcStageMask!=0 && dep.dstStageMask!=0, "empty stage mask");
      check((dep.srcStageMask&VK_PIPELINE_STAGE_ALL_COMMANDS_BIT)==0 &&
            (dep.dstStageMask&VK_PIPELINE_STAGE_ALL_COMMANDS_BIT)==0, "ALL_COMMANDS in precise pass");
      check((dep.srcStageMask&VK_PIPELINE_STAGE_VERTEX_SHADER_BIT)==0 &&
            (dep.dstStageMask&VK_PIPELINE_STAGE_VERTEX_SHADER_BIT)==0, "vertex stage present in default mask");
      check(dep.dependencyFlags==0, "BY_REGION must not be enabled");
    }
    const auto with_vertex=PassDependenciesFor(true,127);
    bool vertex_found=false;
    for(uint32_t i=0;i<with_vertex.count;++i)
      vertex_found|=((with_vertex.items[i].srcStageMask|with_vertex.items[i].dstStageMask)&
                    VK_PIPELINE_STAGE_VERTEX_SHADER_BIT)!=0;
    check(vertex_found,"bit32 must add vertex stage");
    check(PassDependenciesFor(true,0).count==0,"mask zero should disable all patch dependencies");

    recorded={};
    BeforeTransfer(record_barrier,VK_NULL_HANDLE,true,95);
    check(recorded.times==1 && recorded.memory_count==1 &&
          recorded.consumer==VK_PIPELINE_STAGE_TRANSFER_BIT &&
          !(recorded.producer&VK_PIPELINE_STAGE_ALL_COMMANDS_BIT),
          "precise transfer must wait only on TRANSFER destination");
    recorded={};
    BeforeTransfer(record_barrier,VK_NULL_HANDLE,true,95 & ~kDepTransferOut);
    check(recorded.times==0, "without bit16 patch must skip pre-transfer barrier");
    recorded={};
    BeforeTransfer(record_barrier,VK_NULL_HANDLE,true,95 | kDepLegacyTransfer);
    check(recorded.times==1 &&
          recorded.producer==VK_PIPELINE_STAGE_ALL_COMMANDS_BIT &&
          recorded.consumer==VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
          "bit128 must restore legacy transfer barrier");
    recorded={};
    BeforeTransfer(record_barrier,VK_NULL_HANDLE,false,95);
    check(recorded.times==1 &&
          recorded.producer==VK_PIPELINE_STAGE_ALL_COMMANDS_BIT &&
          recorded.consumer==VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
          "precise=false must restore original transfer");
    recorded={};
    BeforeOutputRead(record_barrier,VK_NULL_HANDLE,true);
    check(recorded.times==1 &&
          recorded.consumer==VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT &&
          recorded.producer==(VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT),
          "precise output dependency differs from uploaded patch");
    recorded={};
    BeforeOutputRead(record_barrier,VK_NULL_HANDLE,false);
    check(recorded.producer==VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
          "precise=false original output barrier changed");

    contains("app/src/native/render/bo2zm_native_draws.cpp",
             "scene::precise_patch::PassDependenciesFor(");
    contains("app/src/native/render/bo2zm_native_targets.cpp",
             "scene::precise_patch::BeforeTransfer(");
    contains("app/src/native/render/bo2zm_native_targets.cpp",
             "REXCVAR_DEFINE_BOOL(bo2zm_native_scene_sync_precise, true");
    contains("app/src/native/render/bo2zm_native_targets.cpp",
             "REXCVAR_DEFINE_INT32(bo2zm_native_scene_sync_mask, 95");
    contains("android/app/src/main/java/org/bo2port/diagnostic/GameActivity.java",
             "--bo2zm_native_scene_sync_mask=");
    contains("android/game/entry.cpp","scene_dependencies_precise");
    puts("PASS v0.38.3 uploaded precise Vulkan patch: mask95, fallback, exact barriers, JNI-free startup flags");
    return 0;
  } catch(const std::exception& ex) {
    std::fprintf(stderr,"FAIL v0.38.3: %s\n",ex.what());
    return 1;
  }
}
