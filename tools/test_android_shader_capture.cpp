// SPDX-License-Identifier: GPL-3.0-only

#include "shader_capture.h"
#include <cassert>
#include <fstream>
#include <iostream>
#include <thread>
#include <vector>
using namespace bo2zm::android::capture;
std::vector<uint8_t> blob(uint8_t value) {
  std::vector<uint8_t> b(48,value);
  auto word=[&](size_t at,uint32_t n){for(int i=3;i>=0;--i){b[at+i]=uint8_t(n);n>>=8;}};
  word(0,0x102A1101);word(4,24);word(8,24);return b;
}
int main(int argc,char**argv) {
  assert(argc==2);std::filesystem::path root=std::filesystem::absolute(argv[1]);
  const auto a=blob(1),b=blob(2);
  Start(root/"bounded",48,1);Record(a,true);Record(a,false);Record(a,false);Record(b,false);
  auto invalid=a;invalid[11]=23;Record(invalid,false);Record({},false);
  auto s=Snapshot();assert(s.calls==6&&s.known==1&&s.unique==1&&s.duplicates==1&&s.limited==1&&s.rejected==2&&s.errors==0&&s.bytes==48);
  try{Start(root/"bounded");assert(false);}catch(const std::exception&){}
  Start(root/"threads");std::vector<std::thread> workers;
  for(int i=0;i<8;++i)workers.emplace_back([&]{for(int n=0;n<100;++n){Record(a,false);Record(b,false);}});
  for(auto& t:workers)t.join();s=Snapshot();assert(s.calls==1600&&s.unique==2&&s.duplicates==1598&&s.errors==0&&s.bytes==96);
  int count=0;for(auto& item:std::filesystem::directory_iterator(root/"threads")) {
    assert(item.path().extension()==".bin");std::ifstream f(item.path(),std::ios::binary);
    std::vector<uint8_t> data(48);f.read(reinterpret_cast<char*>(data.data()),48);assert(f&&(data==a||data==b));++count;
  }assert(count==2);
  Start(root/"write-failure");std::filesystem::remove(root/"write-failure");Record(a,false);s=Snapshot();assert(s.errors==1&&s.unique==0&&s.bytes==0&&!s.last_error.empty());
  Start(root/"no-room",0,0);Record(a,false);assert(Snapshot().limited==1);
  std::cout<<"Capture: exact bytes, known exclusion, dedupe, budget, malformed headers, concurrent calls and write failures passed\n";
}
