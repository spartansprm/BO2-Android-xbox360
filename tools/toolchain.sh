#!/usr/bin/env bash


_bo2_unix() { cygpath -u "$1" 2>/dev/null || echo "$1"; }
_bo2_mixed() { cygpath -m "$1" 2>/dev/null || echo "$1"; }


if [ -n "${BO2_CMAKE_DIR:-}" ]; then
  export PATH="$(_bo2_unix "$BO2_CMAKE_DIR"):$PATH"
elif [ -d "/c/Program Files/CMake/bin" ]; then
  export PATH="/c/Program Files/CMake/bin:$PATH"
fi


NINJA_EXE=""
if [ -n "${BO2_NINJA:-}" ]; then
  NINJA_EXE="$(_bo2_mixed "$BO2_NINJA")"
else
  for _bo2_n in "$(_bo2_unix "${LOCALAPPDATA:-}")"/Microsoft/WinGet/Packages/Ninja-build.Ninja_*/ninja.exe; do
    [ -f "$_bo2_n" ] && NINJA_EXE="$(_bo2_mixed "$_bo2_n")"
  done
  if [ -z "$NINJA_EXE" ] && command -v ninja > /dev/null 2>&1; then
    NINJA_EXE="$(_bo2_mixed "$(command -v ninja)")"
  fi
fi
if [ -n "$NINJA_EXE" ]; then
  export PATH="$(dirname "$(_bo2_unix "$NINJA_EXE")"):$PATH"
fi


if [ -n "${BO2_LLVM_DIR:-}" ]; then
  export PATH="$(_bo2_unix "$BO2_LLVM_DIR"):$PATH"
elif [ -d "/c/Program Files/LLVM/bin" ]; then
  export PATH="/c/Program Files/LLVM/bin:$PATH"
fi


if [ -n "${BO2_DEVKITPRO:-}" ]; then
  DEVKITPRO="$(_bo2_mixed "$BO2_DEVKITPRO")"
elif [[ "${DEVKITPRO:-}" =~ ^[A-Za-z]:[/\\] ]] && [ -d "$(_bo2_unix "$DEVKITPRO")/devkitA64" ]; then
  DEVKITPRO="$(_bo2_mixed "$DEVKITPRO")"
else
  DEVKITPRO="C:/devkitPro"
fi
export DEVKITPRO


if [ -z "${VULKAN_SDK:-}" ]; then
  for _bo2_v in /c/VulkanSDK/*/; do
    [ -d "$_bo2_v" ] && VULKAN_SDK="${_bo2_v%/}"
  done
fi
VULKAN_SDK="${VULKAN_SDK:+$(_bo2_unix "$VULKAN_SDK")}"


_bo2_data="$(bash "$(dirname "${BASH_SOURCE[0]}")/data_dir.sh")"
MESA_SDK="${MESA_SDK:-$(_bo2_unix "$_bo2_data")/mesa/mesa-unified-install/opt/devkitpro/portlibs/switch}"
BO2_SPECS_DIR="$(_bo2_mixed "${BO2_SPECS_DIR:-$_bo2_data/specs}")"
BO2_MSYS2_DIR="$(_bo2_unix "${BO2_MSYS2_DIR:-C:/msys64}")"
unset _bo2_n _bo2_v _bo2_data
