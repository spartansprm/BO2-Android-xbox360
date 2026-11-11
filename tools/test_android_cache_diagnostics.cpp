#include "bo2zm_native_cache_paths.h"
#include "bo2zm_native_readback_stats.h"
#include <cassert>
#include <fstream>
#include <iostream>
#include <string>
#include <sys/stat.h>

namespace fs = std::filesystem;
static fs::path private_files, library_dir;
namespace rex {
const fs::path& GetAndroidPrivateFilesDir() { return private_files; }
namespace filesystem {
fs::path GetExecutableFolder() { return library_dir; }
}
}

int main(int argc, char** argv) {
  assert(argc == 3);
  const fs::path root = argv[1];
  private_files = root / "private files á";
  library_dir = root / "installed libraries";
  const std::string operation = argv[2];
  const auto file = bo2zm::native::cache::PipelineFile();
  const auto stamp = bo2zm::native::cache::Folder() / "bo2zm_native_pipelines.ok";
#if defined(__ANDROID__)
  assert(file == private_files / "game-user/cache/bo2zm_native_pipelines.bin");
#else
  assert(file == library_dir / "cache/bo2zm_native_pipelines.bin");
#endif
  constexpr char payload[] = "Authored pipeline cache persistence fixture";
  if (operation == "save") {
    fs::create_directories(library_dir);
#if defined(__ANDROID__)
    assert(chmod(library_dir.c_str(), 0555) == 0);
    std::error_code error;
    fs::create_directories(library_dir / "cache", error);
    assert(error);
#endif
    fs::create_directories(file.parent_path());
    {
      std::ofstream out(file, std::ios::binary);
      out.write(payload, sizeof(payload));
      out.close();
      assert(out);
    }
    { std::ofstream out(stamp); out << "fixture stamp"; assert(out); }
  } else {
    assert(operation == "restore");
    std::ifstream in(file, std::ios::binary);
    const std::string got{std::istreambuf_iterator<char>(in), {}};
    assert(got == std::string(payload, sizeof(payload)));
    assert(fs::is_regular_file(stamp));
#if defined(__ANDROID__)
    assert(fs::is_empty(library_dir));
    assert(chmod(library_dir.c_str(), 0755) == 0);
#endif
  }


  const uint16_t words[12] = {0, 1, 2, 65535, 1, 2, 3, 0, 2, 0, 4, 1};
  std::array<uint8_t, sizeof(words) + 1> bytes{};
  std::memcpy(bytes.data() + 1, words, sizeof(words));
  const auto s = bo2zm::native::MeasureUint16Readback(bytes.data() + 1, 3);
  assert(s.texels == 3);
  assert((s.minimum == std::array<uint16_t, 4>{0, 0, 2, 0}));
  assert((s.maximum == std::array<uint16_t, 4>{2, 2, 4, 65535}));
  assert((s.nonzero == std::array<size_t, 4>{2, 2, 3, 2}));
  assert(s.Mean(0) == 1 && s.Mean(1) == 1 && s.Mean(2) == 3);
  assert(s.sum[3] == 65536);
  std::array<uint8_t, 80 * 45 * 8> zero{};
  const auto z = bo2zm::native::MeasureUint16Readback(zero.data(), 80 * 45);
  assert(z.Mean(0) == 0 && z.nonzero[0] == 0 && z.maximum[3] == 0);
  const auto empty = bo2zm::native::MeasureUint16Readback(nullptr, 0);
  assert(empty.Mean(0) == 0 && empty.minimum[0] == 0);
  std::cout << operation << ": cache path/persistence and UINT16 measurement passed\n";
}
