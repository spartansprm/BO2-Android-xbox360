// SPDX-License-Identifier: GPL-3.0-only
#include "bo2zm_native_draw_batch.h"
#include <algorithm>
#include <array>
#include <cassert>
#include <fstream>
#include <iostream>
#include <random>
#include <thread>
#include <vector>
using namespace bo2zm::native::draw_batch;
static void require(bool v) { if (!v) throw std::runtime_error("draw batch assertion"); }
static std::vector<uint32_t> spirv(std::initializer_list<uint32_t> extra = {}) {
  std::vector<uint32_t> w{0x07230203,0x10000,0,64,0,
      0x20011,1,
      0x5000f,0,1,0x6e69616d,0,
      0x40020,2,3,3,
      0x4003b,2,4,3,
      0x3003e,4,5};
  w.insert(w.end(),extra);return w;
}
static void unit() {
  std::vector<Draw> emitted;
  auto emit=[&](const Draw& d){emitted.push_back(d);};
  Coalescer c;
  c.Push({false,3,1,0},true,emit);c.Push({false,6,1,3},true,emit);
  require(emitted.empty());c.Flush(emit);
  require(emitted.size()==1 && emitted[0].count==9 && c.counters.merged==1);
  emitted.clear();c={};
  c.Push({true,3,1,0,-7,2},true,emit);c.Push({true,3,1,3,-7,2},true,emit);c.Boundary(emit);
  c.Push({true,3,1,6,-7,2},true,emit);c.Flush(emit);
  require(emitted.size()==2 && emitted[0].count==6 && emitted[1].count==3);


  const Draw base{true,3,1,0,-7,2};
  for (const auto& d : std::vector<Draw>{{false,3,1,3,-7,2},{true,3,1,3,-6,2},
       {true,3,1,3,-7,3},{true,3,2,3,-7,2},{true,3,1,4,-7,2},
       {true,2,1,3,-7,2},{true,0,1,3,-7,2}}) {
    c={};emitted.clear();c.Push(base,true,emit);c.Push(d,true,emit);c.Flush(emit);
    require(emitted.size()==2 && c.counters.merged==0);
  }
  c={};emitted.clear();c.Push(base,true,emit);c.Push({true,3,1,3,-7,2},false,emit);c.Flush(emit);
  require(emitted.size()==2 && c.counters.merged==0);
  c={};emitted.clear();c.Push({false,4294967295u,1,0},true,emit);
  c.Push({false,3,1,4294967295u},true,emit);c.Flush(emit);require(c.counters.merged==0);
  c={};emitted.clear();c.Push({false,3,1,4294967294u},true,emit);
  c.Push({false,3,1,1},true,emit);c.Flush(emit);require(c.counters.merged==0);


  std::mt19937 rng(0xb028);unsigned scenarios=0;
  for(int run=0;run<1000;++run) {
    c={};emitted.clear();std::vector<std::array<int64_t,4>> expected,actual;
    auto expand=[](const Draw& d,auto& out){for(uint32_t i=0;i<d.count;++i)
      out.push_back({d.indexed,int64_t(d.first)+i+d.vertex_offset,d.instances,d.first_instance});};
    uint32_t first=0;
    for(int n=0;n<40;++n) {
      Draw d{bool(rng()%2),uint32_t(3*(1+rng()%4)),uint32_t(1+rng()%2),first,int32_t(rng()%3),uint32_t(rng()%2)};
      expand(d,expected);c.Push(d,rng()%4!=0,emit);first+=d.count;
      if(rng()%7==0)c.Boundary(emit);
      if(rng()%11==0)c.Flush(emit);
    }
    c.Flush(emit);for(auto d:emitted)expand(d,actual);
    require(expected==actual && c.counters.requested==c.counters.emitted+c.counters.merged);++scenarios;
  }
  require(ShaderSafe(spirv()));
  require(!ShaderSafe(spirv({0x40047,4,11,7})));
  require(!ShaderSafe(spirv({0x40047,4,11,4426})));
  require(!ShaderSafe(spirv({0x50048,8,0,11,7})));
  require(!ShaderSafe(spirv({0x20011,61})));
  require(!ShaderSafe(spirv({0x20011,53})));
  require(!ShaderSafe(spirv({0x40020,9,12,3,0x4003b,9,10,12,0x3003e,10,5})));
  require(!ShaderSafe(spirv({0x3003e,63,5})));
  require(!ShaderSafe(spirv({0x40063,4,4,0})));
  require(!ShaderSafe(spirv({0x40063,4,4,0,0x100e0})));
  auto malformed=spirv();malformed.push_back(0);require(!ShaderSafe(malformed));
  malformed=spirv();malformed.push_back(0xffff0000);require(!ShaderSafe(malformed));

  for(int n=0;n<10000;++n){auto w=spirv();w[rng()%w.size()]=rng();(void)ShaderSafe(w);}
  std::array<uint8_t,16> old{},same{},changed{};changed[8]=1;
  require(SameConstants(true,3,3,9,8,old,same));
  require(!SameConstants(true,3,3,9,8,old,changed));
  require(!SameConstants(false,3,3,9,8,old,same));
  require(!SameConstants(true,4,3,9,8,old,same));
  require(!SameConstants(true,3,3,8,8,old,same));
  require(!SameConstants(true,3,3,9,8,std::span(old).first(8),same));
  std::atomic<bool> done{false};
  std::thread writer([&]{for(uint64_t i=1;i<=10000;++i)Publish({i*3,i*2,i,i*2,i,i});done.store(true);});
  unsigned snapshots=0;
  while(!done.load()) {
    auto json=Json();
    if(json.find("\"available\":true")==std::string::npos)continue;
    auto number=[&](const char* key){auto at=json.find(std::string("\"")+key+"\":");require(at!=std::string::npos);
      return std::stoull(json.substr(at+std::strlen(key)+3));};
    require(number("requested_total")==number("emitted_total")+number("merged_total"));++snapshots;
  }
  writer.join();require(snapshots>0);Publish({});
  std::cout<<"PASS: merge guards, ordering across "<<scenarios<<" streams, shader fallback/fuzz, constant invalidation, concurrent counter publication\n";
}
int main(int argc,char**argv)try {
  unit();
  if(argc==2) {
    std::ifstream f(argv[1],std::ios::binary);require(bool(f));uint32_t h[6];f.read(reinterpret_cast<char*>(h),24);
    uint64_t safe=0,total=h[3];require(total>0 && total<100000);
    for(uint64_t i=0;i<total;++i){uint32_t e[4];f.read(reinterpret_cast<char*>(e),16);require(bool(f));
      require(e[0]<1000000 && e[1]<1000000);f.seekg(e[0],std::ios::cur);
      std::vector<uint32_t>w(e[1]);f.read(reinterpret_cast<char*>(w.data()),w.size()*4);require(bool(f));safe+=ShaderSafe(w);}
    std::cout<<"Shader safety scan: "<<safe<<'/'<<total<<" library modules eligible (not a runtime draw reduction)\n";
  }
}catch(const std::exception&e){std::cerr<<e.what()<<'\n';return 1;}
