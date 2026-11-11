// SPDX-License-Identifier: GPL-3.0-only
#include "../android/game/touch_look.cpp"
#include <cassert>
#include <iostream>
#include <sys/mman.h>
namespace {
int calls=0;int32_t seen_x=0,seen_y=0;
#if BO2_ANDROID_CAMPAIGN
constexpr uint32_t pointer=0x82929120,mouse=0x828D80E4;
#else
constexpr uint32_t pointer=0x82C70F4C,mouse=0x82BE943C;
#endif
void Original(PPCContext&,uint8_t* base){++calls;uint32_t i=Read(base,mouse+16);seen_x=int32_t(Read(base,mouse+i*4));seen_y=int32_t(Read(base,mouse+8+i*4));}
}
#if BO2_ANDROID_CAMPAIGN
extern "C" REX_FUNC(__imp__sub_821EBCF8){Original(ctx,base);}
#define CALL_BRIDGE sub_821EBCF8
#else
extern "C" REX_FUNC(__imp__sub_8225E798){Original(ctx,base);}
#define CALL_BRIDGE sub_8225E798
#endif
int main(){
 auto* base=static_cast<uint8_t*>(mmap(nullptr,0x100000000ull,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS|MAP_NORESERVE,-1,0));assert(base!=MAP_FAILED);
 PPCContext ctx{};ctx.r3.u64=0x90000000;ctx.r4.u64=0x90010000;ctx.r5.u64=0x90010004;Write(base,pointer,ctx.r3.u32);Write(base,mouse+16,0);Write(base,mouse,10);Write(base,mouse+8,uint32_t(-3));
 auto& look=bo2zm::android::RelativeTouch();look.Touch(0,0,0,0,1920,1080);look.Touch(1,0,.5f,.5f,1920,1080);CALL_BRIDGE(ctx,base);
 assert(calls==1&&seen_x==970&&seen_y==537);assert(ctx.r3.u32==0x90000000&&ctx.r4.u32==0x90010000&&ctx.r5.u32==0x90010004);
 Write(base,mouse+16,1);look.Touch(1,0,1,1,1920,1080);ctx.r3.u64=0x90000100;CALL_BRIDGE(ctx,base);assert(seen_x==0&&seen_y==0);ctx.r3.u64=0x90000000;CALL_BRIDGE(ctx,base);assert(seen_x==960&&seen_y==540);
 look.Cancel();CALL_BRIDGE(ctx,base);assert(calls==4&&seen_x==960&&seen_y==540);
 munmap(base,0x100000000ull);std::cout<<"Guest camera bridge "<<(BO2_ANDROID_CAMPAIGN?"CP":"MP/ZM")<<": passed\n";
}
