#!/bin/sh


here="$(cd "$(dirname "$0")" && pwd)"
cd "$here" || exit 1

missing=""
[ -f game/default_mp.xex ] || missing="$missing game/default_mp.xex"
[ -d update ] || missing="$missing update/"
[ -f bo2zm_shaders.spvpack ] || missing="$missing bo2zm_shaders.spvpack"
if [ -n "$missing" ]; then
  echo "bo2zm: missing next to this script:$missing (see README.txt)" >&2
  exit 1
fi
if ! ldconfig -p 2>/dev/null | grep -q 'libvulkan\.so\.1'; then
  echo "bo2zm: libvulkan.so.1 not found: install your distribution's Vulkan loader (e.g. libvulkan1) and driver" >&2
fi


renderer="--bo2zm_renderer=native"
own_user=""
for a in "$@"; do
  case "$a" in
    --bo2zm_renderer=*) renderer="" ;;
    --user_data_root=*) own_user=1 ;;
  esac
done


if [ -d user ] && [ -z "$own_user" ]; then
  set -- "--user_data_root=$here/user" "$@"
fi
exec ./bo2zm $renderer "$@"
