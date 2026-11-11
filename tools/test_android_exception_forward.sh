#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
SDK="${1:?path to pinned linux-amd64 SDK}"
OUT="${2:?scratch output folder}"
mkdir -p "$OUT"
"${CXX:-g++}" -std=c++23 -O1 -g0 -march=x86-64-v2 -DSPDLOG_FMT_EXTERNAL \
  -I"$ROOT/sdk/include" -I"$ROOT/sdk/thirdparty/simde" -I"$ROOT/android/game" \
  -I"$ROOT/sdk/thirdparty/fmt/include" -I"$ROOT/sdk/thirdparty/spdlog/include" \
  "$ROOT/tools/test_android_exception_forward.cpp" "$ROOT/android/game/fault_evidence.cpp" \
  "$ROOT/sdk/src/core/exception_handler_posix.cpp" \
  -L"$SDK/lib" -Wl,-rpath,"$SDK/lib" -lrexruntime -lfmt -pthread -ldl -o "$OUT/test-forward"
"$OUT/test-forward" "$OUT/evidence"
