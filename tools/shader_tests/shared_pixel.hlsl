// SPDX-License-Identifier: GPL-3.0-only
#include "shader_common.h"
float4 main() : SV_Target0 {

    float2 crossing = BO2ZM_READ_SHARED_FLOAT2(12);
    float4 value = BO2ZM_READ_CONSTANT(Pixel, 0);
    value.xy += crossing + g_HalfPixelOffset + g_NdcScale + g_NdcOffset;
    value.z += g_AlphaThreshold + float(g_Booleans + g_SwappedTexcoords);
    value.w += float(g_AlphaFunction + g_InputRemap(0));
    return value;
}
