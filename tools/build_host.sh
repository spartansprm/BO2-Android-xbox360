#!/usr/bin/env bash


set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
SDK="$ROOT/sdk"
. "$ROOT/tools/toolchain.sh"  # CMake, Ninja and LLVM


if [ ! -d "$SDK/thirdparty/cli11/include" ]; then
  (cd "$ROOT" && python tools/fetch_thirdparty.py)
fi


for d in "$SDK"/thirdparty/*/; do [ -e "$d.git" ] || mkdir "$d.git"; done


python -I - "$SDK/thirdparty" <<'PYEOF'
import os, shutil, sys
root, fixed = sys.argv[1], 0
for _ in range(3):
    for dp, dn, fn in os.walk(root):
        if '.git' in dp.split(os.sep):
            continue
        for n in fn:
            p = os.path.join(dp, n)
            try:
                if os.path.getsize(p) > 300:
                    continue
                t = open(p, 'rb').read().decode('ascii')
            except Exception:
                continue
            if not t or '\n' in t or t.startswith('#'):
                continue
            tgt = os.path.normpath(os.path.join(dp, t))
            if os.path.isfile(tgt) and os.path.getsize(tgt) > 300:
                shutil.copyfile(tgt, p); fixed += 1
            elif os.path.isdir(tgt):
                os.remove(p); shutil.copytree(tgt, p); fixed += 1
print(f'symlink stubs replaced: {fixed}')
PYEOF


cmake -S "$SDK" -B "$ROOT/out/host" -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ \
  -DCMAKE_C_FLAGS=-march=x86-64-v2 -DCMAKE_CXX_FLAGS=-march=x86-64-v2 \
  -DGIT_EXECUTABLE=C:/no-git-here/git.exe \
  "-DCMAKE_PROJECT_ReXGlue_INCLUDE=$ROOT/cmake/host_tools_inject.cmake"

cmake --build "$ROOT/out/host" --target rexglue xexdump -j 12

echo
echo "rexglue: $SDK/out/win-amd64/rexglue.exe"
echo "xexdump: $ROOT/out/host/switch_bo2/xexdump.exe"
echo "Both need $SDK/out/win-amd64 on PATH (runtime DLLs)."
