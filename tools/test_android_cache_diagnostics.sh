#!/usr/bin/env bash
set -euo pipefail
TASK_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
OUT="${1:?scratch output folder}"
mkdir -p "$OUT"
for platform in android desktop; do
  extra=()
  if [ "$platform" = android ]; then extra=(-D__ANDROID__); fi
  "${CXX:-g++}" -std=c++23 -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=all \
    "${extra[@]}" -I"$TASK_ROOT/app/src/native/render" -I"$TASK_ROOT/sdk/include" \
    "$TASK_ROOT/tools/test_android_cache_diagnostics.cpp" -o "$OUT/test-$platform"
  fixture="$(mktemp -d "$OUT/$platform.XXXXXX")"
  "$OUT/test-$platform" "$fixture" save

  "$OUT/test-$platform" "$fixture" restore
  rm -r "$fixture"
done
