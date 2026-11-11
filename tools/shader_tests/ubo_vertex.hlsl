// SPDX-License-Identifier: GPL-3.0-only

#include "shader_common.h"


#define Position0 BO2ZM_READ_CONSTANT(Vertex, 0)
#define Position(INDEX) select((INDEX) < 256, BO2ZM_READ_CONSTANT(Vertex, min(INDEX, 255)), 0.0)
float4 main(uint index : SV_VertexID) : SV_Position {
    return index == 0 ? Position0 : Position(index);
}
