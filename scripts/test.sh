#!/bin/bash
# Builds and runs the tests: sample loading and grain_core without LV2, a random stress test of grain_core,
# then the built plugin in a tiny test host, and every factory preset.
#   ./scripts/test.sh
#   SAN=1 ./scripts/test.sh          the tests with AddressSanitizer and UBSan (slower; not the plugin .so itself)
#   STRESS=600 ./scripts/test.sh     seconds of audio per stress seed (default 30)
set -e
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
"$ROOT/scripts/build.sh"
mkdir -p "$ROOT/build_test"
OPT="-O2"
[ -n "$SAN" ] && OPT="-O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=all"
gcc $OPT -Wall -Wextra $(pkg-config --cflags sndfile) -o "$ROOT/build_test/test_sample" \
    "$ROOT/test/test_sample.c" "$ROOT/src/sample.c" $(pkg-config --libs sndfile) -lm
gcc $OPT -Wall -Wextra $(pkg-config --cflags sndfile) -o "$ROOT/build_test/test_grain" \
    "$ROOT/test/test_grain.c" "$ROOT/src/grain_core.c" "$ROOT/src/sample.c" $(pkg-config --libs sndfile) -lm
gcc $OPT -Wall -Wextra $(pkg-config --cflags sndfile) -o "$ROOT/build_test/test_stress" \
    "$ROOT/test/test_stress.c" "$ROOT/src/grain_core.c" "$ROOT/src/sample.c" $(pkg-config --libs sndfile) -lm
gcc $OPT -Wall -Wextra $(pkg-config --cflags lv2) -o "$ROOT/build_test/test_plugin" \
    "$ROOT/test/test_plugin.c" -ldl -lm
gcc $OPT -Wall -Wextra $(pkg-config --cflags lv2 sndfile) -o "$ROOT/build_test/preset_check" \
    "$ROOT/test/preset_check.c" -ldl -lm $(pkg-config --libs sndfile)
cd "$ROOT/build_test"
./test_sample
./test_grain
./test_stress "${STRESS:-30}"
./test_plugin "$ROOT/build/granular_sampler.lv2/granular_sampler.so"
# every factory preset loads its sample and plays; renders in build_test/presets/ to listen to
mkdir -p "$ROOT/build_test/presets"
./preset_check "$ROOT/build/granular_sampler.lv2/granular_sampler.so" "$ROOT/build/granular_sampler.lv2/presets.ttl" \
    "$ROOT/samples" "$ROOT/build_test/presets"
