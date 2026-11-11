#include "bo2zm_shader_translator.h"

#include "bo2zm_native_shader_profile.h"
#include "bo2zm_shader_translator_hlsl.h"

#include <glslang/Public/ResourceLimits.h>
#include <glslang/Public/ShaderLang.h>

#if __has_include(<glslang/SPIRV/GlslangToSpv.h>)
#include <glslang/SPIRV/GlslangToSpv.h>
#else
#include <SPIRV/GlslangToSpv.h>
#endif
#include <spirv-tools/libspirv.hpp>
#include <spirv-tools/optimizer.hpp>

#include <chrono>
#include <cstring>
#include <mutex>
#include <string_view>

#define XXH_INLINE_ALL
#include <xxhash.h>

namespace bo2zm::native::translate {
namespace {


constexpr uint32_t kVersion = 1;


constexpr std::string_view kPrelude =
    "#define BO2ZM_UBO_ONLY 1\n"
    "#define __spirv__ 1\n"
    "#define bo2_select(c, a, b) ((c) ? (a) : (b))\n";
constexpr std::string_view kAnyOverloads =
    "bool bo2_any(bool v) { return v; }\n"
    "bool bo2_any(bool2 v) { return any(v); }\n"
    "bool bo2_any(bool3 v) { return any(v); }\n"
    "bool bo2_any(bool4 v) { return any(v); }\n"
    "bool bo2_any(float v) { return v != 0.0; }\n"
    "bool bo2_any(float2 v) { return any(v != 0.0); }\n"
    "bool bo2_any(float3 v) { return any(v != 0.0); }\n"
    "bool bo2_any(float4 v) { return any(v != 0.0); }\n";

bool IsIdentifierChar(char c) {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
}


size_t ReplaceWord(std::string& text, std::string_view word, std::string_view with, bool call) {
  size_t count = 0;
  std::string out;
  out.reserve(text.size() + 1024);
  size_t at = 0;
  while (true) {
    const size_t found = text.find(word, at);
    if (found == std::string::npos) {
      break;
    }
    const size_t end = found + word.size();
    const bool start_ok = found == 0 || !IsIdentifierChar(text[found - 1]);
    bool end_ok = end >= text.size() || !IsIdentifierChar(text[end]);
    if (end_ok && call) {
      size_t k = end;
      while (k < text.size() && (text[k] == ' ' || text[k] == '\t')) {
        ++k;
      }
      end_ok = k < text.size() && text[k] == '(';
    }
    out.append(text, at, found - at);
    if (start_ok && end_ok) {
      out += with;
      ++count;
    } else {
      out.append(word);
    }
    at = end;
  }
  out.append(text, at, std::string::npos);
  text = std::move(out);
  return count;
}

std::once_flag g_init;
bool g_init_ok = false;

void Initialize() {
  std::call_once(g_init, [] { g_init_ok = glslang::InitializeProcess(); });
}

}

bool ApplyCompat(std::string& hlsl, std::string& error) {

  static constexpr std::string_view kHeaps[4] = {
      "Texture2D<float4> g_Texture2DDescriptorHeap[]",
      "Texture3D<float4> g_Texture3DDescriptorHeap[]",
      "TextureCube<float4> g_TextureCubeDescriptorHeap[]",
      "SamplerState g_SamplerDescriptorHeap[]",
  };
  for (size_t i = 0; i < 4; ++i) {
    const size_t at = hlsl.find(kHeaps[i]);
    if (at == std::string::npos || hlsl.find(kHeaps[i], at + 1) != std::string::npos) {
      error = std::string("unexpected shader_common.h: no single declaration of ") + std::string(kHeaps[i]);
      return false;
    }
    std::string sized(kHeaps[i]);
    sized.insert(sized.size() - 1, std::to_string(kHeapSizes[i]));
    hlsl.replace(at, kHeaps[i].size(), sized);
  }
  ReplaceWord(hlsl, "texture", "tex_", false);
  ReplaceWord(hlsl, "select", "bo2_select", true);
  ReplaceWord(hlsl, "any", "bo2_any", true);


  const size_t first_heap = hlsl.find("Texture2D<float4> g_Texture2DDescriptorHeap[");
  hlsl.insert(first_heap, kAnyOverloads);
  hlsl.insert(0, kPrelude);
  return true;
}

uint64_t Identity() {
  Initialize();
  const glslang::Version v = glslang::GetVersion();
  std::string bytes;
  bytes += std::to_string(kVersion) + '\n';
  bytes += std::to_string(v.major) + '.' + std::to_string(v.minor) + '.' + std::to_string(v.patch) + '\n';
  bytes += kPrelude;
  bytes += kAnyOverloads;
  for (uint32_t size : kHeapSizes) {
    bytes += std::to_string(size) + ',';
  }
  bytes += ShaderCommonText();
  return XXH3_64bits(bytes.data(), bytes.size());
}

Result Translate(std::span<const uint8_t> container) {
  Result r;
  const auto t0 = std::chrono::steady_clock::now();
  std::string hlsl;
  if (!ContainerToHlsl(container, hlsl, r.pixel, r.error)) {
    return r;
  }
  const auto t1 = std::chrono::steady_clock::now();
  r.ms_hlsl = std::chrono::duration<double, std::milli>(t1 - t0).count();
  if (!ApplyCompat(hlsl, r.error)) {
    return r;
  }
  Initialize();
  if (!g_init_ok) {
    r.error = "glslang could not be initialized";
    return r;
  }
  const EShLanguage stage = r.pixel ? EShLangFragment : EShLangVertex;
  std::vector<uint32_t> spirv;
  {
    glslang::TShader shader(stage);
    const char* text = hlsl.c_str();
    const int length = int(hlsl.size());
    shader.setStringsWithLengths(&text, &length, 1);
    shader.setEntryPoint("main");
    shader.setSourceEntryPoint("main");
    shader.setEnvInput(glslang::EShSourceHlsl, stage, glslang::EShClientVulkan, 100);
    shader.setEnvClient(glslang::EShClientVulkan, glslang::EShTargetVulkan_1_2);
    shader.setEnvTarget(glslang::EShTargetSpv, glslang::EShTargetSpv_1_5);

    shader.setInvertY(!r.pixel);
    const auto messages = EShMessages(EShMsgSpvRules | EShMsgVulkanRules | EShMsgReadHlsl | EShMsgHlslOffsets);
    if (!shader.parse(GetDefaultResources(), 100, false, messages)) {
      r.error = std::string("glslang: ") + shader.getInfoLog();
      return r;
    }
    glslang::TProgram program;
    program.addShader(&shader);
    if (!program.link(messages)) {
      r.error = std::string("glslang link: ") + program.getInfoLog();
      return r;
    }
    glslang::SpvOptions options;
    options.disableOptimizer = false;
    options.validate = false;
    spv::SpvBuildLogger logger;
    glslang::GlslangToSpv(*program.getIntermediate(stage), spirv, &logger, &options);
    if (spirv.empty()) {
      r.error = "glslang: no SPIR-V (" + logger.getAllMessages() + ")";
      return r;
    }
  }

  std::string messages;
  const auto consume = [&messages](spv_message_level_t, const char*, const spv_position_t& p, const char* m) {
    messages += std::to_string(p.index) + ": " + m + '\n';
  };
  spvtools::Optimizer optimizer(SPV_ENV_VULKAN_1_2);
  optimizer.SetMessageConsumer(consume);
  optimizer.RegisterPerformancePasses();
  spvtools::ValidatorOptions validator_options;
  validator_options.SetScalarBlockLayout(true);
  std::vector<uint32_t> optimized;
  if (!optimizer.Run(spirv.data(), spirv.size(), &optimized, validator_options, true)) {
    r.error = "SPIRV-Tools optimizer: " + messages;
    return r;
  }
  spvtools::SpirvTools tools(SPV_ENV_VULKAN_1_2);
  tools.SetMessageConsumer(consume);
  if (!tools.Validate(optimized.data(), optimized.size(), validator_options)) {
    r.error = "invalid SPIR-V: " + messages;
    return r;
  }
  if (!UboOnlySpirv(optimized)) {
    r.error = "the module violates the UBO-only profile";
    return r;
  }
  r.spirv = std::move(optimized);
  r.ms_compile = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t1).count();
  return r;
}

}
