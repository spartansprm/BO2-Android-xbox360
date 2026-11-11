// SPDX-License-Identifier: GPL-3.0-only

#include "shader_common.h"
float4 main(float4 value : TEXCOORD0, uint semanticIndex : TEXCOORD1) : SV_Position
{
    return tfetchTexcoord(g_SwappedTexcoords, value, semanticIndex) + g_UboVertex.v[0];
}
