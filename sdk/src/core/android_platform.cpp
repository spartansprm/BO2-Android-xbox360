// SPDX-License-Identifier: BSD-3-Clause
// Android app paths are supplied by the Java activity before Runtime::Setup.
#include <rex/android.h>
#include <rex/filesystem.h>
#include <rex/system.h>
#include <android/api-level.h>
#include <cerrno>
namespace {
std::filesystem::path library_dir, files_dir;
}
namespace rex {
uint32_t GetAndroidApiLevel() { return static_cast<uint32_t>(android_get_device_api_level()); }
void ConfigureAndroidPaths(std::filesystem::path libraries, std::filesystem::path files) {
  library_dir = std::move(libraries); files_dir = std::move(files);
}
const std::filesystem::path& GetAndroidNativeLibraryDir() { return library_dir; }
const std::filesystem::path& GetAndroidPrivateFilesDir() { return files_dir; }
bool InitializeAndroidSystemForApplicationContext() { return !library_dir.empty() && !files_dir.empty(); }
void ShutdownAndroidSystem() {}
namespace filesystem {
void AndroidInitialize() {}
void AndroidShutdown() {}
bool IsAndroidContentUri(const std::string_view value) { return value.starts_with("content://"); }
int OpenAndroidContentFileDescriptor(const std::string_view, const char*) {
  // The frontend imports private executable inputs through SAF; raw content URIs
  // are deliberately rejected until the VFS descriptor provider is integrated.
  errno = ENOTSUP; return -1;
}
}
}
