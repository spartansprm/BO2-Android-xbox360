#!/usr/bin/env bash
set -euo pipefail
PROJECT_DIR="$(cd "$(dirname "$0")" && pwd)"
exec "${PYTHON:-python3}" "$PROJECT_DIR/tools/build_android.py" "$@"
