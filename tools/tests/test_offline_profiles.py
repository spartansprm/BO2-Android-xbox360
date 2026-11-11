from pathlib import Path
import os
import re
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
STUBS = {
    "memory.h": r"""#pragma once
#include <rex/types.h>
namespace rex::memory {
template<class T> void copy_and_swap(T* out, const T* in, size_t count) {
  for (size_t i = 0; i < count; ++i) out[i] = rex::byte_swap(in[i]);
}
}
""",
    "stream.h": r"""#pragma once
#include <cstdint>
#include <cstring>
namespace rex::stream {
class ByteStream {
 public:
  ByteStream(uint8_t* data, size_t size, size_t offset = 0): data_(data), size_(size), offset_(offset) {}
  size_t offset() const { return offset_; }
  uint8_t* data() { return data_; }
  void Advance(size_t count) { offset_ += count; }
  void Write(const void* source, size_t count) { std::memcpy(data_ + offset_, source, count); offset_ += count; }
 private:
  uint8_t* data_; size_t size_, offset_;
};
}
""",
    "logging.h": "#pragma once\n#define REXSYS_WARN(...) ((void)0)\n",
    "system/kernel_state.h": r"""#pragma once
#include <array>
#include <filesystem>
#include <memory>
#include <vector>
#include <rex/system/xam/user_profile.h>
#include <rex/system/xam/profile_paths.h>
namespace rex::filesystem {
inline FILE* OpenFile(const std::filesystem::path& file, const char* mode) {
  return std::fopen(file.string().c_str(), mode);
}
}
namespace rex::system {
struct ContentBoundary {
  std::filesystem::path root;
  std::filesystem::path ResolveGameUserContentPath(uint64_t xuid) {
    return xam::ResolveProfileDirectory(root, 0x415608C3, xuid, 0xB13EBABEBABEBABE).path;
  }
};
class KernelState {
 public:
  explicit KernelState(const std::filesystem::path& root, int count = 1) : content_{root} {
    for (int i = 0; i < count; ++i) {
      user_profiles_[i] = std::make_unique<xam::UserProfile>(i);
      user_profiles_[i]->set_kernel_state(this);
    }
  }
  uint32_t title_id() const { return 0x415608C3; }
  ContentBoundary* content_manager() { return &content_; }
  // PROFILE_LOOKUPS
  void BroadcastNotification(uint32_t id, uint32_t mask) { notifications.emplace_back(id, mask); }
  std::vector<std::pair<uint32_t, uint32_t>> notifications;
 private:
  ContentBoundary content_;
  std::array<std::unique_ptr<xam::UserProfile>, 4> user_profiles_;
};
inline KernelState* active_kernel = nullptr;
}
#define REX_KERNEL_STATE() rex::system::active_kernel
""",
}


def function(source, name):
    pass
    match = re.search(r"^[\w:]+ " + re.escape(name) + r"\([^;]+?\) \{", source, re.M)
    if not match:
        raise RuntimeError("missing entry: " + name)
    start = source.index("{", match.start())
    depth, end = 1, start + 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[match.start():end], source[match.start():start].strip() + ";"


def main():
    compiler = os.environ.get("CXX") or shutil.which("g++") or "C:/msys64/ucrt64/bin/g++.exe"
    env = dict(os.environ)
    env["PATH"] = str(Path(compiler).parent) + os.pathsep + env["PATH"]
    with tempfile.TemporaryDirectory(prefix="bo2-offline-profiles-") as directory:
        temporary = Path(directory)
        includes = temporary / "include" / "rex"
        for name, source in STUBS.items():
            if name == "system/kernel_state.h":
                header = (ROOT / "sdk/include/rex/system/kernel_state.h").read_text(encoding="utf-8")
                lookups = header[header.index("  xam::UserProfile* user_profile("):header.index("  // Access must be guarded")]
                source = source.replace("  // PROFILE_LOOKUPS", lookups)
            file = includes / name
            file.parent.mkdir(parents=True, exist_ok=True)
            file.write_text(source, encoding="utf-8")
        executable = temporary / "profiles.exe"
        subprocess.run([compiler, "-std=c++23", "-DFMT_HEADER_ONLY", "-pthread",
                        "-I" + str(includes.parent), "-I" + str(ROOT / "sdk/include"),
                        "-I" + str(ROOT / "sdk/thirdparty/fmt/include"),
                        str(ROOT / "tools/tests/offline_profiles_test.cpp"),
                        str(ROOT / "sdk/src/system/xam/user_profile.cpp"), "-o", str(executable)],
                       check=True, env=env, cwd=ROOT)
        subprocess.run([str(executable), str(temporary / "users")], check=True, env=env, cwd=ROOT)

        user = (ROOT / "sdk/src/kernel/xam/xam_user.cpp").read_text(encoding="utf-8")
        content = (ROOT / "sdk/src/kernel/xam/xam_content.cpp").read_text(encoding="utf-8")
        names = ["XamUserGetXUID_entry", "XamUserGetSigninState_entry", "XamUserGetSigninInfo_entry",
                 "XamUserGetName_entry", "XamUserCheckPrivilege_entry", "XamUserGetMembershipTier_entry",
                 "XamUserIsOnlineEnabled_entry", "XamShowSigninUI_entry"]
        bodies, declarations = [], []
        for name in names:
            body, declaration = function(user, name)
            bodies.append(body)
            declarations.append(declaration)
        body, declaration = function(content, "ContentOwner")
        bodies.append(body)
        declarations.append(declaration)
        info = re.search(r"typedef struct \{\s*rex::be<uint64_t> xuid;.*?\} X_USER_SIGNIN_INFO;", user, re.S).group()
        prelude = """#pragma once
#include <rex/system/kernel_state.h>
#include <rex/system/xcontent.h>
#include <rex/system/xtypes.h>
#include <rex/math.h>
#include <rex/string.h>
namespace rex::kernel::xam {
using namespace rex::system;
using i32 = int32_t;
using u32 = uint32_t;
using mapped_u32 = rex::be<uint32_t>*;
using mapped_u64 = rex::be<uint64_t>*;
using mapped_string = char*;
template<class T> using ppc_ptr_t = T*;
"""
        entry_header = temporary / "user_entries.h"
        entry_header.write_text(prelude + info + "\n" + "\n".join(declarations) + "\n}\n", encoding="utf-8")
        entry_source = temporary / "user_entries.cpp"
        entry_source.write_text('#include "user_entries.h"\nnamespace rex::kernel::xam {\n' +
                                "\n".join(bodies) + "\n}\n", encoding="utf-8")
        executable = temporary / "entries.exe"
        subprocess.run([compiler, "-std=c++23", "-DFMT_HEADER_ONLY", "-pthread",
                        "-I" + str(temporary), "-I" + str(includes.parent), "-I" + str(ROOT / "sdk/include"),
                        "-I" + str(ROOT / "sdk/thirdparty/fmt/include"),
                        str(ROOT / "tools/tests/offline_profile_entries_test.cpp"), str(entry_source),
                        str(ROOT / "sdk/src/system/xam/user_profile.cpp"), "-o", str(executable)],
                       check=True, env=env, cwd=ROOT)
        subprocess.run([str(executable), str(temporary / "entry-users")], check=True, env=env, cwd=ROOT)


if __name__ == "__main__":
    main()
