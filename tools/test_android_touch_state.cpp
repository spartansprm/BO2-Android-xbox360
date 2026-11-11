// SPDX-License-Identifier: GPL-3.0-only
#include "touch_gamepad.h"
#include <cassert>
#include <cstddef>
#include <iostream>
#include <thread>
#include <vector>
using namespace bo2zm::android;
int main() {
  TouchState state;assert(state.Get().Neutral()&&state.Get().packet==0);
  state.Set(0x1000,255,0,32767,-32768,-12345,23456);auto p=state.Get();assert(p.packet==1);
  rex::input::X_INPUT_STATE guest;FillTouchGamepad(p,guest);
  assert(uint16_t(guest.gamepad.buttons)==0x1000&&guest.gamepad.left_trigger==255);
  assert(int16_t(guest.gamepad.thumb_lx)==32767&&int16_t(guest.gamepad.thumb_ly)==-32768&&int16_t(guest.gamepad.thumb_rx)==-12345&&int16_t(guest.gamepad.thumb_ry)==23456);
  const auto* bytes=reinterpret_cast<const uint8_t*>(&guest.gamepad.thumb_lx);assert(bytes[0]==0x7f&&bytes[1]==0xff&&bytes[2]==0x80&&bytes[3]==0);
  FillTouchGamepad(p,guest,false);assert(uint16_t(guest.gamepad.buttons)==0&&int16_t(guest.gamepad.thumb_lx)==0&&guest.gamepad.left_trigger==0);
  state.Set(0x1000,255,0,32767,-32768,-12345,23456);assert(state.Get().packet==1);
  state.Set(0xffff,-3,999,-99999,99999,0,0);p=state.Get();assert(p.buttons==0xF3FF&&p.lt==0&&p.rt==255&&p.lx==-32768&&p.ly==32767&&p.packet==2);
  state.Set(0,0,0,0,0,0,0);assert(state.Get().Neutral());
  std::thread writer([&]{for(int i=0;i<20000;++i)state.Set(0,0,0,i,-i,i,-i);});
  for(int i=0;i<20000;++i){p=state.Get();assert(p.lx==-p.ly&&p.rx==-p.ry&&p.lx==p.rx);}
  writer.join();std::cout<<"Native touch state passed: clamps, neutral reset, monotonic changes, real XInput endian mapping and concurrent snapshots\n";
}
