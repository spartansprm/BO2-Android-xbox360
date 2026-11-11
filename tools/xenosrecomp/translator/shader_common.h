#ifndef SHADER_COMMON_H_INCLUDED
#define SHADER_COMMON_H_INCLUDED

#define SPEC_CONSTANT_R11G11B10_NORMAL  (1 << 0)
#define SPEC_CONSTANT_ALPHA_TEST        (1 << 1)


#define SPEC_CONSTANT_CONSTANTS_UBO    (1 << 8)


#define SPEC_CONSTANT_INV_TEX_SIZE    (1 << 9)


#define SPEC_CONSTANT_PCF_CHEAP        (1 << 15)


#define SPEC_CONSTANT_NO_BLUR    (1 << 19)


#define SPEC_CONSTANT_SHADOW_MIN     (1 << 23)


#define BO2ZM_SHADOW_MIN_MARKER       0x5E3B1A84u
#define SPEC_CONSTANT_ALPHA_FUNC_SHIFT  16
#define SPEC_CONSTANT_ALPHA_FUNC_MASK   (7 << 16)

#ifdef UNLEASHED_RECOMP
    #define SPEC_CONSTANT_BICUBIC_GI_FILTER (1 << 2)
    #define SPEC_CONSTANT_ALPHA_TO_COVERAGE (1 << 3)
    #define SPEC_CONSTANT_REVERSE_Z         (1 << 4)
#endif

#if !defined(__cplusplus) || defined(__INTELLISENSE__)

#define FLT_MIN asfloat(0xff7fffff)
#define FLT_MAX asfloat(0x7f7fffff)

#ifdef __spirv__

#if !defined(BO2ZM_UBO_ONLY) || !BO2ZM_UBO_ONLY
struct PushConstants
{
    uint64_t VertexShaderConstants;
    uint64_t PixelShaderConstants;
    uint64_t SharedConstants;
};

[[vk::push_constant]] ConstantBuffer<PushConstants> g_PushConstants;
#endif


struct Bo2zmBlockVs { float4 v[256]; };


struct Bo2zmBlockPs { float4 v[256]; };


struct Bo2zmBlockShared { float4 v[43]; };
[[vk::binding(0, 4)]] ConstantBuffer<Bo2zmBlockVs> g_UboVertex;
[[vk::binding(1, 4)]] ConstantBuffer<Bo2zmBlockPs> g_UboPixel;
[[vk::binding(2, 4)]] ConstantBuffer<Bo2zmBlockShared> g_UboShared;
#define BO2ZM_SHARED_UINT(B)  asuint(g_UboShared.v[(B) / 16][((B) % 16) / 4])
#define BO2ZM_SHARED_FLOAT(B) g_UboShared.v[(B) / 16][((B) % 16) / 4]


#if defined(BO2ZM_UBO_ONLY) && BO2ZM_UBO_ONLY
#define BO2ZM_UBO 1
#define BO2ZM_READ_CONSTANT(BANK, INDEX) (g_Ubo##BANK.v[(INDEX)])
#define BO2ZM_READ_SHARED_UINT(B) BO2ZM_SHARED_UINT(B)
#define BO2ZM_READ_SHARED_FLOAT(B) BO2ZM_SHARED_FLOAT(B)
#define BO2ZM_READ_SHARED_FLOAT2(B) float2(BO2ZM_SHARED_FLOAT(B), BO2ZM_SHARED_FLOAT((B) + 4))
#else
#define BO2ZM_UBO ((g_SpecConstants & SPEC_CONSTANT_CONSTANTS_UBO) != 0)
#define BO2ZM_READ_CONSTANT(BANK, INDEX) \
    (BO2ZM_UBO ? g_Ubo##BANK.v[(INDEX)] : vk::RawBufferLoad<float4>(g_PushConstants.BANK##ShaderConstants + (INDEX) * 16, 0x10))
#define BO2ZM_READ_SHARED_UINT(B) (BO2ZM_UBO ? BO2ZM_SHARED_UINT(B) : vk::RawBufferLoad<uint>(g_PushConstants.SharedConstants + (B)))
#define BO2ZM_READ_SHARED_FLOAT(B) (BO2ZM_UBO ? BO2ZM_SHARED_FLOAT(B) : vk::RawBufferLoad<float>(g_PushConstants.SharedConstants + (B)))
#define BO2ZM_READ_SHARED_FLOAT2(B) \
    (BO2ZM_UBO ? float2(BO2ZM_SHARED_FLOAT(B), BO2ZM_SHARED_FLOAT((B) + 4)) : vk::RawBufferLoad<float2>(g_PushConstants.SharedConstants + (B)))
#endif

#define g_Booleans                 BO2ZM_READ_SHARED_UINT(256)
#define g_SwappedTexcoords         BO2ZM_READ_SHARED_UINT(260)
#define g_HalfPixelOffset          BO2ZM_READ_SHARED_FLOAT2(264)
#define g_AlphaThreshold           BO2ZM_READ_SHARED_FLOAT(272)


#define g_AlphaFunction            BO2ZM_READ_SHARED_UINT(276)


#define g_NdcScale                 BO2ZM_READ_SHARED_FLOAT2(280)
#define g_NdcOffset                BO2ZM_READ_SHARED_FLOAT2(288)


#define g_InputRemap(LOC)          BO2ZM_READ_SHARED_UINT(296 + (LOC) * 4)


#define g_PixelPositionScale       BO2ZM_READ_SHARED_FLOAT2(552)

[[vk::constant_id(0)]] const uint g_SpecConstants = 0;

#define g_SpecConstants() g_SpecConstants

#else

#define DEFINE_SHARED_CONSTANTS() \
    uint g_Booleans : packoffset(c16.x); \
    uint g_SwappedTexcoords : packoffset(c16.y); \
    float2 g_HalfPixelOffset : packoffset(c16.z); \
    float g_AlphaThreshold : packoffset(c17.x); \
    uint g_AlphaFunction : packoffset(c17.y); \
    float2 g_NdcScale : packoffset(c17.z); \
    float2 g_NdcOffset : packoffset(c18.x);

uint g_SpecConstants();

#define g_InputRemap(LOC) 0xFFF

#endif

Texture2D<float4> g_Texture2DDescriptorHeap[] : register(t0, space0);
Texture3D<float4> g_Texture3DDescriptorHeap[] : register(t0, space1);
TextureCube<float4> g_TextureCubeDescriptorHeap[] : register(t0, space2);
SamplerState g_SamplerDescriptorHeap[] : register(s0, space3);

uint2 getTexture2DDimensions(Texture2D<float4> texture)
{
    uint2 dimensions;
    texture.GetDimensions(dimensions.x, dimensions.y);
    return dimensions;
}


float2 normalize2D(uint resourceDescriptorIndex, float2 texCoord, float2 invSize)
{
    [branch] if (g_SpecConstants() & SPEC_CONSTANT_INV_TEX_SIZE)
        return texCoord * invSize;
    return texCoord / getTexture2DDimensions(g_Texture2DDescriptorHeap[resourceDescriptorIndex]);
}

float4 tfetch2D(uint resourceDescriptorIndex, uint samplerDescriptorIndex, float2 texCoord, float2 offset, float2 invSize)
{
    Texture2D<float4> texture = g_Texture2DDescriptorHeap[resourceDescriptorIndex];


    float2 texelOffset;
    [branch] if (g_SpecConstants() & SPEC_CONSTANT_INV_TEX_SIZE)
        texelOffset = offset * invSize;
    else
        texelOffset = offset / getTexture2DDimensions(texture);
    return texture.Sample(g_SamplerDescriptorHeap[samplerDescriptorIndex], texCoord + texelOffset);
}

float2 getWeights2D(uint resourceDescriptorIndex, uint samplerDescriptorIndex, float2 texCoord, float2 offset)
{
    Texture2D<float4> texture = g_Texture2DDescriptorHeap[resourceDescriptorIndex];
    return select(isnan(texCoord), 0.0, frac(texCoord * getTexture2DDimensions(texture) + offset - 0.5));
}


float2 getWeights2D(uint resourceDescriptorIndex, uint samplerDescriptorIndex, float2 texCoord, float2 offset, float2 invSize)
{
    Texture2D<float4> texture = g_Texture2DDescriptorHeap[resourceDescriptorIndex];
    float2 texSize;
    [branch] if (g_SpecConstants() & SPEC_CONSTANT_INV_TEX_SIZE)
        texSize = 1.0 / invSize;
    else
        texSize = getTexture2DDimensions(texture);
    return select(isnan(texCoord), 0.0, frac(texCoord * texSize + offset - 0.5));
}

float w0(float a)
{
    return (1.0f / 6.0f) * (a * (a * (-a + 3.0f) - 3.0f) + 1.0f);
}

float w1(float a)
{
    return (1.0f / 6.0f) * (a * a * (3.0f * a - 6.0f) + 4.0f);
}

float w2(float a)
{
    return (1.0f / 6.0f) * (a * (a * (-3.0f * a + 3.0f) + 3.0f) + 1.0f);
}

float w3(float a)
{
    return (1.0f / 6.0f) * (a * a * a);
}

float g0(float a)
{
    return w0(a) + w1(a);
}

float g1(float a)
{
    return w2(a) + w3(a);
}

float h0(float a)
{
    return -1.0f + w1(a) / (w0(a) + w1(a)) + 0.5f;
}

float h1(float a)
{
    return 1.0f + w3(a) / (w2(a) + w3(a)) + 0.5f;
}


float4 tfetch2DShadow(uint resourceDescriptorIndex, uint samplerDescriptorIndex, float2 texCoord, float2 offset, float2 invSize)
{
    [branch] if (g_SpecConstants() & SPEC_CONSTANT_PCF_CHEAP)
        return tfetch2D(resourceDescriptorIndex, samplerDescriptorIndex, texCoord, float2(0.0, 0.0), invSize);
    return tfetch2D(resourceDescriptorIndex, samplerDescriptorIndex, texCoord, offset, invSize);
}


float4 tfetch2DShadowMin(uint resourceDescriptorIndex, uint pairDescriptorIndex, uint samplerDescriptorIndex, float2 texCoord, float2 offset, float2 invSize)
{
    [branch] if (g_SpecConstants() == BO2ZM_SHADOW_MIN_MARKER)
        return float4(0.0, 0.0, 0.0, 0.0);
    float4 minSample = tfetch2DShadow(resourceDescriptorIndex, samplerDescriptorIndex, texCoord, offset, invSize);
    [branch] if (g_SpecConstants() & SPEC_CONSTANT_SHADOW_MIN)
        minSample = min(minSample, tfetch2DShadow(pairDescriptorIndex, samplerDescriptorIndex, texCoord, offset, invSize));
    return minSample;
}

float4 tfetch2DBicubic(uint resourceDescriptorIndex, uint samplerDescriptorIndex, float2 texCoord, float2 offset)
{
    Texture2D<float4> texture = g_Texture2DDescriptorHeap[resourceDescriptorIndex];
    SamplerState samplerState = g_SamplerDescriptorHeap[samplerDescriptorIndex];
    uint2 dimensions = getTexture2DDimensions(texture);

    float x = texCoord.x * dimensions.x + offset.x;
    float y = texCoord.y * dimensions.y + offset.y;

    x -= 0.5f;
    y -= 0.5f;
    float px = floor(x);
    float py = floor(y);
    float fx = x - px;
    float fy = y - py;

    float g0x = g0(fx);
    float g1x = g1(fx);
    float h0x = h0(fx);
    float h1x = h1(fx);
    float h0y = h0(fy);
    float h1y = h1(fy);

    float4 r =
        g0(fy) * (g0x * texture.Sample(samplerState, float2(px + h0x, py + h0y) / float2(dimensions)) +
            g1x * texture.Sample(samplerState, float2(px + h1x, py + h0y) / float2(dimensions))) +
        g1(fy) * (g0x * texture.Sample(samplerState, float2(px + h0x, py + h1y) / float2(dimensions)) +
            g1x * texture.Sample(samplerState, float2(px + h1x, py + h1y) / float2(dimensions)));

    return r;
}

float4 tfetch3D(uint resourceDescriptorIndex, uint samplerDescriptorIndex, float3 texCoord)
{
    return g_Texture3DDescriptorHeap[resourceDescriptorIndex].Sample(g_SamplerDescriptorHeap[samplerDescriptorIndex], texCoord);
}


float4 tfetch2DLevel(uint resourceDescriptorIndex, uint samplerDescriptorIndex, float2 texCoord, float2 offset, float2 invSize, float lod)
{
    Texture2D<float4> texture = g_Texture2DDescriptorHeap[resourceDescriptorIndex];
    float2 texelOffset;
    [branch] if (g_SpecConstants() & SPEC_CONSTANT_INV_TEX_SIZE)
        texelOffset = offset * invSize;
    else
        texelOffset = offset / getTexture2DDimensions(texture);
    return texture.SampleLevel(g_SamplerDescriptorHeap[samplerDescriptorIndex], texCoord + texelOffset, lod);
}

float4 tfetch3DLevel(uint resourceDescriptorIndex, uint samplerDescriptorIndex, float3 texCoord, float lod)
{
    return g_Texture3DDescriptorHeap[resourceDescriptorIndex].SampleLevel(g_SamplerDescriptorHeap[samplerDescriptorIndex], texCoord, lod);
}

struct CubeMapData
{
    float3 cubeMapDirections[2];
    uint cubeMapIndex;
};

float4 tfetchCube(uint resourceDescriptorIndex, uint samplerDescriptorIndex, float3 texCoord, inout CubeMapData cubeMapData)
{
    return g_TextureCubeDescriptorHeap[resourceDescriptorIndex].Sample(g_SamplerDescriptorHeap[samplerDescriptorIndex], cubeMapData.cubeMapDirections[texCoord.z]);
}


float4 tfetchCubeLevel(uint resourceDescriptorIndex, uint samplerDescriptorIndex, float3 texCoord, inout CubeMapData cubeMapData, float lod)
{
    return g_TextureCubeDescriptorHeap[resourceDescriptorIndex].SampleLevel(g_SamplerDescriptorHeap[samplerDescriptorIndex], cubeMapData.cubeMapDirections[texCoord.z], lod);
}

float4 tfetchR11G11B10(uint4 value)
{
    if (g_SpecConstants() & SPEC_CONSTANT_R11G11B10_NORMAL)
    {
        return float4(
            (value.x & 0x00000400 ? -1.0 : 0.0) + ((value.x & 0x3FF) / 1024.0),
            (value.x & 0x00200000 ? -1.0 : 0.0) + (((value.x >> 11) & 0x3FF) / 1024.0),
            (value.x & 0x80000000 ? -1.0 : 0.0) + (((value.x >> 22) & 0x1FF) / 512.0),
            0.0);
    }
    else
    {
        return asfloat(value);
    }
}

float4 tfetchTexcoord(uint swappedTexcoords, float4 value, uint semanticIndex)
{


    bool swapped = semanticIndex < 32u && ((swappedTexcoords >> (semanticIndex & 31u)) & 1u) != 0u;
    return swapped ? value.yxwz : value;
}


float4 remapInput(float4 value, uint code)
{
    if (code == 0xFFF)
        return value;

    float4 result = value;
    for (uint i = 0; i < 4; i++)
    {
        uint source = (code >> (i * 3)) & 7;
        if (source < 4)
            result[i] = value[source];
        else if (source == 4)
            result[i] = 0.0;
        else if (source == 5)
            result[i] = 1.0;
    }
    return result;
}


float alphaTestValue(float alpha)
{
    bool pass = true;
    switch ((g_SpecConstants() & SPEC_CONSTANT_ALPHA_FUNC_MASK) >> SPEC_CONSTANT_ALPHA_FUNC_SHIFT)
    {
    case 0: pass = false; break;
    case 1: pass = alpha < g_AlphaThreshold; break;
    case 2: pass = alpha == g_AlphaThreshold; break;
    case 3: pass = alpha <= g_AlphaThreshold; break;
    case 4: pass = alpha > g_AlphaThreshold; break;
    case 5: pass = alpha != g_AlphaThreshold; break;
    case 6: pass = alpha >= g_AlphaThreshold; break;
    default: break;
    }
    return pass ? 1.0 : -1.0;
}

float4 cube(float4 value, inout CubeMapData cubeMapData)
{
    uint index = cubeMapData.cubeMapIndex;
    cubeMapData.cubeMapDirections[index] = value.xyz;
    ++cubeMapData.cubeMapIndex;

    return float4(0.0, 0.0, 0.0, index);
}

float4 dst(float4 src0, float4 src1)
{
    float4 dest;
    dest.x = 1.0;
    dest.y = src0.y * src1.y;
    dest.z = src0.z;
    dest.w = src1.w;
    return dest;
}

float4 max4(float4 src0)
{
    return max(max(src0.x, src0.y), max(src0.z, src0.w));
}

float2 getPixelCoord(uint resourceDescriptorIndex, float2 texCoord)
{
    return getTexture2DDimensions(g_Texture2DDescriptorHeap[resourceDescriptorIndex]) * texCoord;
}

float computeMipLevel(float2 pixelCoord)
{
    float2 dx = ddx(pixelCoord);
    float2 dy = ddy(pixelCoord);
    float deltaMaxSqr = max(dot(dx, dx), dot(dy, dy));
    return max(0.0, 0.5 * log2(deltaMaxSqr));
}

#endif

#endif
