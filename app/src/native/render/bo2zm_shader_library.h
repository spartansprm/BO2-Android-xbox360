#pragma once

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <mutex>
#include <span>
#include <string>
#include <vector>

namespace bo2zm::shaderlib {


class ShaderLibrary;

struct Shader {
  std::vector<uint8_t> original;

  std::vector<uint32_t> spirv;

  uint64_t spirv_hash = 0;
  uint64_t hash = 0;
  bool vertices = false;
  uint64_t container_offset = 0;
  uint64_t spirv_offset = 0;
  uint32_t spirv_words = 0;
  const ShaderLibrary* library = nullptr;


  std::string spirv_file;
};


std::vector<uint32_t> Spirv(const Shader& shader);


struct PackInfo {
  uint64_t size = 0;
  uint32_t amount = 0;
  uint64_t body_hash = 0;
};

class ShaderLibrary {
 public:


  void Load(std::span<const uint8_t> file);


  using Inspect = std::function<void(uint32_t index, const Shader& shader)>;
  void Load(const std::filesystem::path& file, const Inspect& inspect = {});


  void Scan(const std::filesystem::path& file, const Inspect& scan);


  PackInfo Open(const std::filesystem::path& file);


  std::vector<uint8_t> Container(uint64_t offset, uint32_t size, uint64_t hash) const;

  uint64_t SpirvWords() const;
  const Shader* Find(std::span<const uint8_t> original) const;
  const std::vector<Shader>& shaders() const { return shaders_; }

 private:
  friend std::vector<uint32_t> Spirv(const Shader& shader);
  void Read(const std::filesystem::path& file, const Inspect& inspect, bool keep);
  void UseFile(const std::filesystem::path& file);

  bool ReadAt(uint64_t offset, void* to, size_t bytes) const;
  std::vector<Shader> shaders_;
  std::filesystem::path file_;
  mutable std::mutex spirv_mutex_;
  mutable std::ifstream spirv_file_;
};


std::vector<uint8_t> PackShaders(std::vector<Shader> shaders);

}
