// SPDX-License-Identifier: GPL-3.0-only

#define REX_TEST_ATOMIC_REF_COMPAT 1
#include <rex/atomic_ref.h>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <thread>

int main() {
  static_assert(rex::atomic_ref<uint32_t>::is_always_lock_free);
  static_assert(rex::atomic_ref<void*>::required_alignment <= alignof(void*));
  alignas(8) uint32_t ready=0,ack=0;
  uint64_t payload=0;
  std::atomic<bool> stop{false};
  bool valid=true;
  constexpr uint32_t iterations=20000;
  std::thread writer([&] {
    for(uint32_t i=1;i<=iterations;++i) {
      while(rex::atomic_ref<uint32_t>(ack).load(std::memory_order_acquire)!=i-1)
        if(stop.load(std::memory_order_relaxed))return;else std::this_thread::yield();
      payload=0x1234567800000000ull+i;
      rex::atomic_ref<uint32_t>(ready).store(i,std::memory_order_release);
    }
  });
  const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(10);
  for(uint32_t i=1;i<=iterations;++i) {
    while(rex::atomic_ref<uint32_t>(ready).load(std::memory_order_acquire)!=i) {
      if(std::chrono::steady_clock::now()>deadline) {valid=false;break;}
      std::this_thread::yield();
    }
    if(!valid)break;
    if(payload!=0x1234567800000000ull+i) {valid=false;break;}
    rex::atomic_ref<uint32_t>(ack).store(i,std::memory_order_release);
  }
  stop.store(true,std::memory_order_relaxed);writer.join();
  int value=7;int* pointer=nullptr;
  rex::atomic_ref<int*>(pointer).store(&value,std::memory_order_release);
  valid &= rex::atomic_ref<int*>(pointer).load(std::memory_order_acquire)==&value;
  std::cout<<(valid?"PASS":"FAIL")<<": 20000 publish/ack exchanges and pointer load/store\n";
  return valid?0:1;
}
