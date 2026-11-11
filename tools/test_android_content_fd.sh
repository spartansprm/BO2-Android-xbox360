#!/usr/bin/env bash

set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
SDK="${1:?path to pinned linux-amd64 SDK}"
OUT="${2:?scratch output folder}"
mkdir -p "$OUT"
python3 "$ROOT/tools/make_content_fd_fixtures.py" "$OUT/fixtures"
"${CXX:-g++}" -std=c++23 -O1 -g -march=x86-64-v2 -DSPDLOG_FMT_EXTERNAL \
  -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer \
  -I"$ROOT/sdk/include" -I"$ROOT/sdk/thirdparty/simde" -I"$ROOT/android/game" \
  -I"$ROOT/sdk/thirdparty/fmt/include" -I"$ROOT/sdk/thirdparty/spdlog/include" \
  "$ROOT/tools/test_android_content_fd.cpp" \
  "$ROOT/sdk/src/filesystem/devices/stfs_container_device.cpp" \
  "$ROOT/sdk/src/filesystem/devices/stfs_container_file.cpp" \
  "$ROOT/sdk/src/filesystem/devices/stfs_container_entry.cpp" \
  "$ROOT/android/game/content_mount.cpp" \
  -L"$SDK/lib" -Wl,-rpath,"$SDK/lib" -lrexruntime -lfmt -pthread -o "$OUT/test-content-fd"
"$OUT/test-content-fd" "$OUT/fixtures/game.header" "$OUT/fixtures/Data0000" \
  "$OUT/fixtures/Data0001" "$OUT/fixtures/update.stfs"
