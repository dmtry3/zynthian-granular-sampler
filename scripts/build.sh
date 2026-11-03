#!/bin/bash
set -e
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
B="$ROOT/build/granular_sampler.lv2"
mkdir -p "$B"
cp "$ROOT/lv2/manifest.ttl" "$ROOT/lv2/granular_sampler.ttl" "$B/"
# Pi 5 (Cortex-A76): tuned build; elsewhere (WSL) a generic one, only to check that it compiles.
CPU=""
[ "$(uname -m)" = "aarch64" ] && CPU="-mcpu=cortex-a76"
gcc -O3 $CPU -fPIC -shared -Wall -Wextra $(pkg-config --cflags lv2) \
    -o "$B/granular_sampler.so" "$ROOT/src/granular_sampler.c" -lm
echo "Built: $B"
