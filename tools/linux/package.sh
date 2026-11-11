#!/usr/bin/env bash


set -euo pipefail
OUT="$(cd "${1:?usage: tools/linux/package.sh <build folder>}" && pwd)"
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
PKG="$OUT/package/bo2zm-linux-arm64"
STRIP="$(command -v "llvm-strip${CLANG_SUFFIX:-}" || command -v llvm-strip || true)"

rm -rf "$PKG"
mkdir -p "$PKG"
cp "$OUT/bo2zm" "$PKG/"
for lib in librexruntime.so librexgpu-xenos.so; do
  src="$OUT/$lib"
  [ -f "$src" ] || src="$OUT/sdk-out/$lib"
  if [ -f "$src" ]; then
    cp "$src" "$PKG/"
  else
    echo "package: $lib not found in $OUT (skipped)" >&2
  fi
done
if [ -n "$STRIP" ]; then
  "$STRIP" --strip-unneeded "$PKG/bo2zm" "$PKG"/*.so
fi
cp "$ROOT/tools/linux/bo2zm.sh" "$PKG/bo2zm.sh"
cp "$ROOT/tools/linux/bo2zm.toml" "$PKG/bo2zm.toml"
cp "$ROOT/tools/linux/README-package.txt" "$PKG/README.txt"
chmod +x "$PKG/bo2zm" "$PKG/bo2zm.sh"

tar -C "$OUT/package" -czf "$OUT/package/bo2zm-linux-arm64.tar.gz" bo2zm-linux-arm64
echo "package: $PKG"
ls -la "$PKG"
