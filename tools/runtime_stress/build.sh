#!/usr/bin/env bash


set -euo pipefail
VARIANT="${1:-fast}"
SCALE="${2:-1.0}"
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$HERE/../.."
SDK="$ROOT/sdk"
OUT="${3:-$HERE/out}"
mkdir -p "$OUT"
DEFS=()
if [ "$VARIANT" = fast ]; then
  DEFS=(-DREX_FAST_WAITS_TEST)
fi
EXE="$OUT/test_waits_$VARIANT"
g++ -std=c++20 -O2 -g -pthread -Wall -Wextra -Wno-unused-parameter "${DEFS[@]}" \
  -DBO2ZM_TEST_WAITS_STANDALONE -include rex/assert.h \
  -I "$HERE/stub" -I "$SDK/include" -I "$ROOT/app/src" \
  "$SDK/src/core/threading_posix.cpp" "$SDK/src/core/threading.cpp" "$SDK/src/core/timer_queue.cpp" \
  "$SDK/src/core/clock.cpp" "$SDK/src/core/clock_posix.cpp" \
  "$ROOT/app/src/bo2zm_test_waits.cpp" "$HERE/main.cpp" \
  -o "$EXE"
echo "built $EXE"
"$EXE" "$SCALE"
