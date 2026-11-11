#pragma once

#include <cstdint>
#include <string_view>

#include <rex/runtime.h>

#include "bo2zm_pad.h"

namespace bo2zm::controllers {


void WrapInput(rex::RuntimeConfig& config);


struct PadEdit {
  uint16_t hide = 0;
  uint16_t add = 0;
};

PadEdit OnPad(const bo2zm::pad::Pad& pad);


void EveryFrame();


bool RunCommand(std::string_view name, std::string_view argument);

}
