#!/usr/bin/env bash


set -euo pipefail
APP="$(cd "$(dirname "$0")" && pwd)"
BO2_DATA="$(bash "$APP/../tools/data_dir.sh")"
SDK="${BO2_SDK_DIR:-$APP/../sdk}"


OUT="$APP/out/win${BO2_BUILD_TAG:+-$BO2_BUILD_TAG}"


SDK_OUT_ARG=(-DREXGLUE_BUILD_HOST_TOOLS=OFF)
if [ -n "${BO2_BUILD_TAG:-}" ]; then
  SDK_OUT_ARG+=("-DREXGLUE_OUTPUT_DIR=$OUT/sdk-out")
fi


EXE_ICON="${BO2ZM_EXE_ICON:-${BO2_DATA}/art/icon_pc.ico}"
if [ -f "$EXE_ICON" ]; then EXE_ICON="$(cygpath -m "$EXE_ICON")"; else EXE_ICON=""; fi
ROOT="$APP/.."
. "$ROOT/tools/toolchain.sh"

DEVLINK_ARGS=("-DBO2ZM_DEVLINK=$([ "${BO2_DEVLINK:-1}" = "0" ] && echo OFF || echo ON)")
cmake -S "$APP" -B "$OUT" -G Ninja ${NINJA_EXE:+"-DCMAKE_MAKE_PROGRAM=$NINJA_EXE"} "${SDK_OUT_ARG[@]}" "${DEVLINK_ARGS[@]}" \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ \
  -DCMAKE_C_FLAGS=-march=x86-64-v2 -DCMAKE_CXX_FLAGS=-march=x86-64-v2 \
  -DGIT_EXECUTABLE=C:/no-git-here/git.exe \
  "-DBO2ZM_BUILD_ID=${BO2ZM_BUILD_ID:-pc-$(date +%Y%m%d-%H%M%S)}" \
  "-DBO2ZM_EXE_ICON=$EXE_ICON" \
  -DREXGLUE_USE_VULKAN=ON \
  "-DREXSDK_DIR=$SDK"


"${PYTHON:-python}" "$APP/../tools/direct_calls.py" --check --gen "$APP/generated/default_mp"
"${PYTHON:-python}" "$APP/../tools/setjmp_locals.py" --verify --gen "$APP/generated/default_mp"
cmake --build "$OUT" --target bo2zm -j "${JOBS:-12}"
