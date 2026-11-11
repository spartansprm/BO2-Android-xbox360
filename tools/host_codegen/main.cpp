// SPDX-License-Identifier: GPL-3.0-only


#include <cstdio>
#include <map>
#include <string>
#include <string_view>
#include <rex/codegen/manifest.h>
#include <rex/codegen/project_recompiler.h>
#include <rex/cvar.h>
#include <rex/logging.h>
#include "project_scan.h"

int main(int argc, char** argv) {
  if (argc != 4 || std::string_view(argv[1]) != "codegen" ||
      std::string_view(argv[2]) != "--ignore-stamp") {
    std::fprintf(stderr, "usage: bo2-codegen codegen --ignore-stamp <manifest.toml>\n");
    return 2;
  }
  rex::InitLoggingEarly();
  rex::cvar::ApplyEnvironment();
  std::map<std::string, std::string> categories;
  auto logging = rex::BuildLogConfig(nullptr, "info", categories);
  logging.log_to_console = true;
  rex::InitLogging(logging);
  auto manifest = rex::codegen::ManifestConfig::Load(argv[3]);
  if (!manifest) return 1;
  rexglue::cli::RefreshGeneratedGlue(manifest->manifestDir, manifest->projectName,
      "0.10.0", manifest->entrypoint.recompiler.outDirectoryPath);
  rex::codegen::ProjectRecompiler recompiler(std::move(*manifest));
  rex::codegen::ProjectRecompilerOptions options;
  options.ignoreStamp = true;
  options.sdkVersion = "0.10.0-bo2-local-generator";
  auto result = recompiler.Run(options);
  if (!result) {
    std::fprintf(stderr, "%s\n", result.error().what().c_str());
    return 1;
  }
  std::printf("Codegen complete: %zu files written, force=false\n", recompiler.writtenFiles().size());
  return 0;
}
