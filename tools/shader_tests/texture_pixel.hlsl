// SPDX-License-Identifier: GPL-3.0-only
#include "shader_common.h"
float4 main(float2 uv : TEXCOORD0) : SV_Target0 {

    uint image = BO2ZM_READ_SHARED_UINT(0);
    uint samplerIndex = BO2ZM_READ_SHARED_UINT(192);
    float2 invSize = BO2ZM_READ_SHARED_FLOAT2(360);
    float scale = BO2ZM_READ_SHARED_FLOAT(488);
    float2 coordScale = BO2ZM_READ_SHARED_FLOAT2(560);
    return tfetch2D(image, samplerIndex, uv * coordScale, float2(0, 0), invSize) *
        BO2ZM_READ_CONSTANT(Pixel, 0) * scale;
}
