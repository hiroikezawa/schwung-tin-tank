#!/usr/bin/env bash
# Offline listening renders and impulse responses (build/demo/*.wav).
#
# Copyright (c) 2026 Hiro Ikezawa. MIT License (see LICENSE).
set -euo pipefail
MODULE="$(cd "$(dirname "$0")/.." && pwd)"
OUT="$MODULE/build/demo"
mkdir -p "$OUT"
bash "$MODULE/scripts/build.sh" native >/dev/null
"${CC:-gcc}" -std=gnu11 -O2 -ffp-contract=off -Wall -Wextra -Wno-unused-function \
  -I"$MODULE/dsp" -I"$MODULE/tests" -I"$MODULE/build/generated" \
  "$MODULE/tests/render_demo.c" "$MODULE/dsp/dv_engine.c" -lm -o "$MODULE/build/native/render-demo"
"$MODULE/build/native/render-demo" "$OUT"
