#pragma once

#include <cstdint>

#include <rex/ppc.h>

namespace bo2zm::native_skin {


void Skin1(PPCContext& ctx, uint8_t* base);

void Skin2(PPCContext& ctx, uint8_t* base);

void Skin3(PPCContext& ctx, uint8_t* base);


bool RecordGpu(int kernel, PPCContext& ctx, uint8_t* base, bool cpu_written);


void Report();

}
