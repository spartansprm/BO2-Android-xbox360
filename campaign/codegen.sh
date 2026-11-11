#!/usr/bin/env bash


set -euo pipefail
CP="$(cd "$(dirname "$0")" && pwd)"
ROOT="$CP/.."
BIN="${REXGLUE_BIN:-$ROOT/sdk/out/win-amd64}"
export PATH="$BIN:$PATH"
PY="${PYTHON:-python}"
GEN=campaign/generated/default
CODEGEN="${BO2_CODEGEN:-rexglue.exe}"
(cd "$CP" && "$CODEGEN" codegen --ignore-stamp bo2cp_manifest.toml > codegen.log 2>&1) || {
  echo "codegen failed, see campaign/codegen.log" >&2
  exit 1
}
tail -n 2 "$CP/codegen.log"
cd "$ROOT"
"$PY" tools/codegen_gaps.py --gen "$GEN" --prefix bo2cp --image work/dump_tu18/default.image.bin \
  --text 82100000:8270D8B4 --image-size 38731776 --toml campaign/gaps.toml --report campaign/codegen_gaps.txt --overrides campaign/overrides.toml
"$PY" -I tools/read_before_write.py --gen "$GEN" > campaign/codegen_read_before_write.log
head -n 1 campaign/codegen_read_before_write.log
"$PY" -I tools/setjmp_locals.py --gen "$GEN" --fix
"$PY" -I tools/direct_calls.py --gen "$GEN"
"$PY" -I tools/guest_barriers.py --gen "$GEN"
