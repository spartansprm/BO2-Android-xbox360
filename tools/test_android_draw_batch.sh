#!/usr/bin/env bash
set -euo pipefail
TASK_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
GLSLC="${1:?path to glslc}"
OUT="${2:?scratch output folder}"
mkdir -p "$OUT"
"${CXX:-g++}" -std=c++23 -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=all -pthread \
  -I"$TASK_ROOT/app/src/native/render" "$TASK_ROOT/tools/test_android_draw_batch.cpp" -o "$OUT/test-draw-batch"
if [[ $# -ge 3 ]]; then "$OUT/test-draw-batch" "$3"; else "$OUT/test-draw-batch"; fi
"$GLSLC" "$TASK_ROOT/tools/shader_tests/draw_batch.vert" -o "$OUT/batch.vert.spv"
"$GLSLC" "$TASK_ROOT/tools/shader_tests/draw_batch.frag" -o "$OUT/batch.frag.spv"
"${CXX:-g++}" -std=c++23 -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=all \
  -I"$TASK_ROOT/app/src/native/render" -I"$TASK_ROOT/sdk/thirdparty/vulkan-headers/include" \
  "$TASK_ROOT/tools/test_android_draw_batch_vulkan.cpp" -l:libvulkan.so.1 -o "$OUT/test-draw-batch-vulkan"
"$OUT/test-draw-batch-vulkan" "$OUT/batch.vert.spv" "$OUT/batch.frag.spv"
