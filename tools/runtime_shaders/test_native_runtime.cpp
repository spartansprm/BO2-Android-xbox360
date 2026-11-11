#include "bo2zm_native_cache_paths.h"
#include "bo2zm_native_shaders.h"
#include "bo2zm_shader_index.h"
#include "bo2zm_shader_library.h"
#include "bo2zm_shader_translator.h"

#include <rex/cvar.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <thread>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using bo2zm::native::NativeShaders;
using bo2zm::native::ShaderInput;

namespace {
int failures = 0;
void Expect(bool ok, const std::string& what) {
  std::printf("%s %s\n", ok ? "PASS" : "FAIL", what.c_str());
  failures += !ok;
}
std::vector<uint8_t> ReadAll(const fs::path& p) {
  std::ifstream f(p, std::ios::binary);
  return std::vector<uint8_t>((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}
void SetMode(int mode) {
  rex::cvar::SetFlagByName("bo2zm_native_shader_translate", std::to_string(mode));
}
}

int main(int argc, char** argv) try {
  if (argc != 2) {
    std::fprintf(stderr, "usage: test_native_runtime <containers>\n");
    return 2;
  }

  std::vector<std::vector<uint8_t>> containers;
  std::vector<fs::path> files;
  for (const auto& e : fs::directory_iterator(argv[1])) {
    if (e.path().extension() == ".bin") files.push_back(e.path());
  }
  std::sort(files.begin(), files.end());
  for (const auto& p : files) {
    auto bytes = ReadAll(p);
    if (!bo2zm::native::translate::Translate(bytes).spirv.empty()) containers.push_back(std::move(bytes));
    if (containers.size() == 12) break;
  }
  if (containers.size() < 8) throw std::runtime_error("need at least 8 translatable containers");
  const size_t in_pack = 6;


  const fs::path folder = bo2zm::native::cache::UserFolder();
  fs::create_directories(folder);
  fs::remove_all(folder / "cache");
  const fs::path pack = folder / "test.spvpack";
  {
    std::vector<bo2zm::shaderlib::Shader> shaders;
    for (size_t i = 0; i < in_pack; ++i) {
      bo2zm::shaderlib::Shader s;
      s.original = containers[i];
      s.spirv = bo2zm::native::translate::Translate(containers[i]).spirv;
      shaders.push_back(std::move(s));
    }
    const auto bytes = bo2zm::shaderlib::PackShaders(std::move(shaders));
    std::ofstream(pack, std::ios::binary).write(reinterpret_cast<const char*>(bytes.data()), std::streamsize(bytes.size()));
    fs::remove(bo2zm::native::ShaderIndexPath(pack));
  }

  SetMode(1);
  std::vector<int32_t> numbers;
  uint64_t identity1 = 0;
  {
    NativeShaders lib;
    Expect(lib.Load(pack), "library loads");
    identity1 = lib.Identity();
    for (size_t i = 0; i < in_pack; ++i) {
      const int32_t n = lib.NumberOfContainer(containers[i]);
      Expect(n >= 0 && lib.InLibrary(n), "library container " + std::to_string(i) + " keeps a library number");
    }
    for (size_t i = in_pack; i < containers.size(); ++i) {
      const int32_t n = lib.NumberOfContainer(containers[i]);
      numbers.push_back(n);
      Expect(n == int32_t(in_pack + (i - in_pack)) && !lib.InLibrary(n),
             "missing container " + std::to_string(i) + " gets runtime number " + std::to_string(n));
      Expect(lib.NumberOfContainer(containers[i]) == n, "the same number when registered again");
    }

    const ShaderInput* e = lib.PerNumber(uint32_t(numbers[0]));
    Expect(e && e->number == uint32_t(numbers[0]) && e->shader->original == containers[in_pack],
           "runtime entry has its container and number");
    const auto translated = bo2zm::native::translate::Translate(containers[in_pack]).spirv;
    Expect(e && bo2zm::shaderlib::Spirv(*e->shader) == translated, "runtime entry's SPIR-V is the translator's");
    Expect(e && e->shader->spirv.empty() && !e->shader->spirv_file.empty(), "SPIR-V is not kept in memory");
    Expect(e && !e->microcode.empty(), "container parsed (microcode)");
    Expect(lib.IdentifyContainer(containers[in_pack]) == e, "IdentifyContainer returns the same entry");
    Expect(lib.PerNumber(uint32_t(numbers[0])) == e, "PerNumber returns the same entry again");

    const ShaderInput* f = lib.IdentifyContainer(containers[in_pack + 1]);
    Expect(f && f->number == uint32_t(numbers[1]), "IdentifyContainer translates a missing container");
    for (size_t k = 2; k < numbers.size(); ++k) lib.PerNumber(uint32_t(numbers[k]));
  }

  {
    NativeShaders lib;
    Expect(lib.Load(pack), "second launch loads");
    Expect(lib.Identity() == identity1, "Identity is stable across launches");

    const ShaderInput* early = lib.PerNumber(uint32_t(numbers[2]));
    Expect(early && early->shader->original == containers[in_pack + 2], "prewarm makes a catalog shader from the cache");
    for (size_t i = in_pack; i < containers.size(); ++i) {
      Expect(lib.NumberOfContainer(containers[i]) == numbers[i - in_pack], "catalog keeps number " + std::to_string(numbers[i - in_pack]));
    }
  }

  {
    for (const auto& e : fs::directory_iterator(folder / "cache" / "runtime-shaders")) {
      if (e.path().extension() == ".bzrs") {
        auto bytes = ReadAll(e.path());
        bytes.back() ^= 0xFF;
        std::ofstream(e.path(), std::ios::binary | std::ios::trunc).write(reinterpret_cast<const char*>(bytes.data()), std::streamsize(bytes.size()));
      }
    }
    NativeShaders lib;
    lib.Load(pack);
    const int32_t n = lib.NumberOfContainer(containers[in_pack]);
    const ShaderInput* e = lib.PerNumber(uint32_t(n));
    Expect(e && bo2zm::shaderlib::Spirv(*e->shader) == bo2zm::native::translate::Translate(containers[in_pack]).spirv,
           "damaged cache file translated again");

  }

  {
    SetMode(0);
    NativeShaders lib;
    lib.Load(pack);
    Expect(lib.NumberOfContainer(containers[in_pack]) == -1, "mode 0: missing container has no number");
    Expect(lib.IdentifyContainer(containers[in_pack]) == nullptr, "mode 0: missing container not identified");
    Expect(lib.Identity() != identity1, "mode 0: Identity is the library's alone");
  }

  {
    SetMode(2);
    NativeShaders lib;
    lib.Load(pack);
    const int32_t n = lib.NumberOfContainer(containers[0]);
    Expect(n >= 0 && !lib.InLibrary(n), "mode 2: a library container gets a runtime number");
    const ShaderInput* e = lib.PerNumber(uint32_t(n));
    Expect(e && e->shader->original == containers[0], "mode 2: library container translated");
    Expect(lib.Identity() != identity1, "mode 2: different Identity");
  }


  {
    SetMode(1);
    fs::remove_all(folder / "cache");
    NativeShaders lib;
    lib.Load(pack);
    constexpr int kThreads = 8;
    std::vector<std::vector<const ShaderInput*>> seen(kThreads);
    std::vector<std::vector<int32_t>> seen_numbers(kThreads);
    std::vector<std::thread> threads;
    for (int t = 0; t < kThreads; ++t) {
      threads.emplace_back([&, t] {
        for (int round = 0; round < 3; ++round) {
          for (size_t k = 0; k < containers.size(); ++k) {
            const size_t i = (k + size_t(t)) % containers.size();
            const int32_t n = lib.NumberOfContainer(containers[i]);
            const ShaderInput* e = (t + round) % 2 ? lib.IdentifyContainer(containers[i]) : lib.PerNumber(uint32_t(n));
            if (round == 2) {
              seen_numbers[t].push_back(n);
              seen[t].push_back(e);
            }
          }
        }
      });
    }
    for (auto& th : threads) th.join();
    bool same = true;
    for (int t = 1; t < kThreads; ++t) {
      for (size_t k = 0; k < containers.size(); ++k) {
        const size_t i = (k + size_t(t)) % containers.size();
        const size_t k0 = i;
        same = same && seen_numbers[t][k] == seen_numbers[0][k0] && seen[t][k] == seen[0][k0] && seen[t][k];
      }
    }
    Expect(same, "8 threads: same numbers and entries for every container");
    size_t files_made = 0;
    for (const auto& e : fs::directory_iterator(folder / "cache" / "runtime-shaders")) files_made += e.path().extension() == ".bzrs";
    Expect(files_made == containers.size() - in_pack, "each missing container translated and cached once");
  }


  {
    SetMode(1);
    fs::remove_all(folder / "cache");
    NativeShaders lib;
    Expect(lib.Load(folder / "missing.spvpack"), "shaderless PM4 test: loads");
    int checked_pixel = 0, checked_vertex = 0;
    for (const auto& blob : containers) {
      bo2zm::shaderlib::Shader source;
      source.original = blob;
      source.vertices = (blob[3] & 1u) != 0;
      if (source.vertices ? checked_vertex : checked_pixel) continue;
      ShaderInput parsed;
      if (bo2zm::native::ReadContainer(source, parsed)) continue;
      std::vector<uint32_t> ring = parsed.microcode;
      if (source.vertices) {

        for (const auto& elem : parsed.elements) {
          const size_t word = size_t(elem.instruction) * 3;
          ring[word] ^= 0xFC000000u;
          ring[word + 1] ^= 0x00FF0000u;
          ring[word + 2] ^= 0x0000FFFFu;
        }
      }
      Expect(lib.Identify(source.vertices, ring) == nullptr,
             "PM4 shader is initially unknown before the game registers its container");
      const int32_t number = lib.NumberOfContainer(blob);
      const ShaderInput* matched = lib.Identify(source.vertices, ring);
      Expect(matched && matched->number == uint32_t(number),
             source.vertices ? "PM4 patched vertex shader identified without .spvpack"
                             : "PM4 pixel shader identified without .spvpack");
      Expect(matched && lib.Identify(source.vertices, ring) == matched,
             "PM4 ring cache reuses the identified runtime shader");
      (source.vertices ? checked_vertex : checked_pixel)++;
      if (checked_pixel && checked_vertex) break;
    }
    Expect(checked_pixel > 0 && checked_vertex > 0,
           "PM4 fixture includes both a pixel and a vertex shader");
  }

  {
    SetMode(1);
    fs::remove_all(folder / "cache");
    NativeShaders lib;
    Expect(lib.Load(folder / "missing.spvpack"), "no library: loads anyway (runtime translation)");
    const int32_t n0 = lib.NumberOfContainer(containers[0]);
    Expect(n0 == 0 && !lib.InLibrary(n0), "no library: first container gets number 0");
    const ShaderInput* e = lib.PerNumber(uint32_t(n0));
    Expect(e && e->shader->original == containers[0], "no library: shader translated");
    Expect(lib.IdentifyContainer(containers[1]) != nullptr, "no library: IdentifyContainer translates");
  }
  {
    SetMode(0);
    NativeShaders lib;
    Expect(!lib.Load(folder / "missing.spvpack"), "no library, mode 0: does not load (as 0.48)");
    SetMode(1);
  }

  {
    SetMode(1);
    rex::cvar::SetFlagByName("bo2zm_native_shader_library", "false");
    NativeShaders lib;
    Expect(lib.Load(pack), "library turned off: loads (runtime translation)");
    const int32_t n = lib.NumberOfContainer(containers[0]);
    Expect(n == 0 || !lib.InLibrary(n), "library turned off: a library container is translated, not taken from the pack");
    const ShaderInput* e = lib.PerNumber(uint32_t(n));
    Expect(e && e->shader->spirv_file.size() > 0, "library turned off: its SPIR-V comes from the runtime cache");
    rex::cvar::SetFlagByName("bo2zm_native_shader_library", "true");
    const std::string json = bo2zm::native::RuntimeShadersJson();
    Expect(json.find("\"no_library\":true") != std::string::npos && json.find("\"first_uses_waited\":") != std::string::npos,
           "report JSON: " + json);
  }

  {
    fs::remove_all(folder / "cache");
    NativeShaders lib;
    lib.Load(folder / "missing.spvpack");
    for (const auto& c : containers) lib.NumberOfContainer(c);
    size_t files_made = 0;
    for (int wait = 0; wait < 300 && files_made < containers.size(); ++wait) {
      std::this_thread::sleep_for(std::chrono::milliseconds(20));
      files_made = 0;
      for (const auto& f : fs::directory_iterator(folder / "cache" / "runtime-shaders"))
        files_made += f.path().extension() == ".bzrs";
    }
    Expect(files_made == containers.size(), "background threads translated every registered shader");
    const ShaderInput* e = lib.PerNumber(uint32_t(lib.NumberOfContainer(containers[3])));
    Expect(e && e->shader->original == containers[3], "a background-translated shader is made from the cache");
  }

  {
    SetMode(1);
    fs::remove(folder / "cache" / "runtime-shaders" / "catalog.bin");
    NativeShaders lib;
    lib.Load(pack);
    Expect(lib.Identity() != identity1, "new catalog changes Identity");
  }
  std::printf("%s: %d failure(s)\n", failures ? "FAILED" : "ALL PASSED", failures);
  return failures ? 1 : 0;
} catch (const std::exception& e) {
  std::fprintf(stderr, "error: %s\n", e.what());
  return 1;
}
