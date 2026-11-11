#!/usr/bin/env bash


set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
PACK="${1:?usage: compare_with_library.sh <pack> <work folder> [step]}"
WORK="${2:?usage: compare_with_library.sh <pack> <work folder> [step]}"
STEP="${3:-1}"
case "$(mkdir -p "$WORK" && cd "$WORK" && pwd)" in
  "$ROOT"*) echo "refusing to write game-derived files inside the repository"; exit 1;;
esac
TOOLS="$WORK/tools"
"$HERE/build_host.sh" "$TOOLS" >/dev/null
R="$ROOT/app/src/native/render"
clang++ -std=c++23 -O2 -I"$R" -I"${XXHASH_INCLUDE:-/usr/include}" "$HERE/dump_library.cpp" "$R/bo2zm_shader_library.cpp" \
  -o "$TOOLS/dump_library"
clang++ -std=c++20 -O2 "$HERE/compare_pixels.cpp" -lvulkan -o "$TOOLS/compare_pixels"
glslangValidator -V "$HERE/fullscreen.vert" -o "$TOOLS/fullscreen.vert.spv" >/dev/null

rm -rf "$WORK/shaders" "$WORK/runtime"
"$TOOLS/dump_library" "$PACK" "$WORK/shaders" "$STEP"
"$TOOLS/translate_cli" "$WORK/shaders" "$WORK/runtime" | tee "$WORK/translate.txt"

: > "$WORK/pairs_all.txt"
: > "$WORK/pairs_ps.txt"
while IFS=$'\t' read -r name stage status _; do
  [ "$name" = name ] && continue
  [ "$status" = ok ] || continue
  echo "$name $WORK/shaders/$name.lib.spv $WORK/runtime/$name.spv" >> "$WORK/pairs_all.txt"
  [ "$stage" = ps ] && echo "$name $WORK/shaders/$name.lib.spv $WORK/runtime/$name.spv" >> "$WORK/pairs_ps.txt"
done < "$WORK/runtime/runtime.tsv"

{
  echo "== translation"
  tail -1 "$WORK/translate.txt"
  awk -F'\t' 'NR>1 && $3!="ok"{print "  failed: " substr($6,1,120)}' "$WORK/runtime/runtime.tsv" | sort | uniq -c | sort -rn | head -20
  echo "== interface (library DXC module = a, runtime module = b)"
  python3 "$HERE/spirv_interface.py" --pairs "$WORK/pairs_all.txt"
  echo "== pixels (pixel shaders, lavapipe)"
  VK_ICD_FILENAMES="${VK_ICD_FILENAMES:-/usr/share/vulkan/icd.d/lvp_icd.json}" \
    "$TOOLS/compare_pixels" "$TOOLS/fullscreen.vert.spv" "$WORK/pairs_ps.txt" > "$WORK/pixels.tsv" 2> "$WORK/pixels.txt" || true
  cat "$WORK/pixels.txt"
  awk -F'\t' '$2=="different"' "$WORK/pixels.tsv" | head -20
  echo "== vertex outputs (vertex shaders, transform feedback on lavapipe)"
  python3 -I "$HERE/vertex_pairs.py" "$WORK/pairs_all.txt" "$WORK/vertex" | sed 's/ -$//' > "$WORK/pairs_vs.txt"
  VK_ICD_FILENAMES="${VK_ICD_FILENAMES:-/usr/share/vulkan/icd.d/lvp_icd.json}" \
    "$TOOLS/compare_pixels" "$TOOLS/fullscreen.vert.spv" "$WORK/pairs_vs.txt" > "$WORK/vertex.tsv" 2> "$WORK/vertex.txt" || true
  cat "$WORK/vertex.txt"
  awk -F'\t' '$2=="different"' "$WORK/vertex.tsv" | head -20
} | tee "$WORK/summary.txt"
