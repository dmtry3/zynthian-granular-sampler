#include "grain_core.h"
#include <math.h>
#include <string.h>
#if defined(__x86_64__) || defined(__i386__)
#include <xmmintrin.h>
#endif

static float clampf(float v, float lo, float hi) { return v < lo ? lo : v > hi ? hi : v; }

static float rnd(GrainCore* g) {          /* xorshift32 -> [0, 1) */
  uint32_t x = g->rng;
  x ^= x << 13; x ^= x >> 17; x ^= x << 5;
  g->rng = x;
  return (float)(x >> 8) * (1.0f / 16777216.0f);
}

void gc_enable_ftz(void) {
#if defined(__aarch64__)
  uint64_t fpcr;
  __asm__ volatile("mrs %0, fpcr" : "=r"(fpcr));
  __asm__ volatile("msr fpcr, %0" : : "r"(fpcr | (1u << 24)));          /* FZ */
#elif defined(__x86_64__) || defined(__i386__)
  _mm_setcsr(_mm_getcsr() | 0x8040);                                     /* FTZ | DAZ */
#endif
}

/* Window families, blended by s in 0..1: Gauss(narrow) 0 -> Hann 0.5 -> Tukey 50% 0.75 -> Tukey 10% 1. */
static double tukey(double x, double a) {
  if (x < a / 2) return 0.5 * (1 - cos(2 * M_PI * x / a));
  if (x > 1 - a / 2) return 0.5 * (1 - cos(2 * M_PI * (1 - x) / a));
  return 1.0;
}
static double window_value(double x, double s) {
  const double gauss = exp(-0.5 * pow((x - 0.5) / 0.12, 2.0));
  const double hann = 0.5 * (1 - cos(2 * M_PI * x));
  const double t50 = tukey(x, 0.5), t10 = tukey(x, 0.1);
  if (s < 0.5) { double t = s * 2; return gauss + t * (hann - gauss); }
  if (s < 0.75) { double t = (s - 0.5) * 4; return hann + t * (t50 - hann); }
  double t = (s - 0.75) * 4; return t50 + t * (t10 - t50);
}

void gc_init(GrainCore* g, float sr, uint32_t seed) {
  memset(g, 0, sizeof *g);
  g->sr = sr;
  g->rng = seed ? seed : 0x9E3779B9u;
  g->voice.note = -1;
  g->rate_ratio = 1.0f;
  for (int s = 0; s < GC_SHAPES; s++) {
    for (int i = 0; i <= GC_WIN; i++)
      g->win[s][i] = (float)window_value((double)i / GC_WIN, (double)s / (GC_SHAPES - 1));
    g->win[s][0] = g->win[s][GC_WIN] = 0.0f;   /* the Gauss tails start at ~0.0003: make the ends exact */
  }
}

void gc_default_params(GcParams* p) {
  p->position = 0.0f; p->scan = 1.0f; p->spray = 0.0f; p->window = 1.0f;
  p->size_ms = 100.0f; p->density = 20.0f; p->shape = 0.5f; p->chaos = 0.0f;
  p->root = 60.0f;
}

void gc_set_source(GrainCore* g, const float* l, const float* r, int64_t frames, float file_rate) {
  for (int i = 0; i < GC_POOL; i++) g->grains[i].active = 0;
  g->nactive = 0;
  if (!l || frames < 2) { g->data[0] = g->data[1] = NULL; g->frames = 0; return; }
  g->data[0] = l; g->data[1] = r ? r : l;
  g->frames = frames;
  g->rate_ratio = file_rate > 0 ? file_rate / g->sr : 1.0f;
}

static double window_start(const GrainCore* g, const GcParams* p) {
  return (double)clampf(p->position, 0.0f, 1.0f) * (double)(g->frames - 1);
}
static double window_len(const GrainCore* g, const GcParams* p) {
  double w = (double)clampf(p->window, GC_WINDOW_MIN, 1.0f) * (double)g->frames;
  return w < 2.0 ? 2.0 : w;
}
static double wrap(double x, double len) {
  x = fmod(x, len);
  return x < 0 ? x + len : x;
}

void gc_note_on(GrainCore* g, const GcParams* p, int note, int vel) {
  GcVoice* v = &g->voice;
  v->note = note; v->gate = 1;
  v->vel = clampf(vel / 127.0f, 0.0f, 1.0f);
  v->playhead = window_start(g, p);      /* Scan: every note starts reading at Position */
  v->next = 0.0f;                        /* first grain right away */
}

void gc_note_off(GrainCore* g, int note) {
  if (g->voice.note == note) g->voice.gate = 0;
}

void gc_all_off(GrainCore* g) {
  g->voice.gate = 0;
  for (int i = 0; i < GC_POOL; i++) g->grains[i].active = 0;
  g->nactive = 0;
}

static void spawn(GrainCore* g, const GcParams* p, int delay) {
  GcVoice* v = &g->voice;
  if (g->nactive >= GC_POOL) { g->skipped++; return; }
  GcGrain* gr = NULL;
  for (int i = 0; i < GC_POOL; i++) if (!g->grains[i].active) { gr = &g->grains[i]; break; }
  if (!gr) { g->skipped++; return; }

  const float size_s = clampf(p->size_ms, GC_SIZE_MIN, GC_SIZE_MAX) * 0.001f;
  const float dens = clampf(p->density, GC_DENS_MIN, GC_DENS_MAX);
  const double wlen = window_len(g, p);
  double start = v->playhead + (double)((rnd(g) * 2.0f - 1.0f) * clampf(p->spray, 0.0f, 1.0f) * 0.5f) * wlen;
  start = wrap(start, (double)g->frames);
  const int len = (int)(size_s * g->sr) > 1 ? (int)(size_s * g->sr) : 1;
  const float overlap = dens * size_s;

  g->last_start = start;
  gr->active = 1;
  gr->delay = delay;
  gr->remain = len;
  gr->idx = (int64_t)start;
  gr->frac = (float)(start - (double)gr->idx);
  gr->inc = g->rate_ratio * exp2f(((float)v->note - p->root) / 12.0f);
  gr->win = g->win[(int)lrintf(clampf(p->shape, 0.0f, 1.0f) * (GC_SHAPES - 1))];
  gr->wph = 0.0f;
  gr->wstep = (float)GC_WIN / (float)len;
  gr->gain = v->vel / sqrtf(overlap > 1.0f ? overlap : 1.0f);   /* keeps loudness when grains overlap */
  g->nactive++;
  g->born++;
}

/* Next grain distance in samples: even at chaos 0, exponential (Poisson) at chaos 1. */
static float next_interval(GrainCore* g, const GcParams* p) {
  const float base = g->sr / clampf(p->density, GC_DENS_MIN, GC_DENS_MAX);
  const float c = clampf(p->chaos, 0.0f, 1.0f);
  float j = 1.0f;
  if (c > 0.0f) {
    float u = rnd(g);
    float e = -logf(u > 1e-6f ? u : 1e-6f);
    if (e > 4.0f) e = 4.0f;
    j = 1.0f + c * (e - 1.0f);
  }
  return base * j;
}

/* Adds one grain into out[0..m). */
static void render_grain(GrainCore* g, GcGrain* gr, float* restrict ol, float* restrict orr, int m) {
  const int d = gr->delay;
  gr->delay = 0;
  if (d >= m) { gr->delay = d - m; return; }
  int cnt = m - d;
  if (cnt > gr->remain) cnt = gr->remain;
  const float* restrict L = g->data[0];
  const float* restrict R = g->data[1];
  const float* restrict w = gr->win;
  const int64_t frames = g->frames;
  const float inc = gr->inc, gain = gr->gain, wstep = gr->wstep;
  const float frac = gr->frac, wph = gr->wph;
  const int64_t idx = gr->idx;
  float* restrict dl = ol + d;
  float* restrict dr = orr + d;

  if (idx + (int64_t)(frac + (float)cnt * inc) + 2 < frames) {
    for (int i = 0; i < cnt; i++) {                       /* fast path: no wrap inside this block */
      const float pf = frac + (float)i * inc;
      const int k = (int)pf;
      const float f = pf - (float)k;
      const int64_t j = idx + k;
      const float a = gain * w[(int)(wph + (float)i * wstep)];
      dl[i] += a * (L[j] + f * (L[j + 1] - L[j]));
      dr[i] += a * (R[j] + f * (R[j + 1] - R[j]));
    }
  } else {
    for (int i = 0; i < cnt; i++) {                       /* reaches the end: wrap to the start */
      const float pf = frac + (float)i * inc;
      const int k = (int)pf;
      const float f = pf - (float)k;
      int64_t j = (idx + k) % frames;
      int64_t j1 = j + 1 < frames ? j + 1 : 0;
      const float a = gain * w[(int)(wph + (float)i * wstep)];
      dl[i] += a * (L[j] + f * (L[j1] - L[j]));
      dr[i] += a * (R[j] + f * (R[j1] - R[j]));
    }
  }
  const float total = frac + (float)cnt * inc;
  const int64_t adv = (int64_t)total;
  gr->idx = (idx + adv) % frames;
  gr->frac = total - (float)adv;
  gr->wph = wph + (float)cnt * wstep;
  if (gr->wph > (float)GC_WIN) gr->wph = (float)GC_WIN;
  gr->remain -= cnt;
  if (gr->remain <= 0) { gr->active = 0; g->nactive--; }
}

void gc_process(GrainCore* g, const GcParams* p, float* out_l, float* out_r, uint32_t n) {
  memset(out_l, 0, n * sizeof(float));
  memset(out_r, 0, n * sizeof(float));
  if (!g->frames) return;
  GcVoice* v = &g->voice;
  const double wstart = window_start(g, p), wlen = window_len(g, p);
  float scan = clampf(p->scan, -GC_SCAN_MAX, GC_SCAN_MAX);
  if (fabsf(scan) < GC_SCAN_DEAD) scan = 0.0f;

  for (uint32_t off = 0; off < n; off += GC_BLOCK) {
    const int m = (int)(n - off < GC_BLOCK ? n - off : GC_BLOCK);
    if (v->note >= 0) {
      while (v->gate && v->next < (float)m) {
        spawn(g, p, v->next > 0.0f ? (int)v->next : 0);
        v->next += next_interval(g, p);
      }
      v->next -= (float)m;
      if (v->next < 0.0f) v->next = 0.0f;
      /* the playhead loops inside the window */
      v->playhead = wstart + wrap(v->playhead + (double)(scan * g->rate_ratio * (float)m) - wstart, wlen);
      if (!v->gate && g->nactive == 0) v->note = -1;
    }
    if (!g->nactive) continue;
    for (int i = 0; i < GC_POOL; i++)
      if (g->grains[i].active) render_grain(g, &g->grains[i], out_l + off, out_r + off, m);
  }
}
