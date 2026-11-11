// SPDX-License-Identifier: GPL-3.0-only

#include "shader_common.h"
struct Output { float4 position : SV_Position; float2 uv : TEXCOORD0; };
Output main(uint index : SV_VertexID) {
    Output result;
    result.position = BO2ZM_READ_CONSTANT(Vertex, index);
    result.uv = result.position.xy * 0.5 + 0.5;
    return result;
}
