#pragma once

#include <cstdint>
#include <string_view>

#include <rex/ppc.h>

namespace bo2zm::name {


void ApplySaved();


void EveryFrame(PPCContext& ctx, uint8_t* base);


bool RunCommand(std::string_view name, std::string_view argument);

}
