#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only

set -euo pipefail
TOUCH_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
TOUCH_TMP="$(mktemp -d "${TMPDIR:-/tmp}/bo2-touch-tests.XXXXXX")"
trap 'rm -rf "$TOUCH_TMP"' EXIT
TOUCH_ANDROID_JAR="${BO2_TEST_ANDROID_JAR:-${ANDROID_HOME:-}/platforms/android-35/android.jar}"
TOUCH_JSON_JAR="${BO2_TEST_JSON_JAR:?Set BO2_TEST_JSON_JAR to a real JSON-java jar (tested with 20240303)}"
test -s "$TOUCH_ANDROID_JAR"
test -s "$TOUCH_JSON_JAR"
if command -v javac >/dev/null; then TOUCH_JAVAC=(javac); else TOUCH_JAVAC=(java com.sun.tools.javac.Main); fi
TOUCH_SOURCE="$TOUCH_ROOT/android/app/src/main/java/org/bo2port/diagnostic"
mkdir "$TOUCH_TMP/classes"
"${TOUCH_JAVAC[@]}" -cp "$TOUCH_ANDROID_JAR:$TOUCH_JSON_JAR" -d "$TOUCH_TMP/classes" \
  "$TOUCH_SOURCE/TouchController.java" "$TOUCH_SOURCE/TouchProfiles.java" "$TOUCH_SOURCE/TouchAdsFire.java" \
  "$TOUCH_SOURCE/TouchSettings.java" "$TOUCH_SOURCE/EdgeMenuGesture.java" \
  "$TOUCH_ROOT/tools/test_touch_controller.java" "$TOUCH_ROOT/tools/test_touch_settings.java" \
  "$TOUCH_ROOT/tools/test_edge_menu_gesture.java"
for suite in TestTouchController TestTouchSettings TestEdgeMenuGesture; do
  java -Dbo2.tests.root="$TOUCH_ROOT" -cp "$TOUCH_TMP/classes:$TOUCH_JSON_JAR" "org.bo2port.diagnostic.$suite"
done
"${CXX:-g++}" -std=c++20 -O2 -ffp-contract=off -pthread "$TOUCH_ROOT/tools/test_touch_look.cpp" -o "$TOUCH_TMP/look"
"$TOUCH_TMP/look"
if [ "${1:-}" = --bridge ]; then
  TOUCH_SDK="${BO2_TEST_SDK_ROOT:-$TOUCH_ROOT/sdk}"
  for mode in 0 1; do
    "${CXX:-g++}" -std=c++23 -O1 -pthread -DFMT_HEADER_ONLY -DSPDLOG_FMT_EXTERNAL -DBO2_ANDROID_CAMPAIGN="$mode" \
      -I "$TOUCH_SDK/include" -I "$TOUCH_SDK/thirdparty/fmt/include" \
      -I "$TOUCH_SDK/thirdparty/spdlog/include" -I "$TOUCH_SDK/thirdparty/simde" \
      "$TOUCH_ROOT/tools/test_touch_look_bridge.cpp" -o "$TOUCH_TMP/bridge"
    "$TOUCH_TMP/bridge"
  done
fi
