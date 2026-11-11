#pragma once

#include <cstdint>

#include <rex/ppc.h>

namespace bo2zm::native_limiter {


void Limit(PPCContext& ctx, uint8_t* base);


bool OnInThisArm();

}
