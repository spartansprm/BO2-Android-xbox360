#include "bo2zm_shader_library.h"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

int main(int argc, char** argv) try {
  if (argc < 3) {
    std::fprintf(stderr, "usage: dump_library <pack> <out> [step]\n");
    return 2;
  }
  const std::filesystem::path out = argv[2];
  const uint32_t step = argc > 3 ? uint32_t(std::strtoul(argv[3], nullptr, 10)) : 1;
  std::filesystem::create_directories(out);
  bo2zm::shaderlib::ShaderLibrary library;
  uint32_t written = 0;
  library.Scan(std::filesystem::path(argv[1]), [&](uint32_t n, const bo2zm::shaderlib::Shader& s) {
    if (step > 1 && n % step) return;
    const std::string base = (out / ("n" + std::to_string(n))).string();
    std::ofstream(base + ".bin", std::ios::binary)
        .write(reinterpret_cast<const char*>(s.original.data()), std::streamsize(s.original.size()));
    std::ofstream(base + ".lib.spv", std::ios::binary)
        .write(reinterpret_cast<const char*>(s.spirv.data()), std::streamsize(s.spirv.size() * 4));
    ++written;
  });
  std::printf("%u shaders written to %s\n", written, out.string().c_str());
  return 0;
} catch (const std::exception& e) {
  std::fprintf(stderr, "error: %s\n", e.what());
  return 1;
}
