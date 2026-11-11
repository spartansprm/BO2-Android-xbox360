#!/usr/bin/env bash


set -euo pipefail
TAG="${1:?usage: tools/bootstrap_worktree.sh <tag> [--own-generated]}"
shift
OWN_GEN=0
for a in "$@"; do
  case "$a" in
    --own-generated) OWN_GEN=1 ;;
    *) echo "unknown option $a" >&2; exit 2 ;;
  esac
done
WT="$(cd "$(dirname "$0")/.." && pwd)"
MAIN="$(cygpath -u "$(git -C "$WT" worktree list --porcelain | sed -n 's/^worktree //p' | head -n 1)")"
if [ "$MAIN" = "$WT" ]; then
  echo "this is the main checkout: nothing to do" >&2
  exit 1
fi
junction() {  # junction <link> <target>
  [ -e "$1" ] && return 0
  mkdir -p "$(dirname "$1")"

  powershell -NoProfile -Command "New-Item -ItemType Junction -Path '$(cygpath -w "$1")' -Target '$(cygpath -w "$2")'" > /dev/null
  echo "linked $1 -> $2"
}

junction "$WT/refs" "$MAIN/refs"


fill() {  # fill <folder, relative to the checkout, with tracked files below it>
  local rel="$1" e sub
  for e in "$MAIN/$rel"/* "$MAIN/$rel"/.git; do
    [ -e "$e" ] || continue
    sub="$rel/$(basename "$e")"
    if [ -n "$(git -C "$WT" ls-files -- "$sub" | head -n 1)" ]; then
      if [ -d "$e" ] && [ ! -L "$WT/$sub" ]; then fill "$sub"; fi
      continue
    fi
    if [ -d "$e" ]; then junction "$WT/$sub" "$e"; else [ -e "$WT/$sub" ] || cp "$e" "$WT/$sub"; fi
  done
}
for d in "$MAIN"/sdk/thirdparty/*/; do
  d="${d%/}"
  name="$(basename "$d")"
  if [ -n "$(git -C "$WT" ls-files -- "sdk/thirdparty/$name" | head -n 1)" ]; then
    fill "sdk/thirdparty/$name"
  else
    junction "$WT/sdk/thirdparty/$name" "$d"
  fi
done


if [ "$OWN_GEN" = 1 ]; then
  if [ ! -e "$WT/app/generated" ]; then
    cp -r "$MAIN/app/generated" "$WT/app/generated"
    echo "copied app/generated"
  fi
else
  junction "$WT/app/generated" "$MAIN/app/generated"
fi
junction "$WT/work" "$MAIN/work"

[ -s "$MAIN/bo2_data.txt" ] && [ ! -e "$WT/bo2_data.txt" ] && cp "$MAIN/bo2_data.txt" "$WT/bo2_data.txt"


mkdir -p "$WT/app/out/win-$TAG"
if [ ! -e "$WT/app/out/win-$TAG/bo2zm_shaders.spvpack" ]; then
  ln "$MAIN/app/out/win/bo2zm_shaders.spvpack" "$WT/app/out/win-$TAG/bo2zm_shaders.spvpack"
  echo "hard-linked the shader library into app/out/win-$TAG"
fi
echo
echo "ready. In every shell of this worktree: export BO2_BUILD_TAG=$TAG"
