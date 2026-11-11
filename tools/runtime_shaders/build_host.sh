#!/usr/bin/env bash


set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
OUT="${1:?usage: build_host.sh <output folder>}"
mkdir -p "$OUT/gen/fmt"
python3 -I "$HERE/make_shader_common_text.py" "$ROOT/tools/xenosrecomp/translator/shader_common.h" \
  "$OUT/gen/bo2zm_shader_common_text.h"
if [ -z "${FMT_INCLUDE:-}" ]; then
  cat > "$OUT/gen/fmt/core.h" <<'EOF'
#pragma once
// Host-test shim: the fmt >= 10 subset XenosRecomp uses, on C++23 <format>.
#include <format>
namespace fmt {
using std::format; using std::format_string; using std::make_format_args; using std::vformat_to;
}
EOF
  FMT_INCLUDE="$OUT/gen"
fi
R="$ROOT/app/src/native/render"
CXX="${CXX:-clang++}"
"$CXX" -std=c++23 -O2 -w -I"$OUT/gen" -I"$FMT_INCLUDE" -I"$ROOT/tools/xenosrecomp/translator" \
  -include "$R/bo2zm_xenosrecomp_prelude.h" -DBO2ZM_RECOMP -c "$ROOT/tools/xenosrecomp/translator/shader_recompiler.cpp" \
  -o "$OUT/shader_recompiler.o"
"$CXX" -std=c++23 -O2 -w -I"$OUT/gen" -I"$FMT_INCLUDE" -I"$ROOT/tools/xenosrecomp/translator" -I"$R" \
  -DBO2ZM_RECOMP -c "$R/bo2zm_shader_translator_hlsl.cpp" -o "$OUT/translator_hlsl.o"
"$CXX" -std=c++23 -O2 -Wall -I"$R" -c "$R/bo2zm_shader_translator.cpp" -o "$OUT/translator.o"
"$CXX" -std=c++23 -O2 -Wall -I"$R" -c "$HERE/translate_cli.cpp" -o "$OUT/translate_cli.o"
"$CXX" -o "$OUT/translate_cli" "$OUT/translate_cli.o" "$OUT/translator.o" "$OUT/translator_hlsl.o" \
  "$OUT/shader_recompiler.o" -lglslang -lglslang-default-resource-limits -lSPIRV -lSPIRV-Tools-opt -lSPIRV-Tools \
  -lpthread
echo "runtime translator test driver: $OUT/translate_cli"
