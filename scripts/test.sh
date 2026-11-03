#!/bin/bash
# Builds and runs the tests: sample loading without LV2, then the built plugin in a tiny test host.
set -e
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
"$ROOT/scripts/build.sh"
mkdir -p "$ROOT/build_test"
gcc -O2 -Wall -Wextra $(pkg-config --cflags sndfile) -o "$ROOT/build_test/test_sample" \
    "$ROOT/test/test_sample.c" "$ROOT/src/sample.c" $(pkg-config --libs sndfile) -lm
gcc -O2 -Wall -Wextra $(pkg-config --cflags lv2) -o "$ROOT/build_test/test_plugin" \
    "$ROOT/test/test_plugin.c" -ldl
cd "$ROOT/build_test"
./test_sample
./test_plugin "$ROOT/build/granular_sampler.lv2/granular_sampler.so"
