#pragma once

#include <cstdint>

namespace bo2zm::pad {


constexpr uint16_t kUp = 0x0001;
constexpr uint16_t kDown = 0x0002;
constexpr uint16_t kLeft = 0x0004;
constexpr uint16_t kRight = 0x0008;
constexpr uint16_t kBack = 0x0020;

struct Pad {
  uint16_t buttons;
  uint16_t before;
  bool aim;
  uint32_t user = 0;

  bool Pressed(uint16_t button) const { return (buttons & button) && !(before & button); }
};

}
