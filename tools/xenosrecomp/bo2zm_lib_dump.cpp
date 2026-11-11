#include "bo2zm_shader_library.h"
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
int main(int argc, char** argv) try {
  if (argc < 4) { std::fprintf(stderr, "usage: bo2zm_lib_dump <library> <out> <n>...\n"); return 1; }
  bo2zm::shaderlib::ShaderLibrary library;
  library.Load(std::filesystem::path(argv[1]));
  std::filesystem::create_directories(argv[2]);
  for (int i = 3; i < argc; ++i) {
    const size_t n = std::strtoul(argv[i], nullptr, 10);
    if (n >= library.shaders().size()) { std::printf("n%zu: out of range\n", n); continue; }
    const auto& s = library.shaders()[n];
    const auto path = std::filesystem::path(argv[2]) / ("n" + std::to_string(n) + (s.vertices ? "_vs.bin" : "_ps.bin"));
    std::ofstream(path, std::ios::binary).write(reinterpret_cast<const char*>(s.original.data()), s.original.size());
    std::printf("n%zu -> %s (%zu bytes)\n", n, path.string().c_str(), s.original.size());
  }
  return 0;
} catch (const std::exception& e) { std::fprintf(stderr, "error: %s\n", e.what()); return 1; }
