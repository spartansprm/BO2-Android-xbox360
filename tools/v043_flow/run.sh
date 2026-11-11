#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-only


set -e
dir=$(dirname "$0")
python3 "$dir/extract.py" .
g++ -std=c++23 -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=all -Wall -Wextra -Wno-unused-parameter \
  -Iapp/src/native/render -I"$dir" "$dir/harness.cpp" -o /tmp/test-v043-flow
/tmp/test-v043-flow
