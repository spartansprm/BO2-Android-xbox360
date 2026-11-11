// SPDX-License-Identifier: GPL-3.0-only

struct Output {
    float4 position : SV_Position;
    [[vk::location(0)]] float4 varying[22] : TEXCOORD0;
};
Output main(uint id : SV_VertexID) {
    Output result;
    result.position=float4(float(id & 1u),float(id >> 1u),0.0,1.0);
    for(uint i=0;i<22;++i) result.varying[i]=float4(float(id),float(i),0.0,1.0);
    return result;
}
