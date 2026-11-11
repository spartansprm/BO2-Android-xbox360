#pragma once

#include <cstdint>

#include <rex/ppc.h>

namespace bo2zm::native_lzx {


void Decode(PPCContext& ctx, uint8_t* base);

}
