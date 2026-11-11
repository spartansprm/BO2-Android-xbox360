#!/usr/bin/env bash


set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
PKG="$(cd "${1:?usage: smoke_qemu.sh <package folder> <data folder> [seconds] [args...]}" && pwd)"
DATA="$(cd "${2:?usage: smoke_qemu.sh <package folder> <data folder> [seconds] [args...]}" && pwd)"
SECS="${3:-600}"
shift 2
[ $# -gt 0 ] && shift
SYSROOT="$(cd "${BO2_LINUX_SYSROOT:-$ROOT/out/linux-sysroot}" && pwd)"
RUN="${BO2_SMOKE_DIR:-/tmp/bo2zm-smoke}"


rm -rf "$RUN"
mkdir -p "$RUN"
cp "$PKG"/bo2zm "$PKG"/*.so "$RUN/"
for f in game update bo2zm_shaders.spvpack bo2zm_shaders.spvidx; do
  [ -e "$DATA/$f" ] && ln -s "$DATA/$f" "$RUN/$f"
done


export QEMU_LD_PREFIX="$SYSROOT"
export VK_DRIVER_FILES="/usr/share/vulkan/icd.d/lvp_icd.aarch64.json"
export VK_ICD_FILENAMES="$VK_DRIVER_FILES"
export SDL_VIDEO_DRIVER=x11
unset WAYLAND_DISPLAY
cd "$RUN"
echo "smoke: $RUN, up to $SECS s"
status=0


xvfb-run -a -s "-screen 0 1280x720x24" \
  timeout --signal=INT --kill-after=30 "$SECS" \
  "${BO2_QEMU:-qemu-aarch64-static}" -L "$SYSROOT" ./bo2zm \
    --log_file="$RUN/bo2zm-smoke.log" --log_level=info \
    --bo2zm_renderer=native --bo2zm_volume=0 --fullscreen=false "$@" || status=$?
echo "smoke: exit status $status (124 = stopped at the time limit)"
tail -n 40 "$RUN/bo2zm-smoke.log" 2>/dev/null || true
