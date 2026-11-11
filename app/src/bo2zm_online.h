#pragma once

#include <string_view>

#include <rex/ppc.h>

namespace bo2zm::online {


bool RunCommand(std::string_view name, std::string_view argument);


void EveryFrame(PPCContext& ctx, uint8_t* base);


void Stop();

}
