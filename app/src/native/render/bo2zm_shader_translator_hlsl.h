#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <string_view>

namespace bo2zm::native::translate {


std::string_view ShaderCommonText();


bool ContainerToHlsl(std::span<const uint8_t> container, std::string& hlsl, bool& pixel, std::string& error);


bool IsSceneResample(std::string_view hlsl);
bool PatchSceneResample(std::string& hlsl);


inline constexpr uint64_t kSceneResampleHash = 0x64054E6BEC193811ull;


inline constexpr uint64_t kSceneResampleHashLow = 0x64054E6BEC193811ull;
inline constexpr uint64_t kSceneResampleHashHigh = 0xAF391491EA8C9E94ull;

}
