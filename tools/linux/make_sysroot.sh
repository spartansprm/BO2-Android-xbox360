#!/usr/bin/env bash


set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
OUT="${1:-$ROOT/out/linux-sysroot}"
MIRROR="${BO2ZM_UBUNTU_PORTS:-http://ports.ubuntu.com/ubuntu-ports}"

PACKAGES=(

  libc6-dev libstdc++-14-dev libgcc-14-dev linux-libc-dev

  libx11-dev libx11-xcb-dev libxcb1-dev libxext-dev libxrandr-dev libxcursor-dev libxi-dev libxfixes-dev libxss-dev
  libxtst-dev libxkbcommon-dev libwayland-dev wayland-protocols libdecor-0-dev libegl-dev libdrm-dev libgbm-dev

  libasound2-dev libpulse-dev libpipewire-0.3-dev libudev-dev libdbus-1-dev

  libvulkan1 mesa-vulkan-drivers
)

if [ -e "$OUT/usr/include/stdio.h" ]; then
  echo "sysroot already present: $OUT (delete it to rebuild)"
  exit 0
fi
mkdir -p "$(dirname "$OUT")"
TMP="$OUT.tmp"
rm -rf "$TMP"

IFS=,
mmdebstrap --variant=extract --arch=arm64 --components=main,universe --include="${PACKAGES[*]}" \
  --skip=cleanup/apt/cache noble "$TMP" "deb $MIRROR noble main universe"
unset IFS


for f in "$TMP"/var/cache/apt/archives/*.deb; do
  b="$(basename "$f" .deb)"
  echo "${b%_*}"
done | sort > "$TMP/packages.txt"
rm -f "$TMP"/var/cache/apt/archives/*.deb


for d in bin sbin lib; do
  [ -e "$TMP/$d" ] || ln -s "usr/$d" "$TMP/$d"
done


UNICODE_H="$TMP/usr/include/c++/14/bits/unicode.h"
if grep -q '^[[:space:]]*++this;$' "$UNICODE_H"; then
  sed -i 's/^\([[:space:]]*\)++this;$/\1++*this;/' "$UNICODE_H"
  echo "patched $UNICODE_H (++this -> ++*this)"
fi


find "$TMP" -type l -lname '/*' | while read -r link; do
  target="$(readlink "$link")"
  rel="$(realpath -m --relative-to="$(dirname "$link")" "$TMP$target")"
  ln -sfn "$rel" "$link"
done

mv "$TMP" "$OUT"
echo "sysroot: $OUT ($(wc -l < "$OUT/packages.txt") packages)"
if [ -f "$ROOT/tools/linux/sysroot-packages.txt" ] && ! diff -q "$ROOT/tools/linux/sysroot-packages.txt" "$OUT/packages.txt" > /dev/null; then
  echo "warning: the package list differs from tools/linux/sysroot-packages.txt:" >&2
  diff "$ROOT/tools/linux/sysroot-packages.txt" "$OUT/packages.txt" >&2 || true
fi
