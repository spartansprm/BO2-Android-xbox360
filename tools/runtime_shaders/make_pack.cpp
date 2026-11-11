#include "bo2zm_shader_library.h"

#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

int main(int argc, char** argv) try {
  if (argc != 3) {
    std::fprintf(stderr, "usage: make_pack <out> <list>\n");
    return 2;
  }
  const auto read = [](const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    return std::vector<char>((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
  };
  std::vector<bo2zm::shaderlib::Shader> shaders;
  std::ifstream list(argv[2]);
  std::string container, module;
  while (list >> container >> module) {
    bo2zm::shaderlib::Shader s;
    const auto c = read(container);
    s.original.assign(c.begin(), c.end());
    const auto w = read(module);
    s.spirv.resize(w.size() / 4);
    std::memcpy(s.spirv.data(), w.data(), s.spirv.size() * 4);
    shaders.push_back(std::move(s));
  }
  const auto bytes = bo2zm::shaderlib::PackShaders(std::move(shaders));
  std::ofstream(argv[1], std::ios::binary).write(reinterpret_cast<const char*>(bytes.data()), std::streamsize(bytes.size()));
  std::printf("%s: %zu bytes\n", argv[1], bytes.size());
  return 0;
} catch (const std::exception& e) {
  std::fprintf(stderr, "error: %s\n", e.what());
  return 1;
}
