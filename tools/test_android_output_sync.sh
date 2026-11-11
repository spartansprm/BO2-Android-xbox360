#!/usr/bin/env bash

set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
GLSLC="${1:?path to glslc (NDK shader-tools)}"
OUT="${2:?scratch output folder}"
mkdir -p "$OUT"
"$GLSLC" "$ROOT/tools/shader_tests/output_sync.vert" -o "$OUT/output.vert.spv"
"$GLSLC" "$ROOT/tools/shader_tests/output_sync.frag" -o "$OUT/output.frag.spv"
"${CXX:-g++}" -std=c++23 -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=all \
  -I"$ROOT/app/src/native/render" -I"$ROOT/sdk/thirdparty/vulkan-headers/include" \
  "$ROOT/tools/test_android_output_sync.cpp" -l:libvulkan.so.1 -o "$OUT/test-sync"
"$OUT/test-sync" "$OUT/output.vert.spv" "$OUT/output.frag.spv" --without-dependencies
"$OUT/test-sync" "$OUT/output.vert.spv" "$OUT/output.frag.spv"
