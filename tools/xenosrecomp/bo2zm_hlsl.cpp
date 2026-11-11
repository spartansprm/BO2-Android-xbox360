#include "shader_recompiler.h"

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

namespace {

std::vector<uint8_t> ReadAll(const std::filesystem::path& path) {
  std::vector<uint8_t> data;
  FILE* f = std::fopen(path.string().c_str(), "rb");
  if (!f) {
    return data;
  }
  std::fseek(f, 0, SEEK_END);
  const long n = std::ftell(f);
  std::fseek(f, 0, SEEK_SET);
  if (n > 0) {
    data.resize(size_t(n));
    if (std::fread(data.data(), 1, data.size(), f) != data.size()) {
      data.clear();
    }
  }
  std::fclose(f);
  return data;
}

std::string OneLine(std::string text) {
  for (char& c : text) {
    if (c == '\t' || c == '\n' || c == '\r') {
      c = ' ';
    }
  }
  return text;
}

}

int main(int argc, char** argv) try {
  if (argc < 4) {
    std::printf("usage: bo2zm_hlsl <input folder> <output folder> <shader_common.h>\n");
    return 1;
  }
  const std::filesystem::path input = argv[1];
  const std::filesystem::path output = argv[2];
  const std::vector<uint8_t> common = ReadAll(argv[3]);
  if (common.empty()) {
    std::printf("cannot read %s\n", argv[3]);
    return 1;
  }
  const std::string_view include(reinterpret_cast<const char*>(common.data()), common.size());

  if (std::filesystem::exists(output) && !std::filesystem::is_empty(output)) {
    std::fprintf(stderr, "the output folder must be empty (no mixing with earlier results)\n");
    return 1;
  }
  std::filesystem::create_directories(output);
  FILE* tsv = std::fopen((output / "translate.tsv").string().c_str(), "wb");
  if (!tsv) {
    std::fprintf(stderr, "cannot write translate.tsv\n");
    return 1;
  }
  std::fprintf(tsv, "name\tstage\tstatus\tdetail\n");

  size_t total = 0, ok = 0, failed = 0, skipped = 0;
  for (const auto& entry : std::filesystem::recursive_directory_iterator(input)) {
    if (!entry.is_regular_file() || entry.path().extension() != ".bin") {
      continue;
    }
    const std::string name = entry.path().stem().string();
    const std::vector<uint8_t> data = ReadAll(entry.path());
    ++total;
    if (data.size() < 24) {
      std::fprintf(tsv, "%s\t?\tskipped\ttruncated header\n", name.c_str());
      ++skipped;
      continue;
    }
    uint32_t flags_be = 0;
    std::memcpy(&flags_be, data.data(), 4);
    const uint32_t flags = __builtin_bswap32(flags_be);
    if ((flags & 0xFFFFFF00u) != 0x102A1100u) {
      std::fprintf(tsv, "%s\t?\tskipped\tsignature 0x%08X\n", name.c_str(), flags);
      ++skipped;
      continue;
    }

    ShaderRecompiler recompiler;
    try {
      recompiler.recompile(data.data(), include);
    } catch (const std::exception& error) {
      std::fprintf(tsv, "%s\t%s\tfailed\t%s\n", name.c_str(), (flags & 1) ? "vs" : "ps",
                   OneLine(error.what()).c_str());
      ++failed;
      continue;
    }
    const char* stage = recompiler.isPixelShader ? "ps" : "vs";
    if (recompiler.out.empty()) {
      std::fprintf(tsv, "%s\t%s\tfailed\tempty output\n", name.c_str(), stage);
      ++failed;
      continue;
    }
    const auto target = output / (std::string(recompiler.isPixelShader ? "p_" : "v_") + name + ".hlsl");
    FILE* f = std::fopen(target.string().c_str(), "wb");
    const bool written = f && std::fwrite(recompiler.out.data(), 1, recompiler.out.size(), f) == recompiler.out.size();
    if (f) {
      std::fclose(f);
    }
    std::fprintf(tsv, "%s\t%s\t%s\t%s\n", name.c_str(), stage, written ? "ok" : "failed",
                 written ? "" : "cannot write output");
    written ? ++ok : ++failed;
  }
  std::fclose(tsv);
  std::printf("%zu containers: %zu translated, %zu failed, %zu skipped\n", total, ok, failed, skipped);
  return 0;
} catch (const std::exception& error) {
  std::fprintf(stderr, "error: %s\n", error.what());
  return 1;
}
