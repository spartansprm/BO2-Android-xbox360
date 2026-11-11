#!/usr/bin/env bash


ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
if [ -n "${BO2_DATA:-}" ]; then
  echo "$BO2_DATA"
elif [ -s "$ROOT/bo2_data.txt" ]; then
  tr -d '\r\n' < "$ROOT/bo2_data.txt"
  echo
else
  echo "$ROOT/data"
fi
