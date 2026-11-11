#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>

#include <fmt/format.h>
#include <rex/cvar.h>
#include <rex/kernel/init.h>
#include <rex/logging.h>
#include <rex/runtime.h>
#include <rex/system/kernel_state.h>
#include <rex/system/user_module.h>
#include <rex/system/xex_module.h>
#include <rex/system/xmemory.h>

namespace fs = std::filesystem;
using rex::X_STATUS;

namespace {

std::string version_string(uint32_t v) {
  return fmt::format("{}.{}.{}.{}", v >> 28, (v >> 24) & 0xF, (v >> 8) & 0xFFFF, v & 0xFF);
}

std::string json_escape(const std::string& s) {
  std::string out;
  for (char c : s) {
    if (c == '"' || c == '\\') out += '\\';
    out += c;
  }
  return out;
}

}

int main(int argc, char** argv) {
  if (argc != 4) {
    std::fprintf(stderr, "usage: xexdump <game_dir> <xex_name> <out_dir>\n");
    return 2;
  }
  const fs::path game_dir = fs::absolute(argv[1]);
  const std::string xex_name = argv[2];
  const fs::path out_dir = fs::absolute(argv[3]);
  fs::create_directories(out_dir);


  rex::InitLoggingEarly();
  rex::cvar::ApplyEnvironment();
  std::map<std::string, std::string> category_levels;
  auto log_config = rex::BuildLogConfig(nullptr, "info", category_levels);
  log_config.log_to_console = true;
  rex::InitLogging(log_config);

  const bool has_patch = fs::exists(game_dir / (xex_name + "p"));

  rex::Runtime runtime(game_dir);
  auto status = runtime.Setup(rex::RuntimeConfig{
      .kernel_init = rex::kernel::InitializeKernel,
      .tool_mode = true,
  });
  if (status != X_STATUS_SUCCESS) {
    std::fprintf(stderr, "Runtime::Setup failed: 0x%08X\n", status);
    return 1;
  }

  status = runtime.LoadXexImage("game:\\" + xex_name);
  if (status != X_STATUS_SUCCESS) {
    std::fprintf(stderr, "LoadXexImage failed: 0x%08X\n", status);
    return 1;
  }

  auto user_module = runtime.kernel_state()->GetExecutableModule();
  auto* xex = user_module ? user_module->xex_module() : nullptr;
  if (!xex) {
    std::fprintf(stderr, "no executable module after load\n");
    return 1;
  }

  const uint32_t base = xex->base_address();
  const uint32_t size = xex->image_size();
  const uint8_t* image = runtime.memory()->TranslateVirtual(base);

  const fs::path stem = fs::path(xex_name).stem();
  const fs::path image_path = out_dir / (stem.string() + ".image.bin");
  {
    std::ofstream f(image_path, std::ios::binary);
    f.write(reinterpret_cast<const char*>(image), size);
  }

  std::string json = "{\n";
  json += fmt::format("  \"xex\": \"{}\",\n", json_escape(xex_name));
  json += fmt::format("  \"patch_applied\": {},\n", has_patch ? "true" : "false");
  json += fmt::format("  \"image_file\": \"{}\",\n", json_escape(image_path.filename().string()));
  json += fmt::format("  \"base_address\": \"0x{:08X}\",\n", base);
  json += fmt::format("  \"image_size\": {},\n", size);
  json += fmt::format("  \"entry_point\": \"0x{:08X}\",\n", xex->entry_point());
  if (auto* exec = xex->opt_execution_info()) {
    json += fmt::format("  \"title_id\": \"{:08X}\",\n", uint32_t(exec->title_id));
    json += fmt::format("  \"version\": \"{}\",\n", version_string(exec->version_value));
    json += fmt::format("  \"base_version\": \"{}\",\n", version_string(exec->base_version_value));
  }

  json += "  \"sections\": [\n";
  const auto& sections = xex->pe_sections();
  for (size_t i = 0; i < sections.size(); i++) {
    const auto& s = sections[i];
    json += fmt::format(
        "    {{\"name\": \"{}\", \"address\": \"0x{:08X}\", \"size\": {}, \"flags\": \"0x{:08X}\"}}{}\n",
        json_escape(s.name), s.address, s.size, s.flags, i + 1 < sections.size() ? "," : "");
  }
  json += "  ],\n";

  json += "  \"imports\": [\n";
  const auto& libs = *xex->import_libraries();
  for (size_t i = 0; i < libs.size(); i++) {
    const auto& lib = libs[i];
    json += fmt::format("    {{\"library\": \"{}\", \"version\": \"{}.{}.{}.{}\", \"count\": {}, \"ordinals\": [",
                        json_escape(lib.name), uint32_t(lib.version.major), uint32_t(lib.version.minor),
                        uint32_t(lib.version.build), uint32_t(lib.version.qfe), lib.imports.size());
    for (size_t j = 0; j < lib.imports.size(); j++) {
      json += fmt::format("{}{}", j ? ", " : "", lib.imports[j].ordinal);
    }
    json += fmt::format("]}}{}\n", i + 1 < libs.size() ? "," : "");
  }
  json += "  ]\n}\n";

  std::ofstream(out_dir / (stem.string() + ".summary.json")) << json;
  std::printf("%s: base 0x%08X size %u entry 0x%08X patch=%s -> %s\n", xex_name.c_str(), base, size,
              xex->entry_point(), has_patch ? "yes" : "no", image_path.string().c_str());


  std::fflush(stdout);
  std::_Exit(0);
}
