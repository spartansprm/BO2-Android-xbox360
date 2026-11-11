#pragma once

#include <cstdint>
#include <string_view>

#include <rex/ppc.h>

namespace bo2zm::cheat_sync {


void EveryFrame(PPCContext& ctx, uint8_t* base);


bool Forward(int controller, std::string_view name, std::string_view argument, bool pack_a_punch);


void TraceFrame(bool map_loading, bool local_actor);

}
