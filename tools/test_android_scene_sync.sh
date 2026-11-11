#!/usr/bin/env bash

set -euo pipefail
TASK_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
GLSLC="${1:?path to glslc}"
OUT="${2:?scratch output folder}"
mkdir -p "$OUT"
"$GLSLC" "$TASK_ROOT/tools/shader_tests/output_sync.vert" -o "$OUT/scene.vert.spv"
"$GLSLC" "$TASK_ROOT/tools/shader_tests/output_sync.frag" -o "$OUT/scene.frag.spv"
"${CXX:-g++}" -std=c++23 -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=all \
  -I"$TASK_ROOT/app/src/native/render" -I"$TASK_ROOT/sdk/thirdparty/vulkan-headers/include" \
  "$TASK_ROOT/tools/test_android_scene_sync.cpp" -l:libvulkan.so.1 -o "$OUT/test-scene-sync"
"$OUT/test-scene-sync" "$OUT/scene.vert.spv" "$OUT/scene.frag.spv" --without-pass-dependencies
"$OUT/test-scene-sync" "$OUT/scene.vert.spv" "$OUT/scene.frag.spv" --without-transfer-barriers
"$OUT/test-scene-sync" "$OUT/scene.vert.spv" "$OUT/scene.frag.spv"
"$OUT/test-scene-sync" "$OUT/scene.vert.spv" "$OUT/scene.frag.spv" --gpu-marks-detailed
"$OUT/test-scene-sync" "$OUT/scene.vert.spv" "$OUT/scene.frag.spv" --gpu-marks-endpoints
"$OUT/test-scene-sync" "$OUT/scene.vert.spv" "$OUT/scene.frag.spv" --pixel-probe
"$OUT/test-scene-sync" "$OUT/scene.vert.spv" "$OUT/scene.frag.spv" --pass-profile
"${CXX:-g++}" -std=c++23 -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=all \
  -I"$TASK_ROOT/app/src/native/render" -I"$TASK_ROOT/sdk/thirdparty/vulkan-headers/include" \
  "$TASK_ROOT/tools/test_android_scene_output.cpp" -l:libvulkan.so.1 -o "$OUT/test-scene-output"
"$OUT/test-scene-output" "$OUT/scene.vert.spv" "$OUT/scene.frag.spv" --without-output-barrier
"$OUT/test-scene-output" "$OUT/scene.vert.spv" "$OUT/scene.frag.spv"
