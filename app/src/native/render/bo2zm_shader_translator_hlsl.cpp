#include "bo2zm_xenosrecomp_prelude.h"

#include "shader_recompiler.h"

#include "bo2zm_shader_common_text.h"
#include "bo2zm_shader_translator_hlsl.h"

#include <cstring>
#include <string_view>
#include <exception>
#include <vector>

namespace bo2zm::native::translate {

namespace {


constexpr std::string_view kResampleFetch = "tfetch2D(codeTexture0_Texture2DDescriptorIndex";
constexpr std::string_view kResampleMarkers[] = {
    "float4 r0 = iTexCoord0;", "float4 r1 = iTexCoord1;", "frac(r1.z)", "postFxControl0", "postFxControl1",
    "(r0.xy * codeTexture0_FetchScaleCoord)"};
constexpr std::string_view kResampleBodyStart = "CubeMapData cubeMapData = (CubeMapData)0;\n";


constexpr std::string_view kResampleScaled = R"(
	// BO2ZM 0.51.7: scaled scene, no 880-column resample (bo2zm_shader_translator_hlsl.h, PatchSceneResample).
	[branch] if (float(getTexture2DDimensions(g_Texture2DDescriptorHeap[codeTexture0_Texture2DDescriptorIndex]).x) *
	             codeTexture0_InvSize.x > 1.01)
	{
		uint2 bo2zmSize = getTexture2DDimensions(g_Texture2DDescriptorHeap[codeTexture0_Texture2DDescriptorIndex]);
		// TEXCOORD0.xy is the centre tap as the guest's point fetch reads it: half a guest texel further is the
		// position the four weights interpolate around.
		float2 bo2zmUv = r0.xy * codeTexture0_FetchScaleCoord + float2(0.5 * codeTexture0_InvSize.x, 0.0);
		float2 bo2zmPos = bo2zmUv * float2(bo2zmSize) - 0.5;
		int2 bo2zmMax = int2(bo2zmSize) - 1;
		float4 bo2zmColor;
		[branch] if (abs(abs(ddx(bo2zmPos.x)) - 1.0) < 0.05)
		{
			int2 bo2zmAt = clamp(int2(floor(bo2zmPos + 0.5)), int2(0, 0), bo2zmMax);
			bo2zmColor = g_Texture2DDescriptorHeap[codeTexture0_Texture2DDescriptorIndex].Load(int3(bo2zmAt, 0));
		}
		else
		{
			float2 bo2zmBase = floor(bo2zmPos);
			float2 bo2zmF = bo2zmPos - bo2zmBase;
			int2 bo2zmA = clamp(int2(bo2zmBase), int2(0, 0), bo2zmMax);
			int2 bo2zmB = clamp(int2(bo2zmBase) + 1, int2(0, 0), bo2zmMax);
			float4 bo2zm00 = g_Texture2DDescriptorHeap[codeTexture0_Texture2DDescriptorIndex].Load(int3(bo2zmA.x, bo2zmA.y, 0));
			float4 bo2zm10 = g_Texture2DDescriptorHeap[codeTexture0_Texture2DDescriptorIndex].Load(int3(bo2zmB.x, bo2zmA.y, 0));
			float4 bo2zm01 = g_Texture2DDescriptorHeap[codeTexture0_Texture2DDescriptorIndex].Load(int3(bo2zmA.x, bo2zmB.y, 0));
			float4 bo2zm11 = g_Texture2DDescriptorHeap[codeTexture0_Texture2DDescriptorIndex].Load(int3(bo2zmB.x, bo2zmB.y, 0));
			bo2zmColor = lerp(lerp(bo2zm00, bo2zm10, bo2zmF.x), lerp(bo2zm01, bo2zm11, bo2zmF.x), bo2zmF.y);
		}
		oC0.xyz = (bo2zmColor * codeTexture0_FetchScale).xyz;
		oC0.w = 1.0;
		return;
	}
)";

size_t Count(std::string_view text, std::string_view what) {
  size_t n = 0;
  for (size_t at = text.find(what); at != std::string_view::npos; at = text.find(what, at + what.size())) {
    ++n;
  }
  return n;
}

}

bool IsSceneResample(std::string_view hlsl) {
  const size_t body = hlsl.find(kResampleBodyStart);
  if (body == std::string_view::npos) {
    return false;
  }
  for (std::string_view marker : kResampleMarkers) {
    if (hlsl.find(marker) == std::string_view::npos) {
      return false;
    }
  }
  return Count(hlsl.substr(body), kResampleFetch) == 4;
}

bool PatchSceneResample(std::string& hlsl) {
  if (!IsSceneResample(hlsl)) {
    return false;
  }
  hlsl.insert(hlsl.find(kResampleBodyStart) + kResampleBodyStart.size(), kResampleScaled);
  return true;
}

std::string_view ShaderCommonText() {
  return std::string_view(kShaderCommonText, sizeof(kShaderCommonText) - 1);
}

bool ContainerToHlsl(std::span<const uint8_t> container, std::string& hlsl, bool& pixel, std::string& error) {
  hlsl.clear();
  if (container.size() < 36 || container.size() > 65536) {
    error = "container size out of range";
    return false;
  }
  uint32_t flags = 0;
  std::memcpy(&flags, container.data(), 4);
  flags = __builtin_bswap32(flags);
  if ((flags & 0xFFFFFF00u) != 0x102A1100u) {
    error = "not a 0x102A11xx shader container";
    return false;
  }


  const auto word = [&](size_t at) {
    uint32_t v = 0;
    std::memcpy(&v, container.data() + at, 4);
    return __builtin_bswap32(v);
  };
  const uint32_t virtual_size = word(4), physical_size = word(8);
  const uint32_t constants_at = word(16), definitions_at = word(20), shader_at = word(24);
  if (uint64_t(virtual_size) + physical_size != container.size() || virtual_size < 36 || !constants_at ||
      constants_at + 32u > virtual_size || shader_at + 24u > virtual_size ||
      (definitions_at && definitions_at + 20u > virtual_size)) {
    error = "container offsets out of range";
    return false;
  }
  std::vector<uint8_t> padded(container.size() + 65536 + 16, 0);
  std::memcpy(padded.data(), container.data(), container.size());
  try {
    ShaderRecompiler recompiler;
    recompiler.recompile(padded.data(), ShaderCommonText());
    if (recompiler.out.empty()) {
      error = "XenosRecomp produced no HLSL";
      return false;
    }
    pixel = recompiler.isPixelShader;
    hlsl = std::move(recompiler.out);
    if (pixel) {
      PatchSceneResample(hlsl);
    }
    return true;
  } catch (const std::exception& e) {
    error = std::string("XenosRecomp: ") + e.what();
  } catch (...) {
    error = "XenosRecomp: unknown exception";
  }
  return false;
}

}
