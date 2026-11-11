#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BIN="${TMPDIR:-/tmp}/eg53ma4_test"

g++ -std=c++17 -O2 -Wall -Wextra \
  -I "$ROOT/src" \
  "$ROOT/src/decode_eg53ma4.cpp" \
  "$ROOT/test/test_decode.cpp" \
  -o "$BIN"

"$BIN"

cd "$ROOT/python"
python3 -m unittest -v test_decode.py
