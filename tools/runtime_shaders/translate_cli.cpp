#include "bo2zm_shader_translator.h"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

int main(int argc, char** argv) {
  if (argc != 3) {
    std::fprintf(stderr, "usage: translate_cli <containers> <output>\n");
    return 2;
  }
  const std::filesystem::path input = argv[1], output = argv[2];
  std::filesystem::create_directories(output);
  std::ofstream tsv(output / "runtime.tsv");
  tsv << "name\tstage\tstatus\tms_hlsl\tms_compile\tdetail\n";
  std::printf("translator identity %016llX\n", (unsigned long long)bo2zm::native::translate::Identity());
  size_t ok = 0, failed = 0;
  double total_ms = 0;
  for (const auto& entry : std::filesystem::recursive_directory_iterator(input)) {
    if (!entry.is_regular_file() || entry.path().extension() != ".bin") {
      continue;
    }
    std::ifstream f(entry.path(), std::ios::binary);
    const std::vector<uint8_t> data((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    const auto r = bo2zm::native::translate::Translate(data);
    std::string detail = r.error;
    for (char& c : detail) {
      if (c == '\n' || c == '\t' || c == '\r') c = ' ';
    }
    const std::string name = entry.path().stem().string();
    if (!r.spirv.empty()) {
      std::ofstream out(output / (name + ".spv"), std::ios::binary);
      out.write(reinterpret_cast<const char*>(r.spirv.data()), std::streamsize(r.spirv.size() * 4));
      ++ok;
    } else {
      ++failed;
    }
    total_ms += r.ms_hlsl + r.ms_compile;
    tsv << name << '\t' << (r.pixel ? "ps" : "vs") << '\t' << (r.spirv.empty() ? "failed" : "ok") << '\t'
        << r.ms_hlsl << '\t' << r.ms_compile << '\t' << detail.substr(0, 300) << '\n';
  }
  std::printf("%zu translated, %zu failed, %.1f ms average\n", ok, failed, (ok + failed) ? total_ms / double(ok + failed) : 0.0);
  return 0;
}
