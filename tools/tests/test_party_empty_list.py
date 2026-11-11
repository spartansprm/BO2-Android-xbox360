from pathlib import Path
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
source = (ROOT / "sdk/src/kernel/xam/xam_party.cpp").read_text(encoding="utf-8")
entry = re.search(r"u32 XamPartyGetUserList_entry\([^\n]+\) \{.*?\n\}", source, re.S)
assert entry, "party-list entry not found"
fixture = r'''
#include <array>
#include <cassert>
#include <cstdint>
#include <rex/types.h>
ENTRY
int main() {
  std::array<rex::be<uint32_t>, 5> output;
  for (auto& value : output) value = 0xA5A5A5A5u;
  const auto before = output;
  const mapped_u32 count{&output[1], 0x10000};
  assert(XamPartyGetUserList_entry(1, count) == 0x807D0003u);
  assert(uint32_t(output[1]) == 0);
  assert(uint32_t(output[0]) == uint32_t(before[0]));
  for (size_t i = 2; i < output.size(); ++i) assert(uint32_t(output[i]) == uint32_t(before[i]));
  assert(XamPartyGetUserList_entry(1, nullptr) == 0x807D0003u);
  assert(XamPartyGetUserList_entry(0, count) == 0x807D0003u);
  assert(uint32_t(output[1]) == 0);
}
'''.replace("ENTRY", entry.group())
compiler = Path("C:/Program Files/LLVM/bin/clang++.exe")
with tempfile.TemporaryDirectory(prefix="party-output-") as temporary:
    folder = Path(temporary)
    cpp, executable = folder / "check.cpp", folder / "check.exe"
    cpp.write_text(fixture, encoding="utf-8")
    subprocess.run([str(compiler), "-std=c++23", "-Wall", "-Wextra", "-Werror", "-Wno-unused-parameter",
                    "-I" + str(ROOT / "sdk/include"), "-I" + str(ROOT / "sdk/thirdparty/fmt/include"),
                    str(cpp), "-o", str(executable)], check=True)
    subprocess.run([str(executable)], check=True)
print("Production party-list entry clears only the count and preserves the no-party result")
