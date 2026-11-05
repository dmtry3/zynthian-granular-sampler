/* Stress test of grain_core (no LV2): random parameters across their whole ranges and at the edges,
 * random notes, voice modes, grid cells, tempo and sample swaps (down to 1 frame, and none),
 * random block sizes. Checks: every output sample finite and bounded, the pool count right,
 * old samples freed only when no grain reads them (build with -fsanitize=address to catch the rest).
 * Usage: test_stress [seconds of audio per seed, default 120] */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../src/grain_core.h"
#include "../src/sample.h"

static int fails = 0;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

#define SR 48000
#define MAXBLK 1024
#define BOUND 64.0f        /* far above anything musical: only blow-ups go past it */

static uint32_t rs;
static uint32_t rnd(void) { rs ^= rs << 13; rs ^= rs >> 17; rs ^= rs << 5; return rs; }
static float frand(void) { return (rnd() >> 8) * (1.0f / 16777216.0f); }
/* a value in lo..hi, often exactly at an edge */
static float edge(float lo, float hi) {
  const uint32_t r = rnd() % 8;
  return r == 0 ? lo : r == 1 ? hi : lo + (hi - lo) * frand();
}

/* Sources built like sample.c builds them, owned here; freed only when their generation is idle. */
typedef struct { float* lv[GC_LEVELS]; GcSource src; uint32_t gen; int live; } Src;
#define NSRC 16
static Src pool[NSRC];

static void guard(float* d, int64_t n) {
  for (int i = 0; i < GS_GUARD; i++) { d[2 * (n + i)] = d[2 * (i % n)]; d[2 * (n + i) + 1] = d[2 * (i % n) + 1]; }
}
static Src* make_src(int64_t n, float rate) {
  Src* s = NULL;
  for (int i = 0; i < NSRC; i++) if (!pool[i].live) { s = &pool[i]; break; }
  if (!s) return NULL;
  memset(s, 0, sizeof *s);
  s->lv[0] = malloc((size_t)(n + GS_GUARD) * 2 * sizeof(float));
  for (int64_t i = 0; i < n; i++) {                 /* noise at full scale: the loudest a sample can be */
    s->lv[0][2 * i] = frand() * 2.0f - 1.0f;
    s->lv[0][2 * i + 1] = frand() * 2.0f - 1.0f;
  }
  guard(s->lv[0], n);
  s->src.data[0] = s->lv[0]; s->src.frames[0] = n; s->src.nlevels = 1; s->src.rate = rate;
  for (int k = 1; k < GC_LEVELS; k++) {             /* as the loader: no level below 4 frames */
    const int64_t m = s->src.frames[k - 1] / 2;
    if (m < 4) break;
    s->lv[k] = malloc((size_t)(m + GS_GUARD) * 2 * sizeof(float));
    gs_decimate2(s->lv[k - 1], s->src.frames[k - 1], s->lv[k]);
    guard(s->lv[k], m);
    s->src.data[k] = s->lv[k]; s->src.frames[k] = m; s->src.nlevels = k + 1;
  }
  s->src.period = rnd() % 2 ? 2.0f + frand() * 2000.0f : 0.0f;   /* a pitch (in phase grains) or none */
  s->live = 1;
  return s;
}
static void free_src(Src* s) {
  for (int k = 0; k < GC_LEVELS; k++) free(s->lv[k]);
  memset(s, 0, sizeof *s);
}

static GrainCore G;
static Src* cur;                                    /* NULL = no sample */

static void swap(void) {
  static const int64_t lens[] = { 1, 2, 3, 4, 7, 8, 9, 31, 100, 1000, 48000, 200000 };
  static const float rates[] = { 8000, 22050, 44100, 48000, 96000, 192000 };
  Src* old = cur;
  if (old) old->gen = G.gen;
  if (rnd() % 6 == 0) cur = NULL;
  else {
    cur = make_src(lens[rnd() % (sizeof lens / sizeof lens[0])], rates[rnd() % 6]);
    if (!cur) { cur = old; return; }                /* every slot still fading: try later */
  }
  gc_set_source(&G, cur ? &cur->src : NULL);
}
static void free_idle(void) {
  for (int i = 0; i < NSRC; i++)
    if (pool[i].live && &pool[i] != cur && !gc_gen_busy(&G, pool[i].gen)) free_src(&pool[i]);
}

static void random_params(GcParams* p) {
  p->position = edge(0, 1); p->scan = edge(-GC_SCAN_MAX, GC_SCAN_MAX); p->spray = edge(0, 1);
  p->window = edge(0, 1);
  p->size_ms = edge(GC_SIZE_MIN, GC_SIZE_MAX); p->density = edge(GC_DENS_MIN, GC_DENS_MAX);
  p->shape = edge(0, 1); p->chaos = edge(0, 1);
  p->attack_ms = edge(GC_ATTACK_MIN, 200); p->release_ms = edge(GC_RELEASE_MIN, 2000);
  p->spread = edge(0, 1);
  p->pitch = edge(-GC_PITCH_MAX, GC_PITCH_MAX); p->pitch_spray = edge(0, GC_PSPRAY_MAX);
  p->reverse = edge(0, 1); p->quantize = edge(0, 1);
  p->cutoff_hz = edge(GC_CUTOFF_MIN, GC_CUTOFF_MAX); p->resonance = edge(0, 1);
  p->ftype = (int)(rnd() % GC_NFTYPES); p->keytrack = edge(0, 1);
  p->mark_start = rnd() % 3 ? edge(0, 1) : 0.0f; p->mark_end = rnd() % 3 ? edge(0, 1) : 1.0f;
  for (int i = 0; i < 2; i++) { p->lfo_rate[i] = edge(GC_LFO_MIN, GC_LFO_MAX); p->env_time[i] = edge(GC_ENV_MIN, 2); }
}

/* One random event: parameters, notes, settings or a sample swap. */
static void random_event(GcParams* p) {
  const uint32_t r = rnd() % 100;
  if (r < 30) gc_note_on(&G, p, (int)(rnd() % 128), 1 + (int)(rnd() % 127));
  else if (r < 55) gc_note_off(&G, (int)(rnd() % 128));
  else if (r < 65) random_params(p);
  else if (r < 72) gc_set_mod_cell(&G, (int)(rnd() % GC_NSRC), (int)(rnd() % GC_NDST), edge(-1, 1));
  else if (r < 74) gc_set_mod_depth(&G, edge(0, 1));
  else if (r < 76) gc_set_modwheel(&G, edge(0, 1));
  else if (r < 78) gc_set_lfo(&G, (int)(rnd() % 2), (int)(rnd() % GC_NLFO_SHAPES), (int)(rnd() % 3));
  else if (r < 79) gc_set_lfo_div(&G, (int)(rnd() % 2), (int)(rnd() % GC_NDIVS));
  else if (r < 80) gc_set_env(&G, (int)(rnd() % 2), edge(0, 1));
  else if (r < 82) gc_set_tempo(&G, edge(20, 300), rnd() % 2 ? -1.0 : (double)(rnd() % 1000) * 0.25);
  else if (r < 85) gc_set_voices(&G, 1 + (int)(rnd() % GC_VOICES), (int)(rnd() % 2));
  else if (r < 86) gc_set_glide(&G, edge(0, 2000));
  else if (r < 87) gc_set_scale(&G, (int)(rnd() % 12), (int)(rnd() % GC_NSCALES));
  else if (r < 88) G.interp = (int)(rnd() % 2);
  else if (r < 93) swap();
  else if (r < 94) gc_all_off(&G);
  else if (r < 95) gc_all_notes_off(&G);
  else if (r < 97) gc_set_sustain(&G, (int)(rnd() % 2));
  else if (r < 98) gc_set_bend(&G, edge(-GC_BEND_MAX, GC_BEND_MAX));
  else if (r < 99) {
    if (rnd() % 2) gc_set_xmod_cell(&G, (int)(rnd() % GC_NSRC), (int)(rnd() % GC_NXDST), edge(-1, 1));
    else gc_set_lfo_uni(&G, (int)(rnd() % 2), (int)(rnd() % 2));
  }
  /* else: nothing this time */
}

static float L[MAXBLK], R[MAXBLK];

static void run_seed(uint32_t seed, double secs) {
  rs = seed ? seed : 1;
  gc_init(&G, SR, seed);
  GcParams p;
  gc_default_params(&p);
  cur = NULL;
  swap();
  long bad = 0, over = 0, count_bad = 0;
  float peak = 0;
  uint64_t frames = 0;
  while (frames < (uint64_t)(secs * SR)) {
    const int nev = (int)(rnd() % 4);
    for (int e = 0; e < nev; e++) random_event(&p);
    const uint32_t n = rnd() % 4 == 0 ? 1 + rnd() % 7 : 1 + rnd() % MAXBLK;   /* tiny blocks too */
    gc_process(&G, &p, L, R, n);
    for (uint32_t i = 0; i < n; i++) {
      if (!isfinite(L[i]) || !isfinite(R[i])) { bad++; continue; }
      const float a = fmaxf(fabsf(L[i]), fabsf(R[i]));
      if (a > peak) peak = a;
      if (a > BOUND) over++;
    }
    int act = 0;
    for (int i = 0; i < GC_POOL; i++) act += G.grains[i].active != 0;
    if (act != G.nactive || G.nactive > GC_POOL) count_bad++;
    free_idle();
    frames += n;
  }
  printf("seed %u: %.0f s, peak %.2f, %llu grains born, %llu skipped\n", seed, secs, peak,
         (unsigned long long)G.born, (unsigned long long)G.skipped);
  CHECK(bad == 0);
  CHECK(over == 0);
  CHECK(count_bad == 0);
  CHECK(G.born > 0);
  /* all off: silence at once, every old sample can go */
  gc_all_off(&G);
  gc_process(&G, &p, L, R, 64);
  float mx = 0;
  for (int i = 0; i < 64; i++) mx = fmaxf(mx, fabsf(L[i]) + fabsf(R[i]));
  CHECK(mx == 0.0f && G.nactive == 0);
  free_idle();
  int live = 0;
  for (int i = 0; i < NSRC; i++) live += pool[i].live && &pool[i] != cur;
  CHECK(live == 0);
  for (int i = 0; i < NSRC; i++) if (pool[i].live) free_src(&pool[i]);
}

int main(int argc, char** argv) {
  const double secs = argc > 1 ? atof(argv[1]) : 120.0;
  for (uint32_t seed = 1; seed <= 4; seed++) run_seed(seed * 2654435761u, secs);
  printf(fails ? "%d FAILED\n" : "All stress tests passed\n", fails);
  return fails != 0;
}
