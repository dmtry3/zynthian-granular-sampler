/* Tests of grain_core (no LV2). Sources are built like sample.c builds them: interleaved, guard frames. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../src/grain_core.h"
#include "../src/sample.h"

static int fails = 0;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

#define SR 48000
#define BUF 256
static GrainCore G;
static float L[BUF], R[BUF];

/* ---------- sources ---------- */
typedef struct { float* lv[GC_LEVELS]; GcSource src; } Src;

static void guard(float* d, int64_t n) {
  for (int i = 0; i < GS_GUARD; i++) { d[2 * (n + i)] = d[2 * (i % n)]; d[2 * (n + i) + 1] = d[2 * (i % n) + 1]; }
}
/* f(i) for both channels; `levels` > 1 builds band-limited levels with the loader's filter. */
static Src make_src(int64_t n, float rate, int levels, float (*f)(int64_t, int64_t)) {
  Src s;
  memset(&s, 0, sizeof s);
  s.lv[0] = malloc((n + GS_GUARD) * 2 * sizeof(float));
  for (int64_t i = 0; i < n; i++) s.lv[0][2 * i] = s.lv[0][2 * i + 1] = f(i, n);
  guard(s.lv[0], n);
  s.src.data[0] = s.lv[0]; s.src.frames[0] = n; s.src.nlevels = 1; s.src.rate = rate;
  for (int k = 1; k < levels; k++) {
    const int64_t m = s.src.frames[k - 1] / 2;
    s.lv[k] = malloc((m + GS_GUARD) * 2 * sizeof(float));
    gs_decimate2(s.lv[k - 1], s.src.frames[k - 1], s.lv[k]);
    guard(s.lv[k], m);
    s.src.data[k] = s.lv[k]; s.src.frames[k] = m; s.src.nlevels = k + 1;
  }
  return s;
}
static void free_src(Src* s) { for (int k = 0; k < GC_LEVELS; k++) free(s->lv[k]); }
static float f_half(int64_t i, int64_t n) { (void)i; (void)n; return 0.5f; }
static float f_ramp(int64_t i, int64_t n) { return (float)i / (float)n; }
static float f_hi_sine(int64_t i, int64_t n) { (void)n; return 0.5f * (float)sin(2 * M_PI * 0.4 * i); }   /* 19.2 kHz */
static float f_sine(int64_t i, int64_t n) { (void)n; return 0.5f * (float)sin(2 * M_PI * 0.01 * i); }

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
static double run_rms(const GcParams* p, double secs) {
  double e = 0; long cnt = 0;
  for (long k = 0; k < (long)(secs * SR / BUF); k++) {
    gc_process(&G, p, L, R, BUF);
    for (int i = 0; i < BUF; i++, cnt++) e += (double)L[i] * L[i];
  }
  return sqrt(e / cnt);
}

/* ---------- tests ---------- */
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
  CHECK(G.win_gain[16] == 1.0f && G.win_gain[0] > 1.2f && G.win_gain[32] < 0.9f);   /* loudness compensation */
}

static void test_silence(void) {
  GcParams p; gc_default_params(&p);
  gc_init(&G, SR, 1);
  L[0] = R[0] = 1.0f;
  gc_process(&G, &p, L, R, BUF);
  CHECK(L[0] == 0.0f && R[0] == 0.0f);              /* no sample: writes silence */
  gc_note_on(&G, &p, 60, 127);                      /* no sample: nothing happens */
  CHECK(run(&p, 0.05, NULL) == 0.0f && G.born == 0);
  Src s = make_src(SR, SR, 1, f_half);
  gc_set_source(&G, &s.src);
  gc_note_off(&G, 60);
  CHECK(run(&p, 0.1, NULL) == 0.0f);                /* no note */
  gc_note_on(&G, &p, 60, 127);
  run(&p, 0.05, NULL);
  gc_set_source(&G, NULL);                          /* sample removed while playing: grains fade out */
  run(&p, 0.01, NULL);
  CHECK(G.nactive == 0 && run(&p, 0.05, NULL) == 0.0f);
  free_src(&s);
}

static void test_note_cycle(void) {
  GcParams p; gc_default_params(&p);
  p.size_ms = 50; p.density = 40;
  gc_init(&G, SR, 1);
  Src s = make_src(SR, SR, 1, f_half);
  gc_set_source(&G, &s.src);
  gc_note_on(&G, &p, 60, 127);
  gc_process(&G, &p, L, R, BUF);
  CHECK(G.born == 1 && L[0] == 0.0f && L[100] > 0.0f);  /* first grain starts at once */
  int bad = 0;
  float peak = run(&p, 1.0, &bad);
  /* two Hann grains overlap: they sum to a constant 0.5 / sqrt(2) */
  CHECK(bad == 0 && fabsf(peak - 0.5f / sqrtf(2.0f)) < 0.01f);
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
  free_src(&s);
}

static void test_pool(void) {
  GcParams p; gc_default_params(&p);
  p.size_ms = 1000; p.density = 200;                /* 200 overlapping grains wanted */
  gc_init(&G, SR, 1);
  Src s = make_src(SR * 4, SR, 1, f_half);
  gc_set_source(&G, &s.src);
  gc_note_on(&G, &p, 60, 127);
  int bad = 0;
  float peak = run(&p, 2.0, &bad);
  CHECK(G.nactive <= GC_POOL && G.skipped > 0 && bad == 0);
  /* the overlap counted for loudness is capped at the pool: 64 grains of 0.5/8 at most */
  CHECK(peak > 0.5f * GC_POOL / 8.0f * 0.4f && peak < 0.5f * GC_POOL / 8.0f + 0.01f);
  free_src(&s);
}

static void test_chaos(void) {
  GcParams p; gc_default_params(&p);
  p.size_ms = 10; p.density = 50; p.chaos = 1.0f;
  gc_init(&G, SR, 7);
  Src s = make_src(SR, SR, 1, f_half);
  gc_set_source(&G, &s.src);
  gc_note_on(&G, &p, 60, 127);
  run(&p, 20.0, NULL);
  CHECK(G.born > 900 && G.born < 1100);             /* about 50/s on average */
  free_src(&s);
}

static void test_position_scan(void) {
  GcParams p; gc_default_params(&p);
  const int64_t n = SR * 10;
  Src s = make_src(n, SR, 1, f_ramp);
  gc_init(&G, SR, 1);
  gc_set_source(&G, &s.src);

  p.position = 0.5f; p.scan = 0.0f; p.size_ms = 20; p.density = 10; p.shape = 1.0f;
  gc_note_on(&G, &p, 60, 127);
  run(&p, 2.0, NULL);
  CHECK(fabs(G.voice.playhead - 0.5 * (n - 1)) < 1.0);   /* freeze */
  CHECK(fabs(G.last_start - 0.5 * (n - 1)) < 1.0);

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
  free_src(&s);
}

/* A grain that would run over the end starts earlier instead (a jump to the start would click). */
static void test_no_end_jump(void) {
  GcParams p; gc_default_params(&p);
  const int64_t n = SR;                             /* 1 s ramp: a jump to the start would be a step of ~1 */
  Src s = make_src(n, SR, 1, f_ramp);
  gc_init(&G, SR, 1);
  gc_set_source(&G, &s.src);
  p.position = 0.999f; p.scan = 0.0f; p.size_ms = 100; p.density = 20; p.shape = 1.0f;
  gc_note_on(&G, &p, 60, 127);
  float prev = 0, jump = 0;
  for (int k = 0; k < SR / BUF; k++) {
    gc_process(&G, &p, L, R, BUF);
    for (int i = 0; i < BUF; i++) { jump = fmaxf(jump, fabsf(L[i] - prev)); prev = L[i]; }
  }
  CHECK(jump < 0.02f);                              /* only the window's smooth edges */
  for (int i = 0; i < GC_POOL; i++)
    if (G.grains[i].active) CHECK(G.grains[i].idx + (int64_t)(G.grains[i].remain * G.grains[i].inc) < n);
  free_src(&s);
}

/* A sample longer than the grain's reach is not needed: very short samples loop through the guard frames. */
static void test_loop_short(void) {
  GcParams p; gc_default_params(&p);
  Src s = make_src(1000, SR, 1, f_half);
  gc_init(&G, SR, 1);
  gc_set_source(&G, &s.src);
  p.position = 0.99f; p.scan = 0.0f; p.size_ms = 100; p.density = 5; p.shape = 1.0f;
  gc_note_on(&G, &p, 84, 127);                      /* 4x speed: the grain crosses the end many times */
  int bad = 0;
  const float top = 0.5f * G.win_gain[GC_SHAPES - 1];  /* constant 0.5 through the flat window */
  float peak = run(&p, 0.5, &bad);
  CHECK(bad == 0 && fabsf(peak - top) < 1e-4f);
  G.interp = GC_INTERP_HERMITE;
  gc_note_on(&G, &p, 84, 127);
  peak = run(&p, 0.5, &bad);
  CHECK(bad == 0 && fabsf(peak - top) < 1e-4f);
  free_src(&s);
}

static void test_pitch_levels(void) {
  GcParams p; gc_default_params(&p);
  Src s = make_src(SR, 44100, 3, f_half);           /* 44.1 kHz file at 48 kHz */
  gc_init(&G, SR, 1);
  gc_set_source(&G, &s.src);
  const float r = 44100.0f / 48000.0f;
  gc_note_on(&G, &p, 60, 127); gc_process(&G, &p, L, R, BUF);
  CHECK(G.last_level == 0);
  gc_note_on(&G, &p, 67, 127); gc_process(&G, &p, L, R, BUF);   /* a fifth up: 1.37x, still level 0 */
  CHECK(G.last_level == 0);
  gc_note_on(&G, &p, 72, 127); gc_process(&G, &p, L, R, BUF);   /* octave: 1.84x -> level 1 at 0.92x */
  CHECK(G.last_level == 1);
  int found = 0;
  for (int i = 0; i < GC_POOL; i++)
    if (G.grains[i].active && fabsf(G.grains[i].inc - r) < 1e-5f && G.grains[i].frames == SR / 2) found = 1;
  CHECK(found);
  gc_note_on(&G, &p, 84, 127); gc_process(&G, &p, L, R, BUF);   /* two octaves -> level 2 */
  CHECK(G.last_level == 2);
  gc_note_on(&G, &p, 96, 127); gc_process(&G, &p, L, R, BUF);   /* three octaves: no level 3, stays on 2 */
  CHECK(G.last_level == 2);
  gc_note_on(&G, &p, 48, 127); gc_process(&G, &p, L, R, BUF);   /* down: level 0 */
  CHECK(G.last_level == 0);
  G.use_levels = 0;
  gc_note_on(&G, &p, 84, 127); gc_process(&G, &p, L, R, BUF);
  CHECK(G.last_level == 0);
  free_src(&s);
}

/* A 19.2 kHz tone an octave up would fold back to 9.6 kHz; the band-limited level removes it. */
static void test_antialias(void) {
  GcParams p; gc_default_params(&p);
  p.size_ms = 100; p.density = 20; p.scan = 0.5f;
  Src s = make_src(SR * 4, SR, 3, f_hi_sine);
  gc_init(&G, SR, 1);
  gc_set_source(&G, &s.src);
  G.use_levels = 0;
  gc_note_on(&G, &p, 72, 127);
  const double aliased = run_rms(&p, 1.0);
  G.use_levels = 1;
  gc_note_on(&G, &p, 72, 127);
  run(&p, 0.2, NULL);                               /* level-0 grains finish */
  const double clean = run_rms(&p, 1.0);
  CHECK(aliased > 0.05 && clean < aliased * 0.01);  /* more than 40 dB less */
  /* and an audible tone an octave up keeps its level */
  Src t = make_src(SR * 4, SR, 3, f_sine);
  gc_init(&G, SR, 1);
  gc_set_source(&G, &t.src);
  G.use_levels = 0;
  gc_note_on(&G, &p, 72, 127);
  const double a0 = run_rms(&p, 1.0);
  G.use_levels = 1;
  gc_note_on(&G, &p, 72, 127);
  run(&p, 0.2, NULL);
  const double a1 = run_rms(&p, 1.0);
  CHECK(fabs(a1 / a0 - 1.0) < 0.05);
  free_src(&s); free_src(&t);
}

/* At the original pitch and integer start positions both interpolations read the samples as they are. */
static void test_hermite_matches_at_unity(void) {
  GcParams p; gc_default_params(&p);
  p.scan = 0.0f; p.size_ms = 50; p.density = 20;
  Src s = make_src(SR, SR, 1, f_sine);
  static float lin[SR / 2], her[SR / 2];
  for (int mode = 0; mode < 2; mode++) {
    gc_init(&G, SR, 3);
    gc_set_source(&G, &s.src);
    G.interp = mode ? GC_INTERP_HERMITE : GC_INTERP_LINEAR;
    gc_note_on(&G, &p, 60, 127);
    for (int k = 0; k < SR / 2 / BUF; k++) {
      gc_process(&G, &p, L, R, BUF);
      memcpy((mode ? her : lin) + k * BUF, L, sizeof L);
    }
  }
  float d = 0;
  for (int i = 0; i < SR / 2; i++) d = fmaxf(d, fabsf(lin[i] - her[i]));
  CHECK(d < 1e-6f);
  free_src(&s);
}

/* Replacing the sample while grains play: no jump, the old grains fade and then release the old sample. */
static void test_swap_fade(void) {
  GcParams p; gc_default_params(&p);
  p.size_ms = 200; p.density = 20; p.shape = 1.0f;
  Src a = make_src(SR, SR, 1, f_half), b = make_src(SR, SR, 1, f_half);
  gc_init(&G, SR, 1);
  const uint32_t ga = gc_set_source(&G, &a.src);
  gc_note_on(&G, &p, 60, 127);
  run(&p, 0.3, NULL);
  float last = L[BUF - 1];
  const uint32_t gb = gc_set_source(&G, &b.src);
  CHECK(gb != ga && gc_gen_busy(&G, ga) > 0);
  gc_process(&G, &p, L, R, BUF);
  float jump = fabsf(L[0] - last);
  for (int i = 1; i < BUF; i++) jump = fmaxf(jump, fabsf(L[i] - L[i - 1]));
  CHECK(jump < 0.01f);
  run(&p, 0.01, NULL);
  CHECK(gc_gen_busy(&G, ga) == 0 && gc_gen_busy(&G, gb) > 0);
  free_src(&a); free_src(&b);
}

int main(void) {
  test_windows();
  test_silence();
  test_note_cycle();
  test_pool();
  test_chaos();
  test_position_scan();
  test_no_end_jump();
  test_loop_short();
  test_pitch_levels();
  test_antialias();
  test_hermite_matches_at_unity();
  test_swap_fade();
  printf(fails ? "%d FAILED\n" : "All grain tests passed\n", fails);
  return fails != 0;
}
