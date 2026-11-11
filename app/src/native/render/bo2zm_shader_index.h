#pragma once

#include "bo2zm_native_shaders.h"
#include "bo2zm_shader_library.h"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <span>
#include <string>
#include <vector>

namespace bo2zm::native {


inline constexpr uint32_t kFetchKeeps[3] = {0x0007FFFF, 0x80000FFF, 0x80000000};
inline constexpr uint32_t kFetchNothing[3] = {0, 0, 0};


using AnalyzeMicrocode = void (*)(ShaderInput& e);
const char* ReadContainer(const bo2zm::shaderlib::Shader& shader, ShaderInput& e, AnalyzeMicrocode analyze = nullptr);


uint32_t CountKills(const std::vector<uint32_t>& spirv);

bool HasMarkShadowMin(const std::vector<uint32_t>& spirv);

struct IndexEntry {
  uint64_t container_hash = 0;
  uint64_t container_hash_low = 0;
  uint64_t container_hash_high = 0;
  uint64_t container_offset = 0;
  uint32_t container_size = 0;
  uint32_t spirv_words = 0;
  uint64_t spirv_hash = 0;
  uint64_t microcode_hash = 0;
  uint32_t microcode_words = 0;
  uint32_t fetches_start = 0;
  uint16_t fetches_count = 0;
  uint8_t flags = 0;
  uint8_t kills = 0;
};
constexpr uint8_t kIndexVertices = 1, kIndexFetchFull = 2, kIndexShadowMin = 4, kIndexReadable = 8;


struct IndexStats {
  uint32_t vertex = 0, pixel = 0, ignored = 0, repeated = 0, repeated_different = 0, with_kill = 0, no_kill = 0,
           with_map_shadows = 0, with_min = 0;
};

struct ShaderIndex {
  std::vector<IndexEntry> entries;
  std::vector<uint16_t> fetches;
  IndexStats stats;
};

struct IndexEvents {

  std::function<void(uint32_t number, const char* reason)> ignored;

  std::function<void(uint64_t done, uint64_t total)> progress;
};


void BuildShaderIndex(bo2zm::shaderlib::ShaderLibrary& library, const std::filesystem::path& pack_file,
                      const bo2zm::shaderlib::PackInfo& pack, ShaderIndex& index, const IndexEvents& events = {});


std::vector<uint8_t> EncodeShaderIndex(const ShaderIndex& index, const bo2zm::shaderlib::PackInfo& pack);


const char* DecodeShaderIndex(std::span<const uint8_t> bytes, const bo2zm::shaderlib::PackInfo& pack,
                              ShaderIndex& index);


bool SaveShaderIndex(const std::filesystem::path& file, std::span<const uint8_t> bytes, std::string& error);


std::filesystem::path ShaderIndexPath(const std::filesystem::path& pack_file);

}
