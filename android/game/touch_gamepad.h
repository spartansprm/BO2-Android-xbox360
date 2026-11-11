// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "touch_state.h"
#include <rex/input/input.h>
namespace bo2zm::android {
inline void FillTouchGamepad(const TouchPacket& t,rex::input::X_INPUT_STATE& output,bool active=true) {
  output={};output.packet_number=t.packet;
  if(!active)return;
  auto& g=output.gamepad;g.buttons=t.buttons;g.left_trigger=t.lt;g.right_trigger=t.rt;
  g.thumb_lx=t.lx;g.thumb_ly=t.ly;g.thumb_rx=t.rx;g.thumb_ry=t.ry;
}
}
