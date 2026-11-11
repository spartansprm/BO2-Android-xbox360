#!/usr/bin/env bash


set -euo pipefail
APP="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$APP/.." && pwd)"
SDK="${BO2_SDK_DIR:-$ROOT/sdk}"
OUT="${BO2_LINUX_OUT:-$APP/out/linux-arm64${BO2_BUILD_TAG:+-$BO2_BUILD_TAG}}"
MARCH="${BO2_LINUX_MARCH:-armv8-a}"
HOST_ARCH="$(uname -m)"

if [ "$(uname -s)" != Linux ]; then
  echo "app/build_linux.sh runs on Linux (on Windows: inside WSL2)" >&2
  exit 1
fi
if [ ! -f "$APP/generated/default_mp/sources.cmake" ]; then
  echo "no generated code in app/generated/default_mp: run tools/codegen.sh on your copy of the game first" >&2
  exit 1
fi


clang_major() { { "clang$1" -dumpversion 2> /dev/null || true; } | cut -d. -f1; }
if [ -z "${CLANG_SUFFIX+set}" ]; then
  for s in "" -22 -21 -20 -19; do
    v="$(clang_major "$s")"
    if [ -n "$v" ] && [ "$v" -ge 19 ]; then
      export CLANG_SUFFIX="$s"
      break
    fi
  done
fi
v="$(clang_major "${CLANG_SUFFIX:-}")"
if [ -z "$v" ] || [ "$v" -lt 19 ]; then
  echo "clang 19 or newer is needed (found: ${v:-none}); on Ubuntu 24.04: apt install clang-20 lld-20 llvm-20" >&2
  exit 1
fi
command -v "ld.lld${CLANG_SUFFIX:-}" > /dev/null || command -v ld.lld > /dev/null ||
  { echo "lld not found (apt install lld${CLANG_SUFFIX:-})" >&2; exit 1; }

ARGS=()
if [ "$HOST_ARCH" = aarch64 ] && [ -z "${BO2_LINUX_SYSROOT:-}" ]; then

  ARGS+=("-DCMAKE_C_COMPILER=clang${CLANG_SUFFIX:-}" "-DCMAKE_CXX_COMPILER=clang++${CLANG_SUFFIX:-}")
else
  export BO2ZM_LINUX_SYSROOT="${BO2_LINUX_SYSROOT:-$ROOT/out/linux-sysroot}"
  if [ ! -e "$BO2ZM_LINUX_SYSROOT/usr/include/stdio.h" ]; then
    bash "$ROOT/tools/linux/make_sysroot.sh" "$BO2ZM_LINUX_SYSROOT"
  fi
  ARGS+=("-DCMAKE_TOOLCHAIN_FILE=$ROOT/tools/linux/cmake/aarch64-linux-gnu.cmake")
fi


FLAGS="-march=$MARCH"
LINK="-fuse-ld=lld -static-libstdc++ -static-libgcc"
RELEASE="-O3 -DNDEBUG"
if [ "${BO2_LTO:-0}" = 1 ]; then
  RELEASE="$RELEASE -flto=thin"
  LINK="$LINK -flto=thin"
fi


SHARED_LINK="$LINK -Wl,-Bsymbolic-functions"


SDK_OUT_ARG=("-DREXGLUE_OUTPUT_DIR=$OUT/sdk-out")
JOBS="${JOBS:-$(n=$(nproc); [ "$n" -gt 12 ] && n=12; echo "$n")}"

cmake -S "$APP" -B "$OUT" -G Ninja "${ARGS[@]}" "${SDK_OUT_ARG[@]}" \
  -DCMAKE_BUILD_TYPE=Release \
  "-DCMAKE_C_FLAGS=$FLAGS" "-DCMAKE_CXX_FLAGS=$FLAGS" \
  "-DCMAKE_C_FLAGS_RELEASE=$RELEASE" "-DCMAKE_CXX_FLAGS_RELEASE=$RELEASE" \
  "-DCMAKE_EXE_LINKER_FLAGS=$LINK" "-DCMAKE_SHARED_LINKER_FLAGS=$SHARED_LINK" "-DCMAKE_MODULE_LINKER_FLAGS=$SHARED_LINK" \
  "-DCMAKE_BUILD_RPATH=\$ORIGIN" \
  -DREXGLUE_BUILD_HOST_TOOLS=OFF \
  -DREXGLUE_USE_VULKAN=ON \
  "-DBO2ZM_GUEST_MEMORY_PLAIN=$([ "${BO2_GUEST_MEMORY_PLAIN:-1}" = 1 ] && echo ON || echo OFF)" \
  "-DBO2ZM_BUILD_ID=linux-arm64-$(date +%Y%m%d-%H%M%S)" \
  "-DREXSDK_DIR=$SDK"


PY="${PYTHON:-python3}"
"$PY" "$ROOT/tools/direct_calls.py" --check --gen "$APP/generated/default_mp"
"$PY" "$ROOT/tools/setjmp_locals.py" --verify --gen "$APP/generated/default_mp"
if [ "${BO2_GUEST_MEMORY_PLAIN:-1}" = 1 ]; then
  "$PY" "$ROOT/tools/guest_barriers.py" --check --gen "$APP/generated/default_mp"
fi

cmake --build "$OUT" -j "$JOBS"
bash "$ROOT/tools/linux/package.sh" "$OUT"
