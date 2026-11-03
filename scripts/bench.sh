#!/bin/bash
# Builds and runs grain_bench with the plugin's flags. Only results on the Pi 5 count.
#   ./scripts/bench.sh [sample file] [--wav out]
#   POOL=128 ./scripts/bench.sh ...      the same with another grain pool size (to find the limit)
set -e
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
CPU=""
[ "$(uname -m)" = "aarch64" ] && CPU="-mcpu=cortex-a76"
mkdir -p "$ROOT/build_test"
POOL="${POOL:-64}"
gcc -O3 $CPU -DGC_POOL=$POOL -Wall -Wextra $(pkg-config --cflags sndfile) -o "$ROOT/build_test/grain_bench" \
    "$ROOT/test/grain_bench.c" "$ROOT/src/grain_core.c" "$ROOT/src/sample.c" $(pkg-config --libs sndfile) -lm
[ "$(uname -m)" = "aarch64" ] || echo "Note: not on the Pi, these numbers do not count."
"$ROOT/build_test/grain_bench" "$@"
