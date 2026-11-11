// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <algorithm>
#include <cstdint>
#include <mutex>
namespace bo2zm::android {
struct TouchPacket {
  uint32_t packet=0;uint16_t buttons=0;uint8_t lt=0,rt=0;int16_t lx=0,ly=0,rx=0,ry=0;
  bool Neutral()const{return !(buttons|lt|rt|lx|ly|rx|ry);}
};
class TouchState {
  mutable std::mutex mutex_;TouchPacket state_;
 public:
  void Set(int buttons,int lt,int rt,int lx,int ly,int rx,int ry) {
    TouchPacket next;next.buttons=uint16_t(buttons)&0xF3FF;next.lt=uint8_t(std::clamp(lt,0,255));next.rt=uint8_t(std::clamp(rt,0,255));
    auto axis=[](int n){return int16_t(std::clamp(n,-32768,32767));};next.lx=axis(lx);next.ly=axis(ly);next.rx=axis(rx);next.ry=axis(ry);
    std::lock_guard lock(mutex_);
    if(next.buttons==state_.buttons&&next.lt==state_.lt&&next.rt==state_.rt&&next.lx==state_.lx&&next.ly==state_.ly&&next.rx==state_.rx&&next.ry==state_.ry)return;
    next.packet=state_.packet+1;state_=next;
  }
  TouchPacket Get()const{std::lock_guard lock(mutex_);return state_;}
};
}
