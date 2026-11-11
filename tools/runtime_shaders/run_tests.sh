#!/usr/bin/env bash


set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
WORK="${1:?usage: run_tests.sh <work folder> [dxc]}"
DXC="${2:-}"
mkdir -p "$WORK"
"$HERE/build_host.sh" "$WORK/tools"
python3 -I "$HERE/make_fuzz_containers.py" "$WORK/containers" --count 3000 --seed 7
rm -rf "$WORK/runtime"
"$WORK/tools/translate_cli" "$WORK/containers" "$WORK/runtime"


R="$ROOT/app/src/native/render"
INC=(-I"$ROOT/app" -I"$ROOT/app/src" -I"$R" -I"$ROOT/sdk/include" -I"$ROOT/sdk/src" -DSPDLOG_FMT_EXTERNAL
     -DSIMDE_MM_ROUND_MASK=0x6000 -DBO2ZM_RUNTIME_SHADERS=1)
OBJ="$WORK/native"
mkdir -p "$OBJ"
for f in "$R/bo2zm_native_shaders.cpp" "$R/bo2zm_shader_index.cpp" "$R/bo2zm_shader_library.cpp" \
         "$ROOT/sdk/src/core/cvar.cpp" "$ROOT/sdk/src/core/logging.cpp" "$HERE/test_native_runtime.cpp" \
         "$HERE/test_stubs.cpp"; do
  clang++ -std=c++23 -O1 -w "${INC[@]}" -c "$f" -o "$OBJ/$(basename "$f").o"
done
clang++ -o "$OBJ/test_native_runtime" "$OBJ"/*.o "$WORK/tools/translator.o" "$WORK/tools/translator_hlsl.o" \
  "$WORK/tools/shader_recompiler.o" -lglslang -lglslang-default-resource-limits -lSPIRV -lSPIRV-Tools-opt \
  -lSPIRV-Tools -lspdlog -lfmt -lpthread
rm -rf "$OBJ/run" && mkdir -p "$OBJ/run"
BO2_TEST_DIR="$OBJ/run" "$OBJ/test_native_runtime" "$WORK/containers" | grep -vE '^\[' | tail -3

if [ -n "$DXC" ]; then


  mkdir -p "$WORK/pc"
  "$ROOT/tools/xenosrecomp/build_tools.sh" "$WORK/pc" >/dev/null 2>&1 || {
    clang++ -std=c++23 -O1 -w -I"$WORK/tools/gen" -I"$ROOT/tools/xenosrecomp/translator" \
      -include "$ROOT/tools/xenosrecomp/translator/pch_min.h" -include "$ROOT/tools/xenosrecomp/assert_throw.h" \
      -DXXH_INLINE_ALL -DBO2ZM_RECOMP "$ROOT/tools/xenosrecomp/bo2zm_hlsl.cpp" \
      "$ROOT/tools/xenosrecomp/translator/shader_recompiler.cpp" -o "$WORK/pc/bo2zm_hlsl.exe"; }
  rm -rf "$WORK/hlsl" && "$WORK/pc/bo2zm_hlsl.exe" "$WORK/containers" "$WORK/hlsl" \
    "$ROOT/tools/xenosrecomp/translator/shader_common.h" >/dev/null
  : > "$WORK/pack_list.txt"
  for h in "$WORK"/hlsl/*.hlsl; do
    base="$(basename "$h" .hlsl)"; name="${base#?_}"
    case "$base" in p_*) target=ps_6_6; flip=();; *) target=vs_6_6; flip=(-fvk-invert-y);; esac
    "$DXC" -spirv -T "$target" -E main -HV 2021 -fspv-target-env=vulkan1.2 -fvk-use-dx-layout -DBO2ZM_UBO_ONLY=1 \
      "${flip[@]}" -Fo "$WORK/hlsl/$base.spv" "$h" >/dev/null 2>&1 &&
      echo "$WORK/containers/$name.bin $WORK/hlsl/$base.spv" >> "$WORK/pack_list.txt" || true
  done
  clang++ -std=c++23 -O1 -include cstring -I"$R" "$HERE/make_pack.cpp" "$R/bo2zm_shader_library.cpp" \
    -o "$WORK/tools/make_pack"
  "$WORK/tools/make_pack" "$WORK/dxc.spvpack" "$WORK/pack_list.txt"
  "$HERE/compare_with_library.sh" "$WORK/dxc.spvpack" "$WORK/compare"
fi
