#!/usr/bin/env bash

set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
SDK="${1:?path to pinned linux-amd64 SDK}"
OUT="${2:?scratch output folder}"
mkdir -p "$OUT"
python3 "$ROOT/tools/make_dlc_fixture.py" "$OUT/marketplace.stfs"
"${CXX:-g++}" -std=c++23 -O0 -g0 -ffunction-sections -fdata-sections -march=x86-64-v2 \
  -DSPDLOG_FMT_EXTERNAL -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer \
  -I"$ROOT/sdk/include" -I"$ROOT/sdk/thirdparty/simde" -I"$ROOT/android/game" \
  -I"$ROOT/sdk/thirdparty/fmt/include" -I"$ROOT/sdk/thirdparty/spdlog/include" \
  "$ROOT/tools/test_android_dlc.cpp" "$ROOT/sdk/src/system/xam/content_manager.cpp" \
  "$ROOT/sdk/src/system/xam/user_profile.cpp" \
  "$ROOT/sdk/src/filesystem/devices/stfs_container_device.cpp" \
  "$ROOT/sdk/src/filesystem/devices/stfs_container_file.cpp" \
  "$ROOT/sdk/src/filesystem/devices/stfs_container_entry.cpp" \
  -L"$SDK/lib" -Wl,-rpath,"$SDK/lib" -Wl,--gc-sections -lrexruntime -lfmt -pthread -o "$OUT/test-dlc"
"$OUT/test-dlc" "$OUT/marketplace.stfs" "$OUT/user-content"
