/* Tests of grain_core (no LV2). */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../src/grain_core.h"

static int fails = 0;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

#define SR 48000
#define BUF 256
static GrainCore G;
static float L[BUF], R[BUF];

static float* make_ramp(int64_t n) {          /* value = index / n: the read position can be seen in the output */
  float* d = malloc(n * sizeof(float));
  for (int64_t i = 0; i < n; i++) d[i] = (float)i / (float)n;
  return d;
}
static float* make_const(int64_t n, float v) {
  float* d = malloc(n * sizeof(float));
  for (int64_t i = 0; i < n; i++) d[i] = v;
  return d;
}

/* Runs `secs` seconds; returns the peak, counts non-finite samples. */
static float run(const GcParams* p, double secs, int* bad) {
  float peak = 0;
  for (long k = 0; k < (long)(secs * SR / BUF); k++) {
    gc_process(&G, p, L, R, BUF);
    for (int i = 0; i < BUF; i++) {
      if (!isfinite(L[i]) || !isfinite(R[i])) { if (bad) (*bad)++; continue; }
      if (fabsf(L[i]) > peak) peak = fabsf(L[i]);
      if (fabsf(R[i]) > peak) peak = fabsf(R[i]);
    }
  }
  return peak;
}

static void test_windows(void) {
  gc_init(&G, SR, 1);
  for (int s = 0; s < GC_SHAPES; s++) {
    CHECK(G.win[s][0] == 0.0f && G.win[s][GC_WIN] == 0.0f);
    CHECK(fabsf(G.win[s][GC_WIN / 2] - 1.0f) < 1e-3f);
    for (int i = 0; i <= GC_WIN; i++) CHECK(G.win[s][i] >= 0.0f && G.win[s][i] <= 1.0f + 1e-6f);
  }
  double a0 = 0, a16 = 0, a32 = 0;
  for (int i = 0; i <= GC_WIN; i++) { a0 += G.win[0][i]; a16 += G.win[16][i]; a32 += G.win[32][i]; }
  CHECK(a0 < a16 && a16 < a32);                     /* narrow Gauss < Hann < almost rectangular */
  CHECK(fabs(a16 / GC_WIN - 0.5) < 0.01);           /* Hann area */
}

static void test_silence(void) {
  GcParams p; gc_default_params(&p);
  gc_init(&G, SR, 1);
  L[0] = R[0] = 1.0f;
  gc_process(&G, &p, L, R, BUF);
  CHECK(L[0] == 0.0f && R[0] == 0.0f);              /* no sample: writes silence */
  float* d = make_const(SR, 0.5f);
  gc_set_source(&G, d, d, SR, SR);
  CHECK(run(&p, 0.1, NULL) == 0.0f);                /* no note */
  gc_note_on(&G, &p, 60, 127);
  gc_set_source(&G, NULL, NULL, 0, 0);
  CHECK(run(&p, 0.1, NULL) == 0.0f && G.nactive == 0);   /* sample removed while playing */
  free(d);
}

static void test_note_cycle(void) {
  GcParams p; gc_default_params(&p);
  p.size_ms = 50; p.density = 40;
  gc_init(&G, SR, 1);
  float* d = make_const(SR, 0.5f);
  gc_set_source(&G, d, d, SR, SR);
  gc_note_on(&G, &p, 60, 127);
  gc_process(&G, &p, L, R, BUF);
  CHECK(G.born == 1 && L[0] == 0.0f && L[100] > 0.0f);  /* first grain starts at once (window starts at 0) */
  int bad = 0;
  float peak = run(&p, 1.0, &bad);
  CHECK(bad == 0 && peak > 0.2f && peak <= 0.5f + 1e-4f);
  CHECK(G.born >= 40 && G.born <= 42);              /* density 40/s, chaos 0 */
  gc_note_off(&G, 60);
  uint64_t born = G.born;
  run(&p, 0.06, NULL);                              /* longer than one grain */
  CHECK(G.born == born && G.nactive == 0 && G.voice.note == -1);
  CHECK(run(&p, 0.05, NULL) == 0.0f);
  gc_note_on(&G, &p, 60, 127);
  gc_note_off(&G, 61);                              /* another key: the voice keeps playing */
  run(&p, 0.1, NULL);
  CHECK(G.voice.gate == 1 && G.nactive > 0);
  gc_all_off(&G);
  CHECK(G.nactive == 0 && run(&p, 0.05, NULL) == 0.0f);
  free(d);
}

static void test_pool(void) {
  GcParams p; gc_default_params(&p);
  p.size_ms = 1000; p.density = 200;                /* 200 overlapping grains wanted */
  gc_init(&G, SR, 1);
  float* d = make_const(SR * 4, 0.5f);
  gc_set_source(&G, d, d, SR * 4, SR);
  gc_note_on(&G, &p, 60, 127);
  int bad = 0;
  float peak = run(&p, 2.0, &bad);
  CHECK(G.nactive <= GC_POOL && G.skipped > 0 && bad == 0);
  CHECK(peak < 0.5f * GC_POOL / sqrtf(200.0f) + 0.01f);   /* gain is 1/sqrt(overlap) per grain */
  free(d);
}

static void test_chaos(void) {
  GcParams p; gc_default_params(&p);
  p.size_ms = 10; p.density = 50; p.chaos = 1.0f;
  gc_init(&G, SR, 7);
  float* d = make_const(SR, 0.5f);
  gc_set_source(&G, d, d, SR, SR);
  gc_note_on(&G, &p, 60, 127);
  run(&p, 20.0, NULL);
  CHECK(G.born > 900 && G.born < 1100);             /* about 50/s on average */
  free(d);
}

/* Position and Scan, seen through a ramp sample: output / window value = read position / frames. */
static void test_position_scan(void) {
  GcParams p; gc_default_params(&p);
  const int64_t n = SR * 10;
  float* d = make_ramp(n);
  gc_init(&G, SR, 1);
  gc_set_source(&G, d, d, n, SR);

  p.position = 0.5f; p.scan = 0.0f; p.size_ms = 20; p.density = 10; p.shape = 1.0f;
  gc_note_on(&G, &p, 60, 127);
  run(&p, 2.0, NULL);
  CHECK(fabs(G.voice.playhead - 0.5 * (n - 1)) < 1.0);   /* freeze */
  CHECK(G.grains[0].idx >= n / 2 && G.grains[0].idx < n / 2 + SR / 50 + 2);

  p.scan = 0.01f;                                   /* inside the dead zone: still frozen */
  run(&p, 1.0, NULL);
  CHECK(fabs(G.voice.playhead - 0.5 * (n - 1)) < 1.0);

  p.scan = 1.0f;                                    /* realtime: one second moves 48000 frames */
  run(&p, 1.0, NULL);
  CHECK(fabs(G.voice.playhead - (0.5 * (n - 1) + SR)) < 2 * BUF);

  p.scan = -2.0f;                                   /* backwards, twice as fast */
  double ph = G.voice.playhead;
  run(&p, 0.5, NULL);
  CHECK(fabs(G.voice.playhead - (ph - SR)) < 2 * BUF);

  /* window: the playhead loops inside [position, position + window) */
  p.position = 0.2f; p.window = 0.1f; p.scan = 2.0f;
  gc_note_on(&G, &p, 60, 127);
  CHECK(fabs(G.voice.playhead - 0.2 * (n - 1)) < 1.0);   /* new note starts at Position */
  int inside = 1;
  for (int k = 0; k < 2000; k++) {
    gc_process(&G, &p, L, R, BUF);
    if (G.voice.playhead < 0.2 * (n - 1) - 1 || G.voice.playhead > 0.2 * (n - 1) + 0.1 * n + 1) inside = 0;
  }
  CHECK(inside);

  /* spray: starts spread around the playhead, within half the window each side */
  p.scan = 0.0f; p.spray = 1.0f; p.size_ms = 5; p.density = 200;
  gc_note_on(&G, &p, 60, 127);
  double lo = n, hi = 0;
  for (int k = 0; k < 2000; k++) {                  /* one block of 32: at most one grain per call */
    gc_process(&G, &p, L, R, GC_BLOCK);
    if (G.last_start < lo) lo = G.last_start;
    if (G.last_start > hi) hi = G.last_start;
  }
  const double c = 0.2 * (n - 1), half = 0.05 * n;
  CHECK(lo >= c - half - 1 && hi <= c + half + 1);
  CHECK(hi - lo > half);                            /* really spread */
  free(d);
}

static void test_pitch_and_rate(void) {
  GcParams p; gc_default_params(&p);
  float* d = make_const(SR, 0.5f);
  gc_init(&G, SR, 1);
  gc_set_source(&G, d, d, SR, 44100);               /* 44.1 kHz file at 48 kHz */
  gc_note_on(&G, &p, 72, 127);
  gc_process(&G, &p, L, R, BUF);
  CHECK(fabsf(G.grains[0].inc - 2.0f * 44100.0f / 48000.0f) < 1e-5f);
  gc_note_on(&G, &p, 48, 127);
  gc_process(&G, &p, L, R, BUF);
  int found = 0;
  for (int i = 0; i < GC_POOL; i++)
    if (G.grains[i].active && fabsf(G.grains[i].inc - 0.5f * 44100.0f / 48000.0f) < 1e-5f) found = 1;
  CHECK(found);
  free(d);
}

/* Reading across the end of the sample wraps to its start without clicks or out-of-range reads. */
static void test_wrap(void) {
  GcParams p; gc_default_params(&p);
  const int64_t n = 1000;
  float* d = make_const(n, 0.5f);
  gc_init(&G, SR, 1);
  gc_set_source(&G, d, d, n, SR);
  p.position = 0.99f; p.scan = 0.0f; p.size_ms = 100; p.density = 5; p.shape = 1.0f;
  gc_note_on(&G, &p, 84, 127);                      /* 4x speed: the grain crosses the end many times */
  int bad = 0;
  float peak = run(&p, 0.5, &bad);
  CHECK(bad == 0 && peak > 0.49f && peak <= 0.5f + 1e-5f);
  free(d);
}

int main(void) {
  test_windows();
  test_silence();
  test_note_cycle();
  test_pool();
  test_chaos();
  test_position_scan();
  test_pitch_and_rate();
  test_wrap();
  printf(fails ? "%d FAILED\n" : "All grain tests passed\n", fails);
  return fails != 0;
}
