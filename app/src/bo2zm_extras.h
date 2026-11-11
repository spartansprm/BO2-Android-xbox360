#pragma once

#include <cstdint>
#include <string_view>

#include <rex/ppc.h>

namespace bo2zm::extras {


void EveryFrame(PPCContext& ctx, uint8_t* base);


void PaceFrame();


bool RunCommand(std::string_view name, std::string_view argument);


bool RunPlayerCommand(int local, std::string_view name, std::string_view argument);

}
