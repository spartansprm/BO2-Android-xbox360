#!/usr/bin/env bash


set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
if [ -L "$ROOT/app/generated" ]; then
  echo "app/generated is a link to another checkout's generated code: run tools/bootstrap_worktree.sh <tag> --own-generated" >&2
  echo "(after removing the link), or run this in the checkout that owns it" >&2
  exit 1
fi
SDK_BIN="$ROOT/sdk/out/win-amd64"
export PATH="$SDK_BIN:$PATH"
PY="${PYTHON:-python}"
CODEGEN="${BO2_CODEGEN:-rexglue.exe}"

echo "== codegen"
(cd "$ROOT/app" && "$CODEGEN" codegen --ignore-stamp bo2zm_manifest.toml > codegen.log 2>&1) || {
  echo "codegen failed, see app/codegen.log" >&2
  exit 1
}
tail -n 2 "$ROOT/app/codegen.log"
echo "== registers read before written"
"$PY" "$ROOT/tools/read_before_write.py" --strict > "$ROOT/app/codegen_read_before_write.log"
echo "== setjmp sites"
"$PY" "$ROOT/tools/setjmp_locals.py" --fix
echo "== direct calls"
"$PY" "$ROOT/tools/direct_calls.py"
echo "== local copies of hot leaf functions (app/locals.txt)"
"$PY" "$ROOT/tools/localize.py"
echo "== guest memory barriers"
"$PY" "$ROOT/tools/guest_barriers.py"
echo "== stats"
"$PY" "$ROOT/tools/codegen_stats.py"
