#pragma once

#include <cstdint>

#include <rex/ppc/context.h>

namespace bo2zm::d3d_native {


bool SetTextureNative(PPCContext& ctx, uint8_t* base);

bool SetVertexShaderNative(PPCContext& ctx, uint8_t* base);
bool SetPixelShaderNative(PPCContext& ctx, uint8_t* base);

bool DrawIndexedVerticesNative(PPCContext& ctx, uint8_t* base);

}
