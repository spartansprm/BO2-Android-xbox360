// SPDX-License-Identifier: GPL-3.0-only
#include "bo2zm_native_pass_profile.h"
#include <iostream>
#include <random>
#include <stdexcept>
#include <thread>
using namespace bo2zm::native::pass_profile;
static void require(bool b){if(!b)throw std::runtime_error("GPU pass profile assertion");}
int main()try {
  Scheduler scheduler;constexpr uint64_t start=100000000000ull;
  require(!scheduler.Next(start,false,1).observe);
  require(!scheduler.Next(start,true,1).observe);
  require(!scheduler.Next(start+kWarmupNs-1,true,1).observe);
  unsigned sampled=0;
  for(uint64_t i=1;i<=192;++i){auto j=scheduler.Next(start+kWarmupNs+i,true,1);require(j.observe);sampled+=j.sampled;}
  require(sampled==3);require(!scheduler.Next(start+kWarmupNs+193,true,2).observe);
  require(!scheduler.Next(start+kWarmupNs+194,false,2).observe);
  Job job;job.observe=job.sampled=true;
  Meta a{2,1280,720,0,123};Meta b{8,1280,720,0,456};
  require(job.Begin(a,1));job.Draw(12,34,3);job.Draw(12,34,6);require(job.End(2));
  require(job.Begin(b,3));job.Draw(56,78,6);require(job.End(4));
  uint64_t values[]={100,110,140,150,190,200};uint8_t cats[]={0,2,0,8,0,255};
  Totals t;require(Add(t,job,values,cats,1,true,true));
  require(t.sampled_jobs==1 && t.sampled_gpu_ns==100 && t.covered_gpu_ns==70 && t.groups==2);
  require(t.passes[0].draws==2 && t.passes[0].pairs[0].draws==2 && t.passes[0].pairs[0].elements==9);
  require(t.category_ns[0]==30 && t.category_ns[2]==30 && t.category_ns[8]==40);
  Job base;base.observe=true;require(Add(t,base,values,cats,1,true,true));require(t.baseline_jobs==1);
  auto before=t.sampled_gpu_ns;require(!Add(t,job,values,cats,1,false,true));require(t.discarded_jobs==1 && t.sampled_gpu_ns==before);
  require(!Add(t,job,values,cats,1,true,false));
  auto invalid=job;invalid.count=kMaxPasses+1;require(!Add(t,invalid,values,cats,1,true,true));
  invalid=job;invalid.passes[1].begin=1;require(!Add(t,invalid,values,cats,1,true,true));
  invalid=job;invalid.passes[1].end=kNone;require(!Add(t,invalid,values,cats,1,true,true));
  uint64_t bad[]={100,110,90,150,190,200};require(!Add(t,job,bad,cats,1,true,true));
  require(!Add(t,job,values,cats,0,true,true));
  require(!Add(t,job,values,cats,1e300,true,true));
  Job budget;budget.observe=budget.sampled=true;
  require(budget.Begin(a,125));require(budget.End(126));require(!budget.Begin(b,127));require(budget.truncated);
  Job full;full.sampled=true;
  for(unsigned n=0;n<kMaxPasses;++n){require(full.Begin(a,1));require(full.End(2));}
  require(!full.Begin(a,3) && full.truncated);
  Job pairs;pairs.sampled=true;require(pairs.Begin(a,1));
  for(uint32_t i=1;i<30;++i)pairs.Draw(i,i+1,3);require(pairs.passes[0].untracked_pair_draws==29-kPairs);

  Totals saturated;
  for(uint32_t i=0;i<kGroups+10;++i){auto j=job;j.passes[0].meta.target=i+1;j.count=1;require(Add(saturated,j,values,cats,1,true,true));}
  require(saturated.groups==kGroups && saturated.unmatched_group_ns==10*30);
  std::mt19937 rng(0x29);
  for(unsigned i=0;i<10000;++i){auto j=job;j.passes[rng()%2].begin=rng();j.passes[rng()%2].end=rng();Totals tmp;(void)Add(tmp,j,values,cats,1,true,true);}
  std::atomic<bool> done{false};
  std::thread writer([&]{for(unsigned i=0;i<1000;++i)Complete(job,values,cats,1,true,true);done=true;});
  while(!done.load()){auto s=Json();require(s.starts_with("{\"schema\":1") && s.ends_with("}"));}
  writer.join();
  require(totals.sampled_jobs==1000);
  std::cout<<"PASS: 1/64 cadence, 10 s warmup, epochs, query budget, exact pass/category accounting, delayed/invalid results, overflow, fuzz and concurrent export\n";
}catch(const std::exception&e){std::cerr<<e.what()<<'\n';return 1;}
