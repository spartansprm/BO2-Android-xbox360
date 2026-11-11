#!/usr/bin/env bash
set -euo pipefail
TASK_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
PROFILE_JSON_JAR="${1:?path to org.json JAR for host test}"
PROFILE_TEST_OUT="${2:?host test output directory}"
mkdir -p "$PROFILE_TEST_OUT"
"${JAVA_HOME:?Set a full JDK JAVA_HOME}/bin/javac" -cp "$PROFILE_JSON_JAR" -d "$PROFILE_TEST_OUT" \
 "$TASK_ROOT/android/app/src/main/java/org/bo2port/diagnostic/ProcessResourceSampler.java" "$TASK_ROOT/tools/TestResourceSampler.java"
"$JAVA_HOME/bin/java" -cp "$PROFILE_JSON_JAR:$PROFILE_TEST_OUT" org.bo2port.diagnostic.TestResourceSampler
