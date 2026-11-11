#!/usr/bin/env bash


set -euo pipefail
CP="$(cd "$(dirname "$0")" && pwd)"
SDK="${BO2_SDK_DIR:-$CP/../sdk}"
OUT="$CP/out/win"
ROOT="$CP/.."
. "$ROOT/tools/toolchain.sh"


BO2_DATA="${BO2_DATA:-$(bash "$ROOT/tools/data_dir.sh")}"
EXE_ICON="${BO2CP_EXE_ICON:-${BO2ZM_EXE_ICON:-${BO2_DATA}/art/icon_pc.ico}}"
if [ -f "$EXE_ICON" ]; then EXE_ICON="$(cygpath -m "$EXE_ICON")"; else EXE_ICON=""; fi
cmake -S "$CP" -B "$OUT" -G Ninja ${NINJA_EXE:+"-DCMAKE_MAKE_PROGRAM=$NINJA_EXE"} \
  -DREXGLUE_BUILD_HOST_TOOLS=OFF "-DREXGLUE_OUTPUT_DIR=$OUT/sdk-out" \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ \
  -DCMAKE_C_FLAGS=-march=x86-64-v2 -DCMAKE_CXX_FLAGS=-march=x86-64-v2 \
  -DGIT_EXECUTABLE=C:/no-git-here/git.exe \
  -DREXGLUE_USE_VULKAN=ON \
  "-DREXSDK_DIR=$SDK" \
  "-DBO2CP_EXE_ICON=$EXE_ICON"

"${PYTHON:-python}" -I "$ROOT/tools/direct_calls.py" --check --gen "$CP/generated/default"
"${PYTHON:-python}" -I "$ROOT/tools/setjmp_locals.py" --verify --gen "$CP/generated/default"
cmake --build "$OUT" --target bo2cp -j "${JOBS:-12}"
