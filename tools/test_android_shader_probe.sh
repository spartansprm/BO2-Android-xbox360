#!/usr/bin/env bash
set -euo pipefail
TASK_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
GLSLC="${1:?path to glslc}"
OUT="${2:?scratch output folder}"
mkdir -p "$OUT"
"${CXX:-g++}" -std=c++23 -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=all -pthread \
  -I"$TASK_ROOT/app/src/native/render" "$TASK_ROOT/tools/test_android_shader_probe.cpp" -o "$OUT/test-shader-probe"
"$OUT/test-shader-probe"
for shader in shader_probe.vert shader_probe.frag shader_probe_discard.frag shader_probe_depth.frag; do
  "$GLSLC" --target-env=vulkan1.0 -O "$TASK_ROOT/tools/shader_tests/$shader" -o "$OUT/$shader.spv"
done
"${CXX:-g++}" -std=c++23 -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=all \
  -I"$TASK_ROOT/app/src/native/render" -I"$TASK_ROOT/sdk/thirdparty/vulkan-headers/include" \
  "$TASK_ROOT/tools/test_android_shader_probe_vulkan.cpp" -l:libvulkan.so.1 -o "$OUT/test-shader-probe-vulkan"
for mode in corrected --unmasked-vs --unsafe-depth; do
  if [[ "$mode" == corrected ]]; then
    "$OUT/test-shader-probe-vulkan" "$OUT/shader_probe.vert.spv" "$OUT/shader_probe.frag.spv" "$OUT/shader_probe_discard.frag.spv" "$OUT/shader_probe_depth.frag.spv"
  else
    "$OUT/test-shader-probe-vulkan" "$OUT/shader_probe.vert.spv" "$OUT/shader_probe.frag.spv" "$OUT/shader_probe_discard.frag.spv" "$OUT/shader_probe_depth.frag.spv" "$mode"
  fi
done
