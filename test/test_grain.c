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
static int argmax(const float* w) {
  int k = 0;
  for (int i = 0; i <= GC_WIN; i++) if (w[i] > w[k]) k = i;
  return k;
}
static void test_windows(void) {
  gc_init(&G, SR, 1);
  for (int s = 0; s < GC_SHAPES; s++) {
    CHECK(G.win[s][0] == 0.0f && G.win[s][GC_WIN] == 0.0f);
    CHECK(G.win[s][argmax(G.win[s])] > 0.99f);
    for (int i = 0; i <= GC_WIN; i++) CHECK(G.win[s][i] >= 0.0f && G.win[s][i] <= 1.0f + 1e-6f);
    /* no click: the first and last 1 % of the table stay low */
    CHECK(G.win[s][GC_WIN / 100] < 0.6f && G.win[s][GC_WIN - GC_WIN / 100] < 0.6f);
  }
  const float* sq = G.win[0];
  const float* dc = G.win[GC_SHAPE_DECAY];
  const float* bl = G.win[GC_SHAPE_BELL];
  const float* rv = G.win[GC_SHAPES - 1];
  CHECK(sq[GC_WIN / 10] == 1.0f && sq[GC_WIN / 2] == 1.0f && sq[GC_WIN * 9 / 10] == 1.0f);   /* square */
  CHECK(argmax(dc) < GC_WIN / 20 && dc[GC_WIN / 2] < 0.1f);                                 /* decay */
  CHECK(argmax(rv) > GC_WIN * 9 / 10 && rv[GC_WIN / 2] < 0.6f);                             /* reverse */
  for (int i = 0; i <= GC_WIN; i++) CHECK(fabsf(bl[i] - bl[GC_WIN - i]) < 1e-3f);           /* bell symmetric */
  CHECK(fabsf(bl[GC_WIN / 4] + bl[GC_WIN * 3 / 4] - 1.0f) < 0.01f);    /* bells overlapping by half add up to 1 */
  CHECK(G.win_gain[GC_SHAPE_BELL] == 1.0f && G.win_gain[0] < 0.9f && G.win_gain[GC_SHAPE_DECAY] > 1.2f);
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
  run(&p, 1.0, NULL);                               /* the release of that note is over */
  CHECK(gc_active_voices(&G) == 0 && run(&p, 0.1, NULL) == 0.0f);   /* no note */
  gc_note_on(&G, &p, 60, 127);
  run(&p, 0.05, NULL);
  gc_set_source(&G, NULL);                          /* sample removed while playing: grains fade out */
  run(&p, 0.01, NULL);
  CHECK(G.nactive == 0 && run(&p, 0.05, NULL) == 0.0f);
  free_src(&s);
}

static void test_note_cycle(void) {
  GcParams p; gc_default_params(&p);
  p.size_ms = 50; p.density = 40; p.release_ms = 10;
  gc_init(&G, SR, 1);
  Src s = make_src(SR, SR, 1, f_half);
  gc_set_source(&G, &s.src);
  gc_note_on(&G, &p, 60, 127);
  gc_process(&G, &p, L, R, BUF);
  CHECK(G.born == 1 && L[0] == 0.0f && L[100] > 0.0f);  /* first grain starts at once */
  int bad = 0;
  float peak = run(&p, 1.0, &bad);
  /* two bell grains overlap by half: they sum to a constant 0.5 / sqrt(2) */
  CHECK(bad == 0 && fabsf(peak - 0.5f / sqrtf(2.0f)) < 0.01f);
  CHECK(G.born >= 40 && G.born <= 42);              /* density 40/s, chaos 0 */
  gc_note_off(&G, 60);
  run(&p, 0.03, NULL);                              /* release 10 ms to -60 dB, -80 dB a little later */
  CHECK(G.voices[0].stage == GC_V_IDLE && G.nactive == 0 && G.voices[0].note == -1);
  CHECK(run(&p, 0.05, NULL) == 0.0f);
  gc_note_on(&G, &p, 60, 127);
  gc_note_off(&G, 61);                              /* another key: the voice keeps playing */
  run(&p, 0.1, NULL);
  CHECK(G.voices[0].gate == 1 && G.nactive > 0);
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

  p.position = 0.5f; p.scan = 0.0f; p.size_ms = 20; p.density = 10; p.shape = 0.0f;
  gc_note_on(&G, &p, 60, 127);
  run(&p, 2.0, NULL);
  CHECK(fabs(G.voices[0].playhead - 0.5 * (n - 1)) < 1.0);   /* freeze */
  CHECK(fabs(G.last_start - 0.5 * (n - 1)) < 1.0);

  p.scan = 0.01f;                                   /* inside the dead zone: still frozen */
  run(&p, 1.0, NULL);
  CHECK(fabs(G.voices[0].playhead - 0.5 * (n - 1)) < 1.0);

  p.scan = 1.0f;                                    /* realtime: one second moves 48000 frames */
  run(&p, 1.0, NULL);
  CHECK(fabs(G.voices[0].playhead - (0.5 * (n - 1) + SR)) < 2 * BUF);

  p.scan = -2.0f;                                   /* backwards, twice as fast */
  double ph = G.voices[0].playhead;
  run(&p, 0.5, NULL);
  CHECK(fabs(G.voices[0].playhead - (ph - SR)) < 2 * BUF);

  /* window: the playhead loops inside [position, position + window) */
  p.position = 0.2f; p.window = 0.1f; p.scan = 2.0f;
  gc_note_on(&G, &p, 60, 127);
  CHECK(fabs(G.voices[0].playhead - 0.2 * (n - 1)) < 1.0);   /* new note starts at Position */
  int inside = 1;
  for (int k = 0; k < 2000; k++) {
    gc_process(&G, &p, L, R, BUF);
    if (G.voices[0].playhead < 0.2 * (n - 1) - 1 || G.voices[0].playhead > 0.2 * (n - 1) + 0.1 * n + 1) inside = 0;
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
  p.position = 0.999f; p.scan = 0.0f; p.size_ms = 100; p.density = 20; p.shape = 0.0f;
  gc_note_on(&G, &p, 60, 127);
  float prev = 0, jump = 0;
  for (int k = 0; k < SR / BUF; k++) {
    gc_process(&G, &p, L, R, BUF);
    for (int i = 0; i < BUF; i++) { jump = fmaxf(jump, fabsf(L[i] - prev)); prev = L[i]; }
  }
  CHECK(jump < 0.05f);                              /* only the window's edges; a jump to the start is ~0.6 */
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
  p.position = 0.99f; p.scan = 0.0f; p.size_ms = 100; p.density = 5; p.shape = 0.0f;
  gc_note_on(&G, &p, 84, 127);                      /* 4x speed: the grain crosses the end many times */
  int bad = 0;
  const float top = 0.5f * G.win_gain[0];            /* constant 0.5 through the square window */
  float peak = run(&p, 0.5, &bad);
  CHECK(bad == 0 && fabsf(peak - top) < 1e-4f);
  G.interp = GC_INTERP_HERMITE;
  gc_all_off(&G);                                   /* the last grain of the first note must not overlap */
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
  p.size_ms = 200; p.density = 20; p.shape = 0.0f;
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

/* ---------- voices ---------- */
static GcParams poly_params(void) {
  GcParams p; gc_default_params(&p);
  p.size_ms = 40; p.density = 50; p.attack_ms = 1; p.release_ms = 10; p.scan = 0.0f;
  return p;
}
static int voice_of(int note) {
  for (int i = 0; i < GC_VOICES; i++)
    if (G.voices[i].note == note && G.voices[i].stage != GC_V_IDLE) return i;
  return -1;
}

static void test_poly(void) {
  GcParams p = poly_params();
  Src s = make_src(SR, SR, 1, f_half);
  gc_init(&G, SR, 1);
  gc_set_source(&G, &s.src);
  gc_note_on(&G, &p, 60, 100);
  gc_note_on(&G, &p, 64, 100);
  gc_note_on(&G, &p, 67, 100);
  run(&p, 0.1, NULL);
  CHECK(gc_active_voices(&G) == 3 && voice_of(60) >= 0 && voice_of(64) >= 0 && voice_of(67) >= 0);
  int per[GC_VOICES] = { 0 };
  for (int i = 0; i < GC_POOL; i++) if (G.grains[i].active) per[G.grains[i].voice]++;
  CHECK(per[voice_of(60)] > 0 && per[voice_of(64)] > 0 && per[voice_of(67)] > 0);
  const int v64 = voice_of(64);
  gc_note_on(&G, &p, 64, 100);                      /* same key again: same voice */
  CHECK(voice_of(64) == v64 && gc_active_voices(&G) == 3);
  gc_note_off(&G, 64);
  run(&p, 0.05, NULL);
  CHECK(gc_active_voices(&G) == 2 && voice_of(64) < 0);
  for (int i = 0; i < GC_POOL; i++) if (G.grains[i].active) CHECK(G.grains[i].voice != v64);
  gc_all_notes_off(&G);
  run(&p, 0.05, NULL);
  CHECK(gc_active_voices(&G) == 0 && G.nactive == 0 && run(&p, 0.02, NULL) == 0.0f);
  free_src(&s);
}

static void test_steal(void) {
  GcParams p = poly_params();
  Src s = make_src(SR, SR, 1, f_half);
  gc_init(&G, SR, 1);
  gc_set_source(&G, &s.src);
  gc_set_voices(&G, 2, 0);
  gc_note_on(&G, &p, 60, 100); run(&p, 0.05, NULL);
  gc_note_on(&G, &p, 62, 100); run(&p, 0.05, NULL);
  gc_note_on(&G, &p, 64, 100);                      /* no free voice: the oldest (60) fades in 5 ms */
  const int v = voice_of(60);
  CHECK(v >= 0 && G.voices[v].stage == GC_V_STEAL);
  float prev = L[BUF - 1], jump = 0;
  for (int k = 0; k < 4; k++) {
    gc_process(&G, &p, L, R, BUF);
    for (int i = 0; i < BUF; i++) { jump = fmaxf(jump, fabsf(L[i] - prev)); prev = L[i]; }
  }
  CHECK(voice_of(64) == v && voice_of(60) < 0 && voice_of(62) >= 0);
  CHECK(jump < 0.05f);                              /* the stolen voice faded, no click */
  /* a quiet voice in release is stolen before an older held one */
  const int v62 = voice_of(62);
  gc_note_off(&G, 64); run(&p, 0.005, NULL);        /* 64 is in release; 62 is older but held */
  CHECK(G.voices[v].stage == GC_V_RELEASE);
  gc_note_on(&G, &p, 65, 100);
  CHECK(G.voices[v].stage == GC_V_STEAL && G.voices[v].pending_note == 65);
  CHECK(G.voices[v62].stage == GC_V_SUSTAIN && G.voices[v62].note == 62);
  run(&p, 0.02, NULL);
  CHECK(voice_of(62) == v62 && voice_of(65) == v);
  /* released during the steal fade: starts and releases */
  gc_note_on(&G, &p, 67, 100);
  gc_note_off(&G, 67);
  run(&p, 0.1, NULL);
  CHECK(voice_of(67) < 0);
  free_src(&s);
}

static void test_envelope(void) {
  GcParams p = poly_params();
  p.size_ms = 100; p.density = 40; p.shape = 0.0f;  /* square grains overlapping: steady level */
  p.attack_ms = 200; p.release_ms = 300;
  Src s = make_src(SR * 2, SR, 1, f_half);
  gc_init(&G, SR, 1);
  gc_set_source(&G, &s.src);
  gc_note_on(&G, &p, 60, 127);
  run(&p, 0.05, NULL);
  CHECK(G.voices[0].stage == GC_V_ATTACK && fabsf(G.voices[0].env - 0.25f) < 0.02f);   /* linear attack */
  run(&p, 0.2, NULL);
  CHECK(G.voices[0].stage == GC_V_SUSTAIN && G.voices[0].env == 1.0f);
  gc_note_off(&G, 60);
  run(&p, 0.3, NULL);                               /* release time: -60 dB */
  CHECK(G.voices[0].stage == GC_V_RELEASE && fabsf(G.voices[0].env - 0.001f) < 0.0005f);
  CHECK(G.nactive > 0);                             /* grains are still born in release */
  run(&p, 0.2, NULL);
  CHECK(G.voices[0].stage == GC_V_IDLE && G.nactive == 0);
  /* retrigger in release: the level goes up from where it is, no jump to 0 */
  gc_note_on(&G, &p, 60, 127); run(&p, 0.3, NULL);
  gc_note_off(&G, 60); run(&p, 0.05, NULL);
  const float e = G.voices[0].env;
  gc_note_on(&G, &p, 60, 127);
  gc_process(&G, &p, L, R, GC_BLOCK);
  CHECK(G.voices[0].env > e && G.voices[0].stage == GC_V_ATTACK);
  free_src(&s);
}

static void test_mono(void) {
  GcParams p = poly_params();
  Src s = make_src(SR, SR, 1, f_half);
  gc_init(&G, SR, 1);
  gc_set_source(&G, &s.src);
  gc_set_voices(&G, 1, 1);                          /* one voice, legato */
  gc_note_on(&G, &p, 60, 100); run(&p, 0.05, NULL);
  const uint32_t age = G.voices[0].age;
  gc_note_on(&G, &p, 64, 100);                      /* legato: same voice, envelope goes on */
  CHECK(gc_active_voices(&G) == 1 && G.voices[0].note == 64 && G.voices[0].age == age &&
        G.voices[0].stage == GC_V_SUSTAIN);
  run(&p, 0.05, NULL);
  int pitch_ok = 0;
  for (int i = 0; i < GC_POOL; i++)
    if (G.grains[i].active && fabsf(G.grains[i].inc - exp2f(4.0f / 12.0f)) < 1e-4f) pitch_ok = 1;
  CHECK(pitch_ok);
  gc_note_off(&G, 64);                              /* back to the held key */
  CHECK(G.voices[0].note == 60 && G.voices[0].gate);
  gc_note_off(&G, 60);
  CHECK(G.voices[0].stage == GC_V_RELEASE);
  run(&p, 0.05, NULL);
  CHECK(gc_active_voices(&G) == 0);
  gc_set_voices(&G, 8, 0);                             /* back to poly */
  gc_note_on(&G, &p, 60, 100); gc_note_on(&G, &p, 64, 100);
  CHECK(gc_active_voices(&G) == 2);
  free_src(&s);
}

/* one voice without legato: every key starts the voice again, also on the way back to a held key */
static void test_mono_retrigger(void) {
  GcParams p = poly_params();
  p.position = 0.25f; p.scan = 1.0f; p.attack_ms = 50;
  Src s = make_src(SR, SR, 1, f_half);
  gc_init(&G, SR, 1);
  gc_set_source(&G, &s.src);
  gc_set_voices(&G, 1, 0);
  gc_note_on(&G, &p, 60, 100); run(&p, 0.1, NULL);
  CHECK(G.voices[0].stage == GC_V_SUSTAIN && G.voices[0].playhead > 0.25 * SR + 0.09 * SR);
  const uint32_t age = G.voices[0].age;
  gc_note_on(&G, &p, 64, 100);
  CHECK(gc_active_voices(&G) == 1 && G.voices[0].note == 64 && G.voices[0].age != age &&
        G.voices[0].stage == GC_V_ATTACK && G.voices[0].env > 0.9f);   /* again, from the current level */
  CHECK(fabs(G.voices[0].playhead - 0.25 * (SR - 1)) < 1.0);         /* reading from Position again */
  run(&p, 0.1, NULL);
  gc_note_off(&G, 64);                              /* back to 60, started again */
  CHECK(G.voices[0].note == 60 && G.voices[0].restart == 1);
  gc_process(&G, &p, L, R, GC_BLOCK);
  /* started again: reading from Position; the attack rises from ~1, so it is over within this block */
  CHECK(G.voices[0].restart == 0 && fabs(G.voices[0].playhead - (0.25 * (SR - 1) + GC_BLOCK)) < 1.0);
  /* switching legato on and off keeps the sound (still one voice) */
  gc_set_voices(&G, 1, 1);
  CHECK(gc_active_voices(&G) == 1 && G.voices[0].gate);
  gc_note_on(&G, &p, 67, 100);
  CHECK(G.voices[0].note == 67 && G.voices[0].stage != GC_V_ATTACK);  /* legato now */
  free_src(&s);
}

/* mono portamento: a new key glides from the sounding pitch in Glide ms */
static void test_glide(void) {
  GcParams p = poly_params();
  p.density = 200; p.size_ms = 5;
  Src s = make_src(SR, SR, 1, f_half);
  gc_init(&G, SR, 1);
  gc_set_source(&G, &s.src);
  gc_set_voices(&G, 1, 1);                          /* L */
  gc_set_glide(&G, 100.0f);
  gc_note_on(&G, &p, 60, 100);
  CHECK(G.voices[0].pitch == 60.0f);                /* from silence: no glide */
  run(&p, 0.05, NULL);
  gc_note_on(&G, &p, 72, 100);                      /* legato: glide up an octave in 100 ms */
  for (int k = 0; k < (int)(0.05f * SR) / GC_BLOCK; k++) gc_process(&G, &p, L, R, GC_BLOCK);
  CHECK(fabsf(G.voices[0].pitch - 66.0f) < 0.3f && G.last_pitch > 64.0f && G.last_pitch < 68.0f);
  run(&p, 0.06, NULL);
  CHECK(G.voices[0].pitch == 72.0f && G.last_pitch == 72.0f);
  gc_note_off(&G, 72);                              /* back to 60, gliding too */
  for (int k = 0; k < 10; k++) gc_process(&G, &p, L, R, GC_BLOCK);
  CHECK(G.voices[0].pitch < 72.0f && G.voices[0].pitch > 60.0f && G.voices[0].note == 60);
  run(&p, 0.11, NULL);
  CHECK(G.voices[0].pitch == 60.0f);
  /* key track follows the gliding pitch */
  p.keytrack = 1.0f; p.cutoff_hz = 1000;
  gc_note_on(&G, &p, 72, 100);
  for (int k = 0; k < (int)(0.05f * SR) / GC_BLOCK; k++) gc_process(&G, &p, L, R, GC_BLOCK);
  CHECK(G.voices[0].fc_hz > 1100 && G.voices[0].fc_hz < 1900);   /* between 1 kHz (C4) and 2 kHz (C5) */
  p.keytrack = 0.0f; p.cutoff_hz = GC_CUTOFF_MAX;
  /* mono 1 (start again on each key): glides as well, but not from silence */
  gc_all_off(&G);
  gc_set_voices(&G, 1, 0);
  gc_note_on(&G, &p, 60, 100); run(&p, 0.05, NULL);
  gc_note_on(&G, &p, 67, 100);
  CHECK(G.voices[0].pitch == 60.0f && G.voices[0].glide_step > 0.0f);
  /* Glide 0: a jump */
  gc_set_glide(&G, 0.0f);
  gc_note_on(&G, &p, 55, 100);
  CHECK(G.voices[0].pitch == 55.0f);
  /* poly: never glides */
  gc_all_off(&G);
  gc_set_voices(&G, 8, 0);
  gc_set_glide(&G, 500.0f);
  gc_note_on(&G, &p, 60, 100); run(&p, 0.05, NULL);
  gc_note_on(&G, &p, 60, 100);                      /* same key again */
  gc_note_on(&G, &p, 72, 100);
  CHECK(G.voices[0].pitch == 60.0f && G.voices[1].pitch == 72.0f);
  free_src(&s);
}

static void test_spread(void) {
  GcParams p = poly_params();
  Src s = make_src(SR, SR, 1, f_half);
  gc_init(&G, SR, 1);
  gc_set_source(&G, &s.src);
  gc_note_on(&G, &p, 60, 127);
  run(&p, 0.1, NULL);
  float centre = 0.0f;
  for (int i = 0; i < GC_POOL; i++)
    if (G.grains[i].active) { CHECK(G.grains[i].gl == G.grains[i].gr); centre = G.grains[i].gl; }
  CHECK(centre > 0.0f);
  p.spread = 1.0f;
  int left = 0, right = 0;
  for (int k = 0; k < 40; k++) {
    run(&p, 0.02, NULL);
    for (int i = 0; i < GC_POOL; i++) {
      const GcGrain* gr = &G.grains[i];
      if (!gr->active) continue;
      CHECK(fabsf(fmaxf(gr->gl, gr->gr) - centre) < 1e-6f);   /* balance law: the louder side keeps unity */
      if (gr->gl > gr->gr * 1.2f) left++;
      if (gr->gr > gr->gl * 1.2f) right++;
    }
  }
  CHECK(left > 5 && right > 5);
  free_src(&s);
}

/* ---------- pitch ---------- */
static void test_pitch(void) {
  GcParams p = poly_params();
  Src s = make_src(SR, SR, 1, f_half);
  gc_init(&G, SR, 1);
  gc_set_source(&G, &s.src);
  G.use_levels = 0;
  p.pitch = 7.0f;
  gc_note_on(&G, &p, 60, 100); gc_process(&G, &p, L, R, BUF);
  CHECK(fabsf(G.last_pitch - 67.0f) < 1e-4f);
  CHECK(fabsf(G.grains[0].inc - exp2f(7.0f / 12.0f)) < 1e-4f);
  p.pitch = -12.5f;                                 /* fractional, below */
  gc_all_off(&G); gc_note_on(&G, &p, 60, 100); gc_process(&G, &p, L, R, BUF);
  CHECK(fabsf(G.last_pitch - 47.5f) < 1e-4f);
  /* Pitch Spray: spread within +-spray, really spread */
  p.pitch = 0.0f; p.pitch_spray = 3.0f; p.density = 200; p.size_ms = 5;
  gc_all_off(&G); gc_note_on(&G, &p, 60, 100);
  float lo = 1e9f, hi = -1e9f;
  for (int k = 0; k < 1000; k++) {
    gc_process(&G, &p, L, R, GC_BLOCK);
    lo = fminf(lo, G.last_pitch); hi = fmaxf(hi, G.last_pitch);
  }
  CHECK(lo >= 57.0f && hi <= 63.0f && hi - lo > 5.0f);
  free_src(&s);
}

static void test_quantize(void) {
  gc_init(&G, SR, 1);
  gc_set_scale(&G, 0, 1);                           /* C major */
  CHECK(gc_snap(&G, 61.3f) == 62.0f && gc_snap(&G, 60.4f) == 60.0f && gc_snap(&G, 65.6f) == 65.0f);
  CHECK(gc_snap(&G, 71.6f) == 72.0f);               /* B -> C across the octave */
  CHECK(gc_snap(&G, 59.0f) == 59.0f && gc_snap(&G, 58.4f) == 59.0f && gc_snap(&G, 57.9f) == 57.0f);   /* below */
  gc_set_scale(&G, 2, 2);                           /* D minor: D E F G A Bb C */
  CHECK(gc_snap(&G, 66.2f) == 67.0f && gc_snap(&G, 65.8f) == 65.0f && gc_snap(&G, 70.9f) == 70.0f &&
        gc_snap(&G, 61.4f) == 62.0f && gc_snap(&G, 60.6f) == 60.0f);
  gc_set_scale(&G, 0, 0);                           /* chromatic: nearest semitone */
  CHECK(gc_snap(&G, 60.4f) == 60.0f && gc_snap(&G, 60.6f) == 61.0f);
  gc_set_scale(&G, 13, 99);                         /* out of range: key wraps, scale chromatic */
  CHECK(G.key == 1 && G.scale == 0);

  GcParams p = poly_params();
  Src s = make_src(SR, SR, 1, f_half);
  gc_set_source(&G, &s.src);
  gc_set_scale(&G, 0, 1);
  p.pitch = 1.3f; p.quantize = 1.0f;                /* 61.3 -> D */
  gc_note_on(&G, &p, 60, 100); gc_process(&G, &p, L, R, BUF);
  CHECK(fabsf(G.last_pitch - 62.0f) < 1e-4f);
  p.quantize = 0.5f;                                /* half way */
  gc_all_off(&G); gc_note_on(&G, &p, 60, 100); gc_process(&G, &p, L, R, BUF);
  CHECK(fabsf(G.last_pitch - 61.65f) < 1e-4f);
  p.pitch = 0.0f; p.quantize = 1.0f;                /* a key outside the scale is pulled in too */
  gc_all_off(&G); gc_note_on(&G, &p, 66, 100); gc_process(&G, &p, L, R, BUF);
  CHECK(G.last_pitch == 65.0f || G.last_pitch == 67.0f);
  /* spray + quantize 100 %: every grain on a note of the scale */
  p.pitch_spray = 12.0f; p.density = 200; p.size_ms = 5;
  gc_all_off(&G); gc_note_on(&G, &p, 60, 100);
  int off_scale = 0;
  for (int k = 0; k < 500; k++) {
    gc_process(&G, &p, L, R, GC_BLOCK);
    const int pc = ((int)lrintf(G.last_pitch) % 12 + 12) % 12;
    if (fabsf(G.last_pitch - rintf(G.last_pitch)) > 1e-4f || pc == 1 || pc == 3 || pc == 6 || pc == 8 || pc == 10)
      off_scale++;
  }
  CHECK(off_scale == 0);
  free_src(&s);
}

/* A ramp read backwards goes down; nothing is read outside the sample (ASan). */
static void test_reverse(void) {
  GcParams p = poly_params();
  const int64_t n = SR;
  Src s = make_src(n, SR, 1, f_ramp);
  gc_init(&G, SR, 1);
  gc_set_source(&G, &s.src);
  p.reverse = 1.0f; p.shape = 0.0f; p.size_ms = 100; p.density = 5; p.position = 0.5f;
  gc_note_on(&G, &p, 60, 127);
  gc_process(&G, &p, L, R, BUF);
  CHECK(G.grains[0].inc < 0.0f);
  float a = 0, b = 0;                                /* inside the square window: output ~ ramp value */
  for (int k = 0; k < 8; k++) gc_process(&G, &p, L, R, BUF);
  a = L[100];
  gc_process(&G, &p, L, R, BUF);
  b = L[100];
  CHECK(b < a && a > 0.0f);
  /* near the start, backwards: the start moves so the grain ends inside the sample */
  p.position = 0.0f;
  gc_all_off(&G); gc_note_on(&G, &p, 60, 127);
  gc_process(&G, &p, L, R, BUF);
  /* started at span = 4800 + 3 frames, then went down by one block */
  CHECK(G.grains[0].idx >= (int64_t)(0.1f * SR) - BUF && G.grains[0].idx <= (int64_t)(0.1f * SR));
  float prev = L[BUF - 1], jump = 0;
  int bad = 0;
  for (int k = 0; k < SR / BUF / 2; k++) {
    gc_process(&G, &p, L, R, BUF);
    for (int i = 0; i < BUF; i++) {
      if (!isfinite(L[i])) bad++;
      jump = fmaxf(jump, fabsf(L[i] - prev)); prev = L[i];
    }
  }
  CHECK(bad == 0 && jump < 0.05f);
  /* short looping sample backwards, both interpolations, pitched */
  Src t = make_src(1000, SR, 1, f_half);
  for (int mode = 0; mode < 2; mode++) {
    gc_init(&G, SR, 1);
    gc_set_source(&G, &t.src);
    G.interp = mode;
    gc_note_on(&G, &p, 79, 127);
    float pk = run(&p, 0.3, &bad);
    CHECK(bad == 0 && fabsf(pk - 0.5f * G.win_gain[0]) < 1e-3f);
  }
  /* 50 %: about half the grains */
  p.reverse = 0.5f; p.density = 200; p.size_ms = 5;
  gc_init(&G, SR, 9);
  gc_set_source(&G, &s.src);
  gc_note_on(&G, &p, 60, 127);
  for (int k = 0; k < 1000; k++) gc_process(&G, &p, L, R, GC_BLOCK);
  CHECK(G.born > 100 && G.born_back > G.born * 2 / 5 && G.born_back < G.born * 3 / 5);
  free_src(&s); free_src(&t);
}

/* ---------- filter ---------- */
static double sine_hz = 1000.0;
static float f_sine_hz(int64_t i, int64_t n) { (void)n; return 0.5f * (float)sin(2 * M_PI * sine_hz * i / SR); }

/* Gain of the voice filter at `hz`: one long square grain of a sine, RMS in its middle, filter / no filter. */
static double filter_gain(double hz, int type, float cutoff, float res) {
  sine_hz = hz;
  Src s = make_src(SR * 2, SR, 1, f_sine_hz);
  double rms[2];
  for (int pass = 0; pass < 2; pass++) {
    GcParams p; gc_default_params(&p);
    p.size_ms = 1000; p.density = 0.5f; p.shape = 0.0f; p.scan = 0.0f; p.attack_ms = 1;
    p.ftype = type; p.cutoff_hz = pass ? cutoff : GC_CUTOFF_MAX; p.resonance = pass ? res : 0.0f;
    if (!pass) p.ftype = GC_LP;                      /* open low-pass = no filter */
    gc_init(&G, SR, 1);
    gc_set_source(&G, &s.src);
    gc_note_on(&G, &p, 60, 127);
    run(&p, 0.3, NULL);                             /* past the edge and the filter's settling */
    rms[pass] = run_rms(&p, 0.3);
  }
  free_src(&s);
  return rms[1] / rms[0];
}
static double db(double g) { return 20.0 * log10(g); }

static void test_filter(void) {
  /* low-pass at 1 kHz, Butterworth: flat below, -3 dB at the cutoff, 12 dB/octave above */
  CHECK(fabs(db(filter_gain(200, GC_LP, 1000, 0))) < 0.5);
  CHECK(fabs(db(filter_gain(1000, GC_LP, 1000, 0)) + 3.0) < 0.6);
  CHECK(db(filter_gain(8000, GC_LP, 1000, 0)) < -33.0);
  /* high-pass: the other way round */
  CHECK(db(filter_gain(200, GC_HP, 1000, 0)) < -25.0);
  CHECK(fabs(db(filter_gain(8000, GC_HP, 1000, 0))) < 0.5);
  /* band-pass: the centre stands out from both sides */
  const double bc = filter_gain(1000, GC_BP, 1000, 0.5f);
  CHECK(bc > 4.0 * filter_gain(150, GC_BP, 1000, 0.5f) && bc > 4.0 * filter_gain(7000, GC_BP, 1000, 0.5f));
  /* resonance: a peak at the cutoff (Q 20 at 100 %) */
  CHECK(filter_gain(1000, GC_LP, 1000, 1.0f) > 10.0);
  CHECK(filter_gain(1000, GC_LP, 1000, 0.5f) > 1.2);

  /* fully open low-pass: bypassed, the output is exactly the unfiltered one */
  GcParams p = poly_params();
  Src s = make_src(SR, SR, 1, f_sine);
  gc_init(&G, SR, 1);
  gc_set_source(&G, &s.src);
  gc_note_on(&G, &p, 60, 127);
  run(&p, 0.05, NULL);
  CHECK(G.voices[0].filter_on == 0);
  p.cutoff_hz = 5000;
  run(&p, 0.05, NULL);
  CHECK(G.voices[0].filter_on == 1);
  p.ftype = GC_HP; p.cutoff_hz = GC_CUTOFF_MIN;     /* fully open high-pass: bypassed too */
  run(&p, 0.1, NULL);
  CHECK(G.voices[0].filter_on == 0);

  /* the cutoff glides to the knob (~10 ms), no jump */
  p.ftype = GC_LP; p.cutoff_hz = GC_CUTOFF_MAX;
  run(&p, 0.1, NULL);
  p.cutoff_hz = 500;
  gc_process(&G, &p, L, R, GC_BLOCK);
  CHECK(G.voices[0].fc_hz > 5000);
  run(&p, 0.1, NULL);
  CHECK(fabsf(G.voices[0].fc_hz - 500) < 5);

  /* key track: an octave up doubles the cutoff at 100 %, half of that at 50 % */
  p.keytrack = 1.0f;
  gc_all_off(&G); gc_note_on(&G, &p, 72, 127); run(&p, 0.05, NULL);
  CHECK(fabsf(G.voices[0].fc_hz - 1000) < 10);
  p.keytrack = 0.5f;
  gc_all_off(&G); gc_note_on(&G, &p, 72, 127); run(&p, 0.05, NULL);
  CHECK(fabsf(G.voices[0].fc_hz - 707) < 10);
  free_src(&s);

  /* stable: full resonance, cutoff jumping around every block, noise in */
  Src nz = make_src(SR, SR, 1, f_hi_sine);
  gc_init(&G, SR, 5);
  gc_set_source(&G, &nz.src);
  p = poly_params();
  p.resonance = 1.0f; p.spray = 1.0f; p.chaos = 1.0f; p.density = 100;
  gc_note_on(&G, &p, 60, 127);
  int bad = 0;
  float peak = 0;
  uint32_t x = 1;
  for (int k = 0; k < 2000; k++) {
    x = x * 1664525u + 1013904223u;
    p.cutoff_hz = GC_CUTOFF_MIN * powf(1000.0f, (float)(x >> 8) / 16777216.0f);
    p.ftype = (int)(x % 3);
    gc_process(&G, &p, L, R, GC_BLOCK);
    for (int i = 0; i < GC_BLOCK; i++) {
      if (!isfinite(L[i])) bad++;
      peak = fmaxf(peak, fabsf(L[i]));
    }
  }
  CHECK(bad == 0 && peak < 50.0f);
  free_src(&nz);
}

/* ---------- modulation ---------- */
static void test_mod_shapes(void) {
  CHECK(fabsf(gc_lfo_value(GC_LFO_SINE, 0.25f, 0, 0) - 1.0f) < 1e-5f);
  CHECK(gc_lfo_value(GC_LFO_TRI, 0.0f, 0, 0) == -1.0f && gc_lfo_value(GC_LFO_TRI, 0.5f, 0, 0) == 1.0f);
  CHECK(gc_lfo_value(GC_LFO_SAW_UP, 0.75f, 0, 0) == 0.5f && gc_lfo_value(GC_LFO_SAW_DOWN, 0.75f, 0, 0) == -0.5f);
  CHECK(gc_lfo_value(GC_LFO_SQUARE, 0.2f, 0, 0) == 1.0f && gc_lfo_value(GC_LFO_SQUARE, 0.7f, 0, 0) == -1.0f);
  CHECK(gc_lfo_value(GC_LFO_SH, 0.7f, 0.3f, -0.9f) == 0.3f);
  CHECK(gc_lfo_value(GC_LFO_SMOOTH, 0.0f, 0.3f, -0.9f) == 0.3f && fabsf(gc_lfo_value(GC_LFO_SMOOTH, 1.0f, 0.3f, -0.9f) + 0.9f) < 1e-6f);
  /* envelopes: decay, rise and hold, rise-fall */
  CHECK(gc_env_value(0.0f, 0.0f, 1.0f) == 1.0f && gc_env_value(0.0f, 0.5f, 1.0f) == 0.25f && gc_env_value(0.0f, 1.5f, 1.0f) == 0.0f);
  CHECK(gc_env_value(1.0f, 0.5f, 1.0f) == 0.5f && gc_env_value(1.0f, 5.0f, 1.0f) == 1.0f);
  CHECK(gc_env_value(0.5f, 0.5f, 1.0f) == 1.0f && gc_env_value(0.5f, 0.25f, 1.0f) == 0.5f && gc_env_value(0.5f, 2.0f, 1.0f) == 0.0f);
}

static void test_mod_grid(void) {
  gc_init(&G, SR, 1);
  CHECK(G.nmod == 0);
  gc_set_mod_cell(&G, GC_SRC_LFO1, GC_DST_PITCH, 0.5f);
  gc_set_mod_cell(&G, GC_SRC_ENV2, GC_DST_CUTOFF, -2.0f);     /* clamped */
  CHECK(G.nmod == 2 && G.mod_cell[GC_SRC_ENV2][GC_DST_CUTOFF] == -1.0f);
  gc_set_mod_cell(&G, GC_SRC_LFO1, GC_DST_PITCH, 0.0f);
  CHECK(G.nmod == 1);
  gc_set_mod_cell(&G, GC_SRC_ENV2, GC_DST_CUTOFF, 0.0f);
  gc_set_mod_cell(&G, 99, 0, 1.0f);                           /* ignored */
  CHECK(G.nmod == 0);

  /* an empty grid changes nothing, to the bit */
  GcParams p = poly_params();
  p.spray = 0.3f; p.chaos = 0.5f; p.spread = 0.5f;
  Src s = make_src(SR, SR, 1, f_sine);
  static float a[SR / 4], b[SR / 4];
  for (int pass = 0; pass < 2; pass++) {
    gc_init(&G, SR, 4);
    gc_set_source(&G, &s.src);
    if (pass) { gc_set_mod_cell(&G, GC_SRC_LFO1, GC_DST_SIZE, 0.7f); gc_set_mod_cell(&G, GC_SRC_LFO1, GC_DST_SIZE, 0.0f); }
    gc_note_on(&G, &p, 60, 100);
    for (int k = 0; k < SR / 4 / BUF; k++) { gc_process(&G, &p, L, R, BUF); memcpy((pass ? b : a) + k * BUF, L, sizeof L); }
  }
  CHECK(!memcmp(a, b, sizeof a));
  free_src(&s);
}

static void test_mod_dest(void) {
  GcParams p = poly_params();
  p.density = 200; p.size_ms = 5;
  Src s = make_src(SR * 2, SR, 1, f_half);

  /* LFO 1 -> Pitch 50 %: grain pitches swing +-12 around the note */
  gc_init(&G, SR, 1);
  gc_set_source(&G, &s.src);
  gc_set_lfo(&G, 0, GC_LFO_SINE, 1);
  gc_set_mod_cell(&G, GC_SRC_LFO1, GC_DST_PITCH, 0.5f);
  p.lfo_rate[0] = 4.0f;
  gc_note_on(&G, &p, 60, 100);
  float lo = 1e9f, hi = -1e9f;
  for (int k = 0; k < SR / GC_BLOCK / 2; k++) {
    gc_process(&G, &p, L, R, GC_BLOCK);
    lo = fminf(lo, G.last_pitch); hi = fmaxf(hi, G.last_pitch);
  }
  CHECK(lo < 48.5f && lo >= 47.99f && hi > 71.5f && hi <= 72.01f);
  gc_set_mod_depth(&G, 0.0f);                                 /* Mod Depth 0: nothing */
  gc_process(&G, &p, L, R, GC_BLOCK * 4);
  CHECK(G.last_pitch == 60.0f);
  gc_set_mod_depth(&G, 1.0f);

  /* Env 1 -> Cutoff +100 %: decay from 5 octaves above the knob down to it */
  gc_init(&G, SR, 1);
  gc_set_source(&G, &s.src);
  gc_set_env(&G, 0, 0.0f);
  gc_set_mod_cell(&G, GC_SRC_ENV1, GC_DST_CUTOFF, 1.0f);
  p.cutoff_hz = 300; p.env_time[0] = 0.5f;
  gc_note_on(&G, &p, 60, 100);
  run(&p, 0.02, NULL);
  const float early = G.voices[0].fc_hz;
  run(&p, 0.6, NULL);
  CHECK(early > 4000 && fabsf(G.voices[0].fc_hz - 300) < 5);
  p.cutoff_hz = GC_CUTOFF_MAX;

  /* Velocity -> Level 100 %: gain 1 + vel */
  gc_init(&G, SR, 1);
  gc_set_source(&G, &s.src);
  gc_set_mod_cell(&G, GC_SRC_VEL, GC_DST_LEVEL, 1.0f);
  gc_note_on(&G, &p, 60, 127);
  gc_note_on(&G, &p, 64, 0 + 64);
  gc_process(&G, &p, L, R, GC_BLOCK);
  CHECK(fabsf(G.voices[0].level - 2.0f) < 1e-5f && fabsf(G.voices[1].level - (1.0f + 64 / 127.0f)) < 1e-5f);

  /* Mod Wheel -> Size +33 %: grains twice as long; -> Position +100 %: half a sample further */
  gc_init(&G, SR, 1);
  gc_set_source(&G, &s.src);
  gc_set_mod_cell(&G, GC_SRC_WHEEL, GC_DST_SIZE, 1.0f / 3.0f);
  gc_set_mod_cell(&G, GC_SRC_WHEEL, GC_DST_POSITION, 1.0f);
  gc_set_modwheel(&G, 1.0f);
  p.size_ms = 50; p.scan = 0; p.position = 0.1f;
  gc_note_on(&G, &p, 60, 100);
  gc_process(&G, &p, L, R, GC_BLOCK);
  CHECK(abs(G.grains[0].remain + GC_BLOCK - (int)(0.1f * SR)) <= 2);
  CHECK(fabs(G.last_start - (0.1 * (2 * SR - 1) + 0.5 * 2 * SR)) < 2.0);

  /* Velocity -> Density +33 %: twice as many grains; Mod Wheel -> Scan 50 %: moves at realtime */
  gc_init(&G, SR, 1);
  gc_set_source(&G, &s.src);
  p = poly_params(); p.density = 20; p.size_ms = 10; p.scan = 0.0f;
  gc_note_on(&G, &p, 60, 127);
  run(&p, 1.0, NULL);
  const uint64_t base = G.born;
  gc_init(&G, SR, 1);
  gc_set_source(&G, &s.src);
  gc_set_mod_cell(&G, GC_SRC_VEL, GC_DST_DENSITY, 1.0f / 3.0f);
  gc_set_mod_cell(&G, GC_SRC_WHEEL, GC_DST_SCAN, 0.5f);
  gc_set_modwheel(&G, 1.0f);
  gc_note_on(&G, &p, 60, 127);
  const double ph0 = G.voices[0].playhead;
  run(&p, 1.0, NULL);
  CHECK(G.born > base * 19 / 10 && G.born < base * 21 / 10);
  CHECK(fabs(G.voices[0].playhead - ph0 - SR) < 2 * BUF);

  /* Random -> Pan: grains land left and right without Stereo Spread */
  gc_init(&G, SR, 1);
  gc_set_source(&G, &s.src);
  gc_set_mod_cell(&G, GC_SRC_RANDOM, GC_DST_PAN, 1.0f);
  p = poly_params(); p.density = 200; p.size_ms = 5;
  gc_note_on(&G, &p, 60, 127);
  int left = 0, right = 0;
  for (int k = 0; k < 500; k++) {
    gc_process(&G, &p, L, R, GC_BLOCK);
    for (int i = 0; i < GC_POOL; i++)
      if (G.grains[i].active) { if (G.grains[i].gl > G.grains[i].gr * 1.5f) left++; if (G.grains[i].gr > G.grains[i].gl * 1.5f) right++; }
  }
  CHECK(left > 20 && right > 20);
  free_src(&s);
}

/* free LFOs: each voice its own phase (a chord moves apart); key sync: phase 0 on every note */
static void test_mod_voices(void) {
  GcParams p = poly_params();
  Src s = make_src(SR, SR, 1, f_half);
  gc_init(&G, SR, 1);
  gc_set_source(&G, &s.src);
  gc_set_mod_cell(&G, GC_SRC_LFO1, GC_DST_PAN, 0.5f);
  gc_note_on(&G, &p, 60, 100); gc_note_on(&G, &p, 64, 100); gc_note_on(&G, &p, 67, 100);
  gc_process(&G, &p, L, R, GC_BLOCK);
  CHECK(G.voices[0].src[GC_SRC_LFO1] != G.voices[1].src[GC_SRC_LFO1] &&
        G.voices[1].src[GC_SRC_LFO1] != G.voices[2].src[GC_SRC_LFO1]);
  gc_all_off(&G);
  gc_set_lfo(&G, 0, GC_LFO_SAW_UP, 1);
  gc_note_on(&G, &p, 60, 100); gc_note_on(&G, &p, 64, 100);
  gc_process(&G, &p, L, R, GC_BLOCK);
  CHECK(G.voices[0].src[GC_SRC_LFO1] == G.voices[1].src[GC_SRC_LFO1]);
  CHECK(fabsf(G.voices[0].src[GC_SRC_LFO1] - (2.0f * GC_BLOCK / SR * p.lfo_rate[0] - 1.0f)) < 1e-4f);
  /* S&H: one value per cycle */
  gc_set_lfo(&G, 0, GC_LFO_SH, 1);
  p.lfo_rate[0] = 10.0f;
  gc_all_off(&G); gc_note_on(&G, &p, 60, 100);
  float prev = 2.0f;
  int changes = 0;
  for (int k = 0; k < SR / GC_BLOCK; k++) {
    gc_process(&G, &p, L, R, GC_BLOCK);
    if (G.voices[0].src[GC_SRC_LFO1] != prev) { changes++; prev = G.voices[0].src[GC_SRC_LFO1]; }
  }
  CHECK(changes >= 9 && changes <= 11);
  free_src(&s);
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
  test_poly();
  test_steal();
  test_envelope();
  test_mono();
  test_mono_retrigger();
  test_glide();
  test_spread();
  test_pitch();
  test_quantize();
  test_reverse();
  test_filter();
  test_mod_shapes();
  test_mod_grid();
  test_mod_dest();
  test_mod_voices();
  printf(fails ? "%d FAILED\n" : "All grain tests passed\n", fails);
  return fails != 0;
}
