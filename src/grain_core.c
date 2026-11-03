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

/* Grain envelopes after Mutable Instruments Beads (grain_envelope.h): attack, sustain and release
 * with a smoothstep blend. Shape 0 = square, 1/3 = decay (fast attack, t^4 release), 2/3 = bell,
 * 1 = reverse. Beads keeps edges >= 0.5 ms / 10 ms; tables are normalized to the grain length,
 * so the edges are kept >= 2 % / 4 % of the grain instead (a square window would click). */
#define EDGE_ATTACK  0.02
#define EDGE_RELEASE 0.04
static double smoothstep(double t) { return t * t * (3.0 - 2.0 * t); }
static void envelope_shape(double shape, double* att, double* sus, double* rel, double* smooth) {
  const double sustain = fmax(1.0 - 3.0 * shape, 0.0);
  double s = (shape - 1.0 / 3.0) * 1.5;
  if (s < 0.0) s = 0.0;
  else if (s < 0.5) s = s * s * s * 4.0;
  const double attack = s;
  const double release = (1.0 - attack) * (1.0 - sustain);
  *smooth = attack < 0.5 ? 2.0 * attack : 2.0 - 2.0 * attack;
  double a = fmax(attack, EDGE_ATTACK), r = fmax(release, EDGE_RELEASE);
  double u = sustain;
  const double total = a + u + r;                   /* minimum edges: take the time from the others */
  if (total > 1.0) {
    const double extra = total - 1.0;
    if (u >= extra) u -= extra;
    else {                                          /* no sustain left: shorten the longer edge */
      const double over = extra - u;
      u = 0.0;
      if (a > r) a -= over; else r -= over;
    }
  }
  *att = a; *sus = u; *rel = 1.0 - a - u;
}
static double window_value(double x, double shape) {
  double a, u, r, sm;
  envelope_shape(shape, &a, &u, &r, &sm);
  if (x <= 0.0 || x >= 1.0) return 0.0;
  if (x < a) { const double t = x / a; return t + (smoothstep(t) - t) * sm; }
  if (x < a + u) return 1.0;
  const double t = (1.0 - x) / r;                   /* 1 -> 0 over the release */
  const double base = (t * t) * (t * t);
  return base + (smoothstep(t) - base) * sm;
}

void gc_init(GrainCore* g, float sr, uint32_t seed) {
  memset(g, 0, sizeof *g);
  g->sr = sr;
  g->rng = seed ? seed : 0x9E3779B9u;
  g->voice.note = -1;
  g->interp = GC_INTERP_LINEAR;
  g->use_levels = 1;
  double rms[GC_SHAPES];
  for (int s = 0; s < GC_SHAPES; s++) {
    double e = 0;
    for (int i = 0; i <= GC_WIN; i++) {
      g->win[s][i] = (float)window_value((double)i / GC_WIN, (double)s / (GC_SHAPES - 1));
      e += (double)g->win[s][i] * g->win[s][i];
    }
    rms[s] = sqrt(e / GC_WIN);
  }
  /* Overlapping grains add up by power: a window with less energy than Hann gets louder, up to 2x. */
  for (int s = 0; s < GC_SHAPES; s++) {
    const double k = rms[GC_SHAPE_BELL] / rms[s];
    g->win_gain[s] = (float)(k < 0.5 ? 0.5 : k > 2.0 ? 2.0 : k);
  }
}

void gc_default_params(GcParams* p) {
  p->position = 0.0f; p->scan = 1.0f; p->spray = 0.0f; p->window = 1.0f;
  p->size_ms = 100.0f; p->density = 20.0f; p->shape = 2.0f / 3.0f; p->chaos = 0.0f;
  p->root = 60.0f;
}

uint32_t gc_set_source(GrainCore* g, const GcSource* s) {
  for (int i = 0; i < GC_POOL; i++) {
    GcGrain* gr = &g->grains[i];
    if (!gr->active || gr->fstep < 0.0f) continue;
    if (gr->remain > GC_FADE) gr->remain = GC_FADE;
    gr->fstep = -gr->fade / (float)gr->remain;
  }
  g->gen++;
  memset(&g->src, 0, sizeof g->src);
  if (s && s->nlevels > 0 && s->data[0] && s->frames[0] >= 2) {
    g->src = *s;
    if (g->src.nlevels > GC_LEVELS) g->src.nlevels = GC_LEVELS;
    while (g->src.nlevels > 1 && (!s->data[g->src.nlevels - 1] || s->frames[g->src.nlevels - 1] < 2))
      g->src.nlevels--;
  }
  return g->gen;
}

int gc_gen_busy(const GrainCore* g, uint32_t gen) {
  int n = 0;
  for (int i = 0; i < GC_POOL; i++) if (g->grains[i].active && g->grains[i].gen == gen) n++;
  return n;
}

static double window_start(const GrainCore* g, const GcParams* p) {
  return (double)clampf(p->position, 0.0f, 1.0f) * (double)(g->src.frames[0] - 1);
}
static double window_len(const GrainCore* g, const GcParams* p) {
  double w = (double)clampf(p->window, GC_WINDOW_MIN, 1.0f) * (double)g->src.frames[0];
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
  v->playhead = g->src.frames[0] ? window_start(g, p) : 0.0;   /* every note starts reading at Position */
  v->next = 0.0f;                                              /* first grain right away */
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
  start = wrap(start, (double)g->src.frames[0]);
  g->last_start = start;
  const int len = (int)(size_s * g->sr) > 1 ? (int)(size_s * g->sr) : 1;

  /* Pitched up: read a band-limited level at a lower rate, so nothing folds back above Nyquist. */
  float inc = (g->src.rate > 0 ? g->src.rate / g->sr : 1.0f) * exp2f(((float)v->note - p->root) / 12.0f);
  int lv = 0;
  while (g->use_levels && lv + 1 < g->src.nlevels && inc > GC_MIP_UP) { inc *= 0.5f; start *= 0.5; lv++; }
  const int64_t frames = g->src.frames[lv];
  /* Do not run over the end of the sample (a jump to its start would click): start earlier if it fits. */
  const double span = (double)len * inc + 3.0;
  if (span < (double)frames - 2.0 && start > (double)frames - 2.0 - span) start = (double)frames - 2.0 - span;
  if (start < 0.0) start = 0.0;

  /* Loudness: grains add up by power. The overlap cannot exceed the pool. */
  float overlap = dens * size_s;
  if (overlap > (float)GC_POOL) overlap = (float)GC_POOL;
  const int shape = (int)lrintf(clampf(p->shape, 0.0f, 1.0f) * (GC_SHAPES - 1));

  gr->active = 1;
  gr->delay = delay;
  gr->remain = len;
  gr->gen = g->gen;
  gr->data = g->src.data[lv];
  gr->frames = frames;
  gr->idx = (int64_t)start;
  gr->frac = (float)(start - (double)gr->idx);
  gr->inc = inc;
  gr->win = g->win[shape];
  gr->wph = 0.0f;
  gr->wstep = (float)GC_WIN / (float)len;
  gr->gain = v->vel * g->win_gain[shape] / sqrtf(overlap > 1.0f ? overlap : 1.0f);
  gr->fade = 1.0f;
  gr->fstep = 0.0f;
  g->last_level = lv;
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

/* 4-point, 3rd-order Hermite (Laurent de Soras), as in Mutable Instruments Clouds/Beads. */
static inline float hermite(float xm1, float x0, float x1, float x2, float t) {
  const float c = (x1 - xm1) * 0.5f;
  const float v = x0 - x1;
  const float w = c + v;
  const float a = w + v + (x2 - x0) * 0.5f;
  const float b = w + a;
  return ((a * t - b) * t + c) * t + x0;
}

/* Adds one grain into out[0..m). Sample data is interleaved stereo. */
static void render_grain(GrainCore* g, GcGrain* gr, float* restrict ol, float* restrict orr, int m) {
  const int d = gr->delay;
  if (d >= m) { gr->delay = d - m; return; }
  gr->delay = 0;
  int cnt = m - d;
  if (cnt > gr->remain) cnt = gr->remain;
  const float* restrict s = gr->data;
  const float* restrict w = gr->win;
  const int64_t frames = gr->frames;
  const float inc = gr->inc, gain = gr->gain, wstep = gr->wstep, fstep = gr->fstep;
  const float frac = gr->frac, wph = gr->wph, fade = gr->fade;
  const int64_t idx = gr->idx;
  float* restrict dl = ol + d;
  float* restrict dr = orr + d;
  const int64_t last = idx + (int64_t)(frac + (float)cnt * inc);    /* last integer frame read */
  /* fast path: all frames needed (j-1 .. j+2) exist without wrapping; levels end with guard frames */
  const int fast = idx >= 1 && last + 2 < frames + 2;

  const int hermite_on = g->interp == GC_INTERP_HERMITE;

  if (!fast || fstep != 0.0f) {
    /* general path: wraps at the end (short samples) and fades out (sample swap); rarely used */
    for (int i = 0; i < cnt; i++) {
      const float pf = frac + (float)i * inc;
      const int k = (int)pf;
      const float t = pf - (float)k;
      int64_t j = idx + k, jm = j - 1;
      if (!fast) { j %= frames; jm = j ? j - 1 : frames - 1; }   /* guard frames cover j+1, j+2 */
      const float* p = s + 2 * j;
      const float a = gain * w[(int)(wph + (float)i * wstep)] * (fade + (float)i * fstep);
      if (hermite_on) {
        const float* q = s + 2 * jm;
        dl[i] += a * hermite(q[0], p[0], p[2], p[4], t);
        dr[i] += a * hermite(q[1], p[1], p[3], p[5], t);
      } else {
        dl[i] += a * (p[0] + t * (p[2] - p[0]));
        dr[i] += a * (p[1] + t * (p[3] - p[1]));
      }
    }
  } else if (hermite_on) {
    for (int i = 0; i < cnt; i++) {
      const float pf = frac + (float)i * inc;
      const int k = (int)pf;
      const float t = pf - (float)k;
      /* indexed loads: with a pointer and p[-2] gcc made this loop 2x slower (measured) */
      const int64_t j = idx + k, jm = j - 1, j1 = j + 1, j2 = j + 2;
      const float a = gain * w[(int)(wph + (float)i * wstep)];
      dl[i] += a * hermite(s[2 * jm], s[2 * j], s[2 * j1], s[2 * j2], t);
      dr[i] += a * hermite(s[2 * jm + 1], s[2 * j + 1], s[2 * j1 + 1], s[2 * j2 + 1], t);
    }
  } else {
    for (int i = 0; i < cnt; i++) {
      const float pf = frac + (float)i * inc;
      const int k = (int)pf;
      const float t = pf - (float)k;
      const float* p = s + 2 * (idx + k);
      const float a = gain * w[(int)(wph + (float)i * wstep)];
      dl[i] += a * (p[0] + t * (p[2] - p[0]));
      dr[i] += a * (p[1] + t * (p[3] - p[1]));
    }
  }
  const float total = frac + (float)cnt * inc;
  const int64_t adv = (int64_t)total;
  gr->idx = (idx + adv) % frames;
  gr->frac = total - (float)adv;
  gr->wph = wph + (float)cnt * wstep;
  if (gr->wph > (float)GC_WIN) gr->wph = (float)GC_WIN;
  gr->fade = fade + (float)cnt * fstep;
  gr->remain -= cnt;
  if (gr->remain <= 0) { gr->active = 0; g->nactive--; }
}

void gc_process(GrainCore* g, const GcParams* p, float* out_l, float* out_r, uint32_t n) {
  memset(out_l, 0, n * sizeof(float));
  memset(out_r, 0, n * sizeof(float));
  GcVoice* v = &g->voice;
  const int have = g->src.frames[0] > 0;
  if (!have && !g->nactive) return;
  const double wstart = have ? window_start(g, p) : 0.0, wlen = have ? window_len(g, p) : 1.0;
  float scan = clampf(p->scan, -GC_SCAN_MAX, GC_SCAN_MAX);
  if (fabsf(scan) < GC_SCAN_DEAD) scan = 0.0f;
  const float ratio = g->src.rate > 0 ? g->src.rate / g->sr : 1.0f;

  for (uint32_t off = 0; off < n; off += GC_BLOCK) {
    const int m = (int)(n - off < GC_BLOCK ? n - off : GC_BLOCK);
    if (v->note >= 0) {
      while (have && v->gate && v->next < (float)m) {
        spawn(g, p, v->next > 0.0f ? (int)v->next : 0);
        v->next += next_interval(g, p);
      }
      v->next -= (float)m;
      if (v->next < 0.0f) v->next = 0.0f;
      /* the playhead loops inside the window */
      if (have) v->playhead = wstart + wrap(v->playhead + (double)(scan * ratio * (float)m) - wstart, wlen);
      if ((!v->gate || !have) && g->nactive == 0) v->note = -1;
    }
    if (!g->nactive) continue;
    for (int i = 0; i < GC_POOL; i++)
      if (g->grains[i].active) render_grain(g, &g->grains[i], out_l + off, out_r + off, m);
  }
}
