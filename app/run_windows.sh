#!/usr/bin/env bash


set -uo pipefail
APP="$(cd "$(dirname "$0")" && pwd)"
SECS="${1:-0}"; shift || true
DATA="$(bash "$APP/../tools/data_dir.sh")"
EXE="$APP/out/win${BO2_BUILD_TAG:+-$BO2_BUILD_TAG}/bo2zm.exe"
EXE_W="$(cygpath -w "$EXE")"
COMPANION_W="$(cygpath -w "$(dirname "$EXE")/bo2cp.exe")"
LOCK="$DATA/pc-run.lock"


own_pids() {
  powershell -NoProfile -Command "Get-CimInstance Win32_Process -Filter \"Name='bo2zm.exe' OR Name='bo2cp.exe'\" | Where-Object { \$_.ExecutablePath -eq '$EXE_W' -or \$_.ExecutablePath -eq '$COMPANION_W' } | ForEach-Object { \$_.ProcessId }" | tr -d '\r'
}
any_game() { tasklist //FI "IMAGENAME eq bo2zm.exe" //NH 2>/dev/null | grep -qi "bo2zm.exe" ||
             tasklist //FI "IMAGENAME eq bo2cp.exe" //NH 2>/dev/null | grep -qi "bo2cp.exe"; }
alive() { [ -n "$1" ] && tasklist //FI "PID eq $1" //NH 2>/dev/null | grep -q " $1 "; }

mkdir -p "$(dirname "$LOCK")"
announced=0
while ! mkdir "$LOCK" 2>/dev/null; do
  holder_pid="$(cat "$LOCK/winpid" 2>/dev/null || true)"
  if [ -n "$holder_pid" ] && ! alive "$holder_pid"; then
    rm -rf "$LOCK"  # its script is gone without releasing it
    continue
  fi
  if [ "$announced" = 0 ]; then
    echo "waiting for another PC run: $(cat "$LOCK/owner" 2>/dev/null)"
    announced=1
  fi
  sleep 5
done
echo "${BO2_BUILD_TAG:-main} since $(date +%H:%M:%S): $EXE_W" > "$LOCK/owner"
cat "/proc/$$/winpid" > "$LOCK/winpid" 2>/dev/null || true
trap 'rm -rf "$LOCK"' EXIT
while any_game; do  # a game started by other means (a debugger, RenderDoc) also gets to finish
  if [ "$announced" != 2 ]; then
    echo "waiting for a running bo2zm.exe to exit"
    announced=2
  fi
  sleep 5
done

DESKTOP="$(powershell -NoProfile -ExecutionPolicy Bypass -File "$(cygpath -w "$APP/../tools/pc_desktop_state.ps1")" 2>/dev/null | tr -d '\r')"
VOLUME="${BO2_VOLUME:-}"
if [ -z "$VOLUME" ]; then
  if [ -f "$DATA/gaming.flag" ]; then VOLUME=0; else VOLUME=5; fi
fi
export BO2_PC_TEST_VOLUME="$VOLUME"
PLACE=()
if [ "${BO2_FOREGROUND:-0}" != 1 ]; then
  export SDL_WINDOW_ACTIVATE_WHEN_SHOWN=0 SDL_WINDOW_ACTIVATE_WHEN_RAISED=0
  case " $* " in
    *" --monitor="*) ;;
    *) case "$DESKTOP" in screens=[2-9]*) PLACE=(--monitor=2) ;; esac ;;
  esac
fi


DEFAULTS=()
case " $* " in
  *" --bo2zm_renderer="*) ;;
  *) DEFAULTS+=(--bo2zm_renderer=native) ;;
esac
case " $* " in
  *" --bo2zm_test_god="*) ;;
  *) DEFAULTS+=(--bo2zm_test_god=true) ;;
esac

mkdir -p "$APP/logs"
LOG="$APP/logs/run_$(date +%Y%m%d_%H%M%S).log"
LOG_ARGS=()
if [ "${BO2_NUMBERED_LOGS:-0}" = 1 ]; then
  LOG="$(dirname "$EXE")/logs (numbered)"
else
  LOG_ARGS+=(--log_file="$(cygpath -w "$LOG")")
fi
if [ "${LOG_LEVEL:-info}" != none ]; then
  LOG_ARGS+=(--log_level="${LOG_LEVEL:-info}")
fi
"$EXE" \
  --game_data_root="$DATA/game" --update_data_root="$DATA/update" \
  --gpu_plugin=xenos \
  "${LOG_ARGS[@]}" --log_max_file_size_mb=200 --bo2zm_volume="$VOLUME" \
  "${PLACE[@]}" "${DEFAULTS[@]}" "$@" &
PID=$!
if [ "$SECS" -gt 0 ]; then


  end=$((SECONDS + SECS))
  while [ "$SECONDS" -lt "$end" ]; do
    if ! kill -0 "$PID" 2>/dev/null && [ -z "$(own_pids)" ]; then
      break
    fi
    sleep 2
  done
  for p in $(own_pids); do
    taskkill //F //PID "$p" >/dev/null 2>&1
  done
  kill "$PID" 2>/dev/null
fi
wait "$PID" 2>/dev/null
while [ -n "$(own_pids)" ]; do  # a relaunched copy of the game is still running
  sleep 2
done
echo "log: $LOG"
