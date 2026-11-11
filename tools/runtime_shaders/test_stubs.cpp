#include <cstdlib>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace rex::filesystem {

std::filesystem::path GetExecutableFolder() {
  const char* dir = std::getenv("BO2_TEST_DIR");
  return dir ? std::filesystem::path(dir) : std::filesystem::temp_directory_path() / "bo2-runtime-shaders-test";
}
}

namespace rex::platform::env {
std::optional<std::string> get(std::string_view name) {
  const char* v = std::getenv(std::string(name).c_str());
  return v ? std::optional<std::string>(v) : std::nullopt;
}
}

namespace bo2zm::native {
bool LogVertexShaderMisses() { return false; }
}
