#!/usr/bin/env bash

GEN="${2:-$(cd "$(dirname "$0")/../.." && pwd)/app/generated/default_mp}"
awk -v f="DEFINE_REX_FUNC($1)" 'index($0,f)==1{p=1} p{print} p&&/^}/{exit}' "$GEN"/bo2zm_recomp.*.cpp
