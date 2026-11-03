#!/bin/bash
set -e
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
B="$ROOT/build/granular_sampler.lv2"
if ! pkg-config --exists lv2 sndfile; then
    echo "Missing headers: sudo apt install lv2-dev libsndfile1-dev" >&2
    exit 1
fi
mkdir -p "$B"
cp "$ROOT/lv2/manifest.ttl" "$ROOT/lv2/granular_sampler.ttl" "$B/"
# Pi 5 (Cortex-A76): tuned build; elsewhere (WSL) a generic one, only to check that it compiles.
CPU=""
[ "$(uname -m)" = "aarch64" ] && CPU="-mcpu=cortex-a76"
gcc -O3 $CPU -fPIC -shared -Wall -Wextra $(pkg-config --cflags lv2 sndfile) \
    -o "$B/granular_sampler.so" "$ROOT/src/granular_sampler.c" "$ROOT/src/grain_core.c" "$ROOT/src/sample.c" \
    $(pkg-config --libs sndfile) -lm
echo "Built: $B"
