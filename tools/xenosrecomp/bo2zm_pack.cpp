#include "bo2zm_shader_index.h"
#include "bo2zm_shader_library.h"

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>

#define XXH_FORCE_MEMORY_ACCESS 0
#define XXH_INLINE_ALL
#include <xxhash.h>

namespace fs = std::filesystem;

namespace {

std::vector<uint8_t> ReadAll(const fs::path& path) {
  std::ifstream f(path, std::ios::binary | std::ios::ate);
  if (!f || f.tellg() < 0 || f.tellg() > 64 * 1024 * 1024) {
    throw std::runtime_error("unreadable or too large: " + path.string());
  }
  std::vector<uint8_t> data(size_t(f.tellg()));
  f.seekg(0);
  if (!f.read(reinterpret_cast<char*>(data.data()), data.size())) {
    throw std::runtime_error("short read: " + path.string());
  }
  return data;
}

int MakeIndex(const fs::path& pack_file, const fs::path& index_file) {
  bo2zm::shaderlib::ShaderLibrary library;
  const bo2zm::shaderlib::PackInfo pack = library.Open(pack_file);
  bo2zm::native::ShaderIndex index;
  bo2zm::native::IndexEvents events;
  events.ignored = [](uint32_t number, const char* reason) {
    std::printf("  container %u ignored: %s\n", number, reason);
  };
  uint64_t shown = 0;
  events.progress = [&](uint64_t done, uint64_t total) {
    const uint64_t tenth = total ? done * 10 / total : 10;
    if (tenth > shown) {
      shown = tenth;
      std::printf("  indexing: %u %%\n", unsigned(tenth * 10));
      std::fflush(stdout);
    }
  };
  bo2zm::native::BuildShaderIndex(library, pack_file, pack, index, events);
  const std::vector<uint8_t> bytes = bo2zm::native::EncodeShaderIndex(index, pack);
  bo2zm::native::ShaderIndex again;
  if (const char* why = bo2zm::native::DecodeShaderIndex(bytes, pack, again)) {
    throw std::runtime_error(std::string("the index does not read back: ") + why);
  }
  std::string error;
  if (!bo2zm::native::SaveShaderIndex(index_file, bytes, error)) {
    throw std::runtime_error(error);
  }
  const bo2zm::native::IndexStats& s = index.stats;
  std::printf("%s: %zu shaders (%u vertex, %u pixel, %u ignored), %zu bytes\n", index_file.string().c_str(),
              index.entries.size(), s.vertex, s.pixel, s.ignored, bytes.size());
  return 0;
}

}

int main(int argc, char** argv) try {
  if ((argc == 3 || argc == 4) && std::string(argv[1]) == "--index") {
    return MakeIndex(argv[2], argc == 4 ? fs::path(argv[3]) : bo2zm::native::ShaderIndexPath(argv[2]));
  }
  if (argc == 3 && std::string(argv[1]) == "--coverage") {
    bo2zm::shaderlib::ShaderLibrary library;
    std::printf("index\tstage\twords\tmicrocode_hash\tcontainer_hash\tspirv_hash\n");
    library.Scan(argv[2], [](uint32_t index, const bo2zm::shaderlib::Shader& shader) {
      const auto& b = shader.original;
      auto be = [&](size_t p) {
        if (p > b.size() || b.size() - p < 4) throw std::runtime_error("short shader header");
        return uint32_t(b[p]) << 24 | uint32_t(b[p+1]) << 16 | uint32_t(b[p+2]) << 8 | b[p+3];
      };
      const bool modern = (be(0) & ~1u) == 0x102A1100;
      const uint32_t virt = be(4), physical = be(8), header = be(modern ? 24 : 20);
      const uint32_t start = modern ? be(header) : 0;
      const uint32_t size = modern ? be(size_t(header) + 4) : physical;
      if ((size % 4) || uint64_t(start) + size > physical || uint64_t(virt) + start + size > b.size()) {
        throw std::runtime_error("microcode outside container");
      }
      std::vector<uint32_t> words;
      for (size_t p = size_t(virt) + start; p < size_t(virt) + start + size; p += 4) words.push_back(be(p));
      std::printf("%u\t%s\t%zu\t%016llX\t%016llX\t%016llX\n", index, shader.vertices ? "vs" : "ps", words.size(),
                  static_cast<unsigned long long>(XXH3_64bits(words.data(), words.size() * 4)),
                  static_cast<unsigned long long>(shader.hash), static_cast<unsigned long long>(shader.spirv_hash));
    });
    return 0;
  }
  if (argc != 4 && !(argc == 6 && std::string(argv[4]) == "--base")) {
    throw std::runtime_error("usage: bo2zm_pack <containers> <spirv> <new output> [--base <pack>] | --coverage <pack> | "
                             "--index <pack> [<index file>]");
  }
  const fs::path containers = argv[1], compiled = argv[2], output = argv[3];
  if (fs::exists(output)) {
    throw std::runtime_error("the output already exists: " + output.string());
  }
  std::vector<fs::path> paths;
  for (const auto& e : fs::directory_iterator(containers)) {
    if (e.is_regular_file() && e.path().extension() == ".bin") {
      paths.push_back(e.path());
    }
  }
  std::sort(paths.begin(), paths.end());

  std::vector<bo2zm::shaderlib::Shader> shaders;
  bo2zm::shaderlib::ShaderLibrary base;
  if (argc == 6) {
    base.Load(fs::path(argv[5]));
    for (const auto& entry : base.shaders()) {
      auto retained = entry;
      retained.spirv = bo2zm::shaderlib::Spirv(entry);
      shaders.push_back(std::move(retained));
    }
  }
  std::vector<fs::path> packed;
  size_t missing = 0, retained = 0;
  for (const auto& path : paths) {
    auto original = ReadAll(path);
    if (base.Find(original)) {
      ++retained;
      continue;
    }
    const std::string stem = path.stem().string();
    const fs::path spv = compiled / ((stem.rfind("vs_", 0) == 0 ? "v_" : "p_") + stem + ".spv");
    if (!fs::exists(spv)) {
      std::printf("  no SPIR-V for %s\n", stem.c_str());
      ++missing;
      continue;
    }
    bo2zm::shaderlib::Shader s;
    s.original = std::move(original);
    const auto bytes = ReadAll(spv);
    if (bytes.size() % 4) {
      throw std::runtime_error("misaligned SPIR-V: " + spv.string());
    }
    for (size_t i = 0; i < bytes.size(); i += 4) {
      s.spirv.push_back(uint32_t(bytes[i]) | uint32_t(bytes[i + 1]) << 8 | uint32_t(bytes[i + 2]) << 16 |
                        uint32_t(bytes[i + 3]) << 24);
    }
    shaders.push_back(std::move(s));
    packed.push_back(path);
  }

  auto package = bo2zm::shaderlib::PackShaders(std::move(shaders));
  bo2zm::shaderlib::ShaderLibrary library;
  library.Load(package);
  for (const auto& path : packed) {
    if (!library.Find(ReadAll(path))) {
      throw std::runtime_error("not found after packing: " + path.string());
    }
  }
  for (const auto& entry : base.shaders()) {
    const auto* merged = library.Find(entry.original);
    if (!merged || merged->spirv != bo2zm::shaderlib::Spirv(entry)) {
      throw std::runtime_error("base translation changed or missing after packing");
    }
  }
  std::ofstream f(output, std::ios::binary);
  if (!f.write(reinterpret_cast<const char*>(package.data()), package.size()) || !f.flush()) {
    throw std::runtime_error("could not write the whole package");
  }
  std::printf("%zu containers: %zu added, %zu already in base, %zu base entries retained (%zu total), "
              "%zu without SPIR-V; %zu bytes; all found again\n",
              paths.size(), packed.size(), retained, base.shaders().size(), library.shaders().size(), missing,
              package.size());
  return 0;
} catch (const std::exception& error) {
  std::fprintf(stderr, "error: %s\n", error.what());
  return 1;
}
