#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace bo2zm::shaderlib {
struct Shader;
}

namespace bo2zm::native {


struct VertexElement {
  uint16_t instruction = 0;
  uint8_t use = 0;
  uint8_t index_use = 0;
};


struct SamplerShader {
  uint16_t reg_index = 0;
  uint16_t type = 0;


  bool map_shadows = false;
};

struct ShaderInput {
  const bo2zm::shaderlib::Shader* shader = nullptr;
  bool vertices = false;
  uint32_t number = 0;
  std::vector<uint32_t> microcode;
  uint64_t hash = 0;


  bool fetch_full = false;
  std::vector<uint32_t> microcode_no_fetch;
  uint64_t hash_no_fetch = 0;
  std::vector<VertexElement> elements;
  std::vector<SamplerShader> samplers;
  uint32_t outputs = 0;


  uint32_t kills = 0;
  bool discards = false;


  bool shadow_min = false;

  uint32_t constants_bytes = 256 * 16;


  std::vector<std::pair<uint16_t, uint16_t>> constants_runs;
  bool constants_runs_valid = false;
  uint32_t constants_runs_bytes = 0;
};

struct NativeShaderStats {
  uint64_t loads = 0;
  uint64_t different = 0;
  uint64_t identified = 0;
  uint64_t no_identify = 0;
  uint64_t ambiguous = 0;
};

class NativeShaders {
 public:
  NativeShaders();
  ~NativeShaders();


  using IndexProgress = std::function<void(uint64_t done, uint64_t total)>;


  bool Load(const std::filesystem::path& file, const IndexProgress& progress = {});
  bool loaded() const;


  uint64_t Identity() const;


  const ShaderInput* IdentifyContainer(std::span<const uint8_t> container) const;


  int32_t NumberOfContainer(std::span<const uint8_t> container) const;


  const ShaderInput* PerNumber(uint32_t number) const;


  const ShaderInput* Identify(bool vertices, std::span<const uint32_t> microcode);

  NativeShaderStats Stats() const;


  bool InLibrary(int32_t number) const;

 private:
  struct Data;
  void CheckIndex(const std::filesystem::path& file) const;
  std::unique_ptr<Data> data_;
};


std::string RuntimeShadersJson();


const char* NameUse(uint8_t use);


bool FetchCoherent(const ShaderInput& vs, std::span<const uint32_t> patched);

}
