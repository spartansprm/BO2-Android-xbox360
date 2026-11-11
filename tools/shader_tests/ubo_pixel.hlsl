// SPDX-License-Identifier: GPL-3.0-only
#include "shader_common.h"
float4 main() : SV_Target0 {
    return BO2ZM_READ_CONSTANT(Pixel, 0);
}
