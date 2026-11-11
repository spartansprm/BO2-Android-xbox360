// SPDX-License-Identifier: GPL-3.0-only

float4 main([[vk::location(0)]] float4 varying[22] : TEXCOORD0) : SV_Target0 {
    float4 sum=0.0;
    for(uint i=0;i<22;++i) sum+=varying[i];
    return sum;
}
