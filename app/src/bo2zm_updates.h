#pragma once

#include <cstdint>
#include <string_view>

#include <rex/ppc.h>

namespace bo2zm::updates {


void Command(std::string_view argument);


void EveryFrame(PPCContext& ctx, uint8_t* base);


void Stop();

}
