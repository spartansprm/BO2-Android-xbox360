#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only
set -euo pipefail
PROJECT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
exec "${PYTHON:-python3}" "$PROJECT_DIR/tools/build_android.py" --program campaign --stage native "$@"
