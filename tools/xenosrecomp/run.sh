#!/usr/bin/env bash


set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$HERE/../.."
IN="${1:?blob folder}"
OUT="${2:?output folder}"
TR="$HERE/translator"            # XenosRecomp (MIT, hedge-dev)
TP="$ROOT/sdk/thirdparty"             # fmt + xxHash (fetched by tools/fetch_thirdparty.py)
. "$ROOT/tools/toolchain.sh"  # LLVM and the Vulkan SDK (VULKAN_SDK)
VK="$VULKAN_SDK"
DXC="$VK/Bin/dxc.exe"
VAL="$VK/Bin/spirv-val.exe"
JOBS="${JOBS:-12}"
BO2_SHADER_PROFILE="${BO2_SHADER_PROFILE:-default}"
BO2_PROFILE_CHECKER="$ROOT/tools/check_ubo_spirv.py"
BO2_PROFILE_PYTHON="${PYTHON:-python}"
case "$BO2_SHADER_PROFILE" in
  default|ubo-only) ;;
  *) echo "unknown BO2_SHADER_PROFILE: $BO2_SHADER_PROFILE (expected default or ubo-only)" >&2; exit 1;;
esac

case "$(cd "$OUT" 2>/dev/null && pwd || echo "$OUT")" in
  "$(cd "$ROOT" && pwd)"*) echo "refusing to write game-derived output inside the repo"; exit 1;;
esac
[ -e "$OUT" ] && { echo "output folder exists: $OUT"; exit 1; }
mkdir -p "$OUT/spirv" "$ROOT/work/xenosrecomp"
printf '%s\n' "$BO2_SHADER_PROFILE" > "$OUT/shader-profile.txt"

EXE="$ROOT/work/xenosrecomp/bo2zm_hlsl.exe"
clang++ -std=c++23 -O1 -w \
  "-I$TR" "-I$TP/fmt/include" "-I$TP/xxHash" \
  -include "$TR/pch_min.h" -include "$HERE/assert_throw.h" \
  -DFMT_HEADER_ONLY -DXXH_INLINE_ALL -DBO2ZM_RECOMP -D_CRT_SECURE_NO_WARNINGS \
  "$HERE/bo2zm_hlsl.cpp" "$TR/shader_recompiler.cpp" -o "$EXE" \
  > "$OUT/build.log" 2>&1 || { echo "translator build failed: $OUT/build.log"; tail -20 "$OUT/build.log"; exit 1; }

"$EXE" "$IN" "$OUT/hlsl" "$TR/shader_common.h" | tee "$OUT/translate.log"


compile_one() {
  f="$1"; base=$(basename "$f" .hlsl)
  if [[ "$base" == p_* ]]; then t=ps_6_6; extra=(); else t=vs_6_6; extra=(-fvk-invert-y); fi
  profile_flags=()
  if [[ "$BO2_SHADER_PROFILE" == ubo-only ]]; then profile_flags=(-DBO2ZM_UBO_ONLY=1); fi
  if ! "$DXC" -spirv -T "$t" -E main -HV 2021 -fspv-target-env=vulkan1.2 -fvk-use-dx-layout \
        "${extra[@]}" "${profile_flags[@]}" -Fo "$OUT/spirv/$base.spv" "$f" > "$OUT/spirv/$base.dxc.txt" 2>&1; then
    printf '%s\tdxc\t%s\n' "$base" "$(grep -m1 -E 'error' "$OUT/spirv/$base.dxc.txt" | tr '\t' ' ' | cut -c1-200)"
    return
  fi
  if ! "$VAL" --target-env vulkan1.2 --scalar-block-layout "$OUT/spirv/$base.spv" > "$OUT/spirv/$base.val.txt" 2>&1; then
    printf '%s\tspirv-val\t%s\n' "$base" "$(head -1 "$OUT/spirv/$base.val.txt" | tr '\t' ' ' | cut -c1-200)"
    return
  fi
  if [[ "$BO2_SHADER_PROFILE" == ubo-only ]]; then
    if ! "$BO2_PROFILE_PYTHON" "$BO2_PROFILE_CHECKER" "$OUT/spirv/$base.spv" > "$OUT/spirv/$base.profile.txt" 2>&1; then
      printf '%s\tubo-profile\t%s\n' "$base" "$(tail -1 "$OUT/spirv/$base.profile.txt" | tr '\t' ' ' | cut -c1-200)"
      return
    fi
  fi
  rm -f "$OUT/spirv/$base.dxc.txt" "$OUT/spirv/$base.val.txt"
  printf '%s\tok\t\n' "$base"
}
export -f compile_one; export OUT DXC VAL BO2_SHADER_PROFILE BO2_PROFILE_CHECKER BO2_PROFILE_PYTHON
ls "$OUT"/hlsl/*.hlsl | xargs -P "$JOBS" -I{} bash -c 'compile_one "$@"' _ {} > "$OUT/compile.tsv"

echo "translate: $(grep -c $'\tok\t' "$OUT/hlsl/translate.tsv") ok, $(grep -c $'\tfailed\t' "$OUT/hlsl/translate.tsv") failed, $(grep -c $'\tskipped\t' "$OUT/hlsl/translate.tsv") skipped"
echo "compile:   $(grep -c $'\tok\t' "$OUT/compile.tsv") ok, $(grep -c $'\tdxc\t' "$OUT/compile.tsv") dxc errors, $(grep -c $'\tspirv-val\t' "$OUT/compile.tsv") invalid SPIR-V"
if [[ "$BO2_SHADER_PROFILE" == ubo-only ]]; then
  echo "UBO profile: $(grep -c $'\tubo-profile\t' "$OUT/compile.tsv") modules failed the no-Int64/logical-address checks"
  if grep -q -E $'\t(dxc|spirv-val|ubo-profile)\t' "$OUT/compile.tsv"; then
    echo "UBO shader compilation/validation failed; inspect compile.tsv before packing" >&2
    exit 1
  fi
fi
