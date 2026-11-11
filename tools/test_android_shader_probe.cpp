// SPDX-License-Identifier: GPL-3.0-only
#include "bo2zm_native_shader_probe.h"
#include "bo2zm_native_pixel_probe.h"
#include <cassert>
#include <deque>
#include <iostream>
using namespace bo2zm::native;
int main(){
  using namespace shader_probe;
  uint32_t formats[]={37,0,0,0,129};
  assert(Target(kTarget,3,880,768,formats));
  assert(!Target(kTarget,2,880,768,formats));assert(!Target(kTarget,3,881,768,formats));
  assert(!Target(kTarget+1,3,880,768,formats));
  assert(Eligible(true,1,false,false,false,false,true)==Reason::kEligible);
  assert(Eligible(false,0,false,false,false,false,true)==Reason::kNoColorShader);
  assert(Eligible(true,17,false,false,false,false,true)==Reason::kShaderDepth);
  assert(Eligible(true,1,true,false,false,false,true)==Reason::kDiscard);
  assert(Eligible(true,1,false,true,false,false,true)==Reason::kAlphaTest);
  assert(Eligible(true,1,false,false,true,false,true)==Reason::kBlend);
  assert(Eligible(true,1,false,false,false,true,true)==Reason::kQuery);
  assert(Eligible(true,1,false,false,false,false,false)==Reason::kUnsafeShader);
  assert(VertexSpecialization(0x12345|kFlat)==0x12345);
  uint32_t spv[]={0x07230203,0x10000,0,32,0,0x40047,1,11,20};
  assert(!CoverageSafe(spv));spv[8]=22;assert(!CoverageSafe(spv));spv[8]=15;assert(CoverageSafe(spv));
  using namespace pixel_probe;
  Controller c;assert(c.Start(true,false,2,Kind::kShader));
  uint64_t now=100*kSecond;const uint64_t step=kSecond/100;
  Tag frame=c.Boundary(now,now,now,{}, {},true);
  std::deque<Tag> pending;
  for(uint32_t i=0;i<13310;++i){
    Counts counts;counts.draws=2000;counts.triangles=200000;counts.scene_draws=1500;counts.scene_triangles=150000;
    counts.target_passes=1;counts.target_draws=1000;counts.target_elements=300000;counts.eligible_draws=800;
    counts.replaced_draws=frame.clip?800:0;counts.replaced_elements=frame.clip?240000:0;
    counts.shader_reasons[0]=800;counts.shader_reasons[5]=200;
    auto submitted=frame;c.Submitted(submitted,now,false);pending.push_back(submitted);
    now+=step;
    if(pending.size()>2){auto t=pending.front();pending.pop_front();c.Complete(t,true,t.clip?41000000:50000000,true,true,t.clip?7000000:16000000);}
    frame=c.Boundary(now,now,now/10,frame,counts,true);
  }
  while(!pending.empty()){auto t=pending.front();pending.pop_front();c.Complete(t,true,50000000,true,true,16000000);}
  c.Boundary(now,now,now/10,frame,{},true);
  const auto& s=c.Read();assert(s.kind==Kind::kShader && s.state==State::kComplete);
  assert(kDuration==133*kSecond);
  for(size_t i=0;i<4;++i){const auto& b=s.blocks[i];assert(b.frames>2400 && b.gpu_jobs>2400 && !b.gpu_pending);
    assert(b.counts.target_draws==b.frames*1000 && b.counts.eligible_draws==b.frames*800);
    assert(b.counts.replaced_draws==b.frames*((i==1||i==2)?800:0));
    assert(b.target_gpu_jobs==b.gpu_jobs && !b.target_gpu_failed);
    assert(b.target_gpu_ns/b.target_gpu_jobs==uint64_t((i==1||i==2)?7000000:16000000));
  }
  Controller cancelled;assert(cancelled.Start(true,false,2,Kind::kShader));
  assert(!cancelled.Start(true,false,2,Kind::kScissor));
  auto t=cancelled.Boundary(100*kSecond,0,0,{}, {},true);
  t=cancelled.Boundary(146*kSecond,0,0,t,{},true);assert(t.clip && t.kind==Kind::kShader);
  auto pending_old=t;assert(cancelled.Submitted(pending_old,146*kSecond,false));
  cancelled.Cancel("background_or_exit");
  auto restored=cancelled.Boundary(146*kSecond+1,0,0,t,{},true);assert(!restored.trial && !restored.clip);
  assert(cancelled.Start(true,false,2,Kind::kScissor));
  cancelled.Complete(pending_old,true,50000000,true,true,16000000);
  assert(cancelled.Read().kind==Kind::kScissor && cancelled.Read().blocks[1].gpu_jobs==0);
  auto json=Json(s);assert(json.find("flat_fragment")!=std::string::npos && json.find("target_shader")!=std::string::npos);
  std::cout<<json<<'\n';
  std::cout<<"PASS: shader eligibility, exact target, original VS specialization, 133-second ABBA, unchanged geometry, delayed target timestamps and normal restoration\n";
}
