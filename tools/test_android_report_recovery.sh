#!/usr/bin/env bash
set -euo pipefail
TASK_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
RECOVERY_JSON_JAR="${1:?path to org.json JAR for host test}"
RECOVERY_TEST_OUT="${2:?host test output directory}"
mkdir -p "$RECOVERY_TEST_OUT"
"${JAVA_HOME:?Set a full JDK JAVA_HOME}/bin/javac" -cp "$RECOVERY_JSON_JAR" -d "$RECOVERY_TEST_OUT" \
 "$TASK_ROOT/android/app/src/main/java/org/bo2port/diagnostic/ProcessResourceSampler.java" \
 "$TASK_ROOT/android/app/src/main/java/org/bo2port/diagnostic/GameReportFiles.java" \
 "$TASK_ROOT/tools/TestGameReportRecovery.java"
"$JAVA_HOME/bin/java" -Xmx96m -cp "$RECOVERY_JSON_JAR:$RECOVERY_TEST_OUT" org.bo2port.diagnostic.TestGameReportRecovery
