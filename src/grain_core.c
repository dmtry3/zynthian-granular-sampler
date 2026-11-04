#include "grain_core.h"
#include <math.h>
#include <string.h>
#if defined(__x86_64__) || defined(__i386__)
#include <xmmintrin.h>
#endif

static float clampf(float v, float lo, float hi) { return v < lo ? lo : v > hi ? hi : v; }

/* Scales for Quantize: semitones above the tonic (the same list as in Generative Arp). */
const char* const gc_scale_names[GC_NSCALES] = {
  "Chromatic", "Major", "Minor", "Dorian", "Phrygian", "Lydian", "Mixolydian",
  "Harm. minor", "Mel. minor", "Major pent.", "Minor pent.", "Blues" };
static const int8_t scale_steps[GC_NSCALES][13] = {   /* -1 ends the list */
  { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, -1 }, { 0, 2, 4, 5, 7, 9, 11, -1 }, { 0, 2, 3, 5, 7, 8, 10, -1 },
  { 0, 2, 3, 5, 7, 9, 10, -1 }, { 0, 1, 3, 5, 7, 8, 10, -1 }, { 0, 2, 4, 6, 7, 9, 11, -1 },
  { 0, 2, 4, 5, 7, 9, 10, -1 }, { 0, 2, 3, 5, 7, 8, 11, -1 }, { 0, 2, 3, 5, 7, 9, 11, -1 },
  { 0, 2, 4, 7, 9, -1 }, { 0, 3, 5, 7, 10, -1 }, { 0, 3, 5, 6, 7, 10, -1 } };

void gc_set_scale(GrainCore* g, int key, int scale) {
  g->key = ((key % 12) + 12) % 12;
  g->scale = scale < 0 || scale >= GC_NSCALES ? 0 : scale;
}

float gc_snap(const GrainCore* g, float semis) {
  const float rel = semis - (float)g->key;
  const float oct = floorf(rel / 12.0f);
  float best = rel, dist = 1e9f;
  for (int o = -1; o <= 1; o++)
    for (const int8_t* d = scale_steps[g->scale]; *d >= 0; d++) {
      const float c = (oct + (float)o) * 12.0f + (float)*d;
      const float e = fabsf(c - rel);
      if (e < dist) { dist = e; best = c; }
    }
  return best + (float)g->key;
}

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
  for (int i = 0; i < GC_VOICES; i++) g->voices[i].note = -1;
  g->nvoices = GC_VOICES;
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
  p->attack_ms = 10.0f; p->release_ms = 500.0f; p->spread = 0.0f;
  p->pitch = 0.0f; p->pitch_spray = 0.0f; p->reverse = 0.0f; p->quantize = 0.0f;
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

/* ---------- voices ---------- */
static void kill_voice_grains(GrainCore* g, int v) {
  for (int i = 0; i < GC_POOL; i++)
    if (g->grains[i].active && g->grains[i].voice == v) { g->grains[i].active = 0; g->nactive--; }
}

/* Starts a note on a voice; the envelope rises from where it is (a retrigger does not click). */
static void voice_start(GrainCore* g, const GcParams* p, int vi, int note, int vel) {
  GcVoice* v = &g->voices[vi];
  v->stage = GC_V_ATTACK;
  v->note = note; v->gate = 1;
  v->vel = clampf(vel / 127.0f, 0.0f, 1.0f);
  v->playhead = g->src.frames[0] ? window_start(g, p) : 0.0;   /* every note starts reading at Position */
  v->next = 0.0f;                                              /* first grain right away */
  v->age = ++g->age;
  g->last_voice = vi;
}

static void voice_release(GcVoice* v) {
  v->gate = 0;
  if (v->stage == GC_V_ATTACK || v->stage == GC_V_SUSTAIN) v->stage = GC_V_RELEASE;
}

/* Poly: the voice to steal: the quietest one in release, else the oldest. */
static int steal_candidate(const GrainCore* g) {
  int best = -1;
  for (int i = 0; i < g->nvoices; i++)
    if (g->voices[i].stage == GC_V_RELEASE && (best < 0 || g->voices[i].env < g->voices[best].env)) best = i;
  if (best >= 0) return best;
  for (int i = 0; i < g->nvoices; i++)
    if (best < 0 || g->voices[i].age < g->voices[best].age) best = i;
  return best;
}

static void held_remove(GrainCore* g, int note) {
  int k = 0;
  for (int i = 0; i < g->nheld; i++) if (g->held[i] != note) g->held[k++] = g->held[i];
  g->nheld = k;
}

void gc_note_on(GrainCore* g, const GcParams* p, int note, int vel) {
  if (g->mono) {
    held_remove(g, note);
    if (g->nheld == GC_HELD) { memmove(g->held, g->held + 1, sizeof(int) * (GC_HELD - 1)); g->nheld--; }
    g->held[g->nheld++] = note;
    GcVoice* v = &g->voices[0];
    if (v->gate && (v->stage == GC_V_ATTACK || v->stage == GC_V_SUSTAIN)) {
      v->note = note;                     /* legato: new pitch for new grains, envelope and playhead go on */
      return;
    }
    voice_start(g, p, 0, note, vel);
    return;
  }
  for (int i = 0; i < g->nvoices; i++)    /* the same key again: its own voice */
    if (g->voices[i].note == note && g->voices[i].stage != GC_V_IDLE && g->voices[i].stage != GC_V_STEAL) {
      voice_start(g, p, i, note, vel);
      return;
    }
  for (int i = 0; i < g->nvoices; i++)
    if (g->voices[i].stage == GC_V_IDLE) { voice_start(g, p, i, note, vel); return; }
  const int i = steal_candidate(g);
  GcVoice* v = &g->voices[i];
  if (v->stage != GC_V_STEAL) {
    v->stage = GC_V_STEAL;
    v->steal_step = v->env / (float)GC_FADE;
    if (v->steal_step <= 0.0f) v->steal_step = 1.0f;
  }
  v->gate = 0;
  v->pending_note = note; v->pending_vel = vel; v->pending_off = 0;
  v->age = ++g->age;
}

void gc_note_off(GrainCore* g, int note) {
  if (g->mono) {
    held_remove(g, note);
    GcVoice* v = &g->voices[0];
    if (v->note != note || !v->gate) return;
    if (g->nheld > 0) v->note = g->held[g->nheld - 1];   /* back to the key still held */
    else voice_release(v);
    return;
  }
  for (int i = 0; i < GC_VOICES; i++) {
    GcVoice* v = &g->voices[i];
    if (v->stage == GC_V_STEAL && v->pending_note == note) v->pending_off = 1;
    else if (v->note == note && v->gate) voice_release(v);
  }
}

void gc_all_notes_off(GrainCore* g) {
  g->nheld = 0;
  for (int i = 0; i < GC_VOICES; i++) {
    voice_release(&g->voices[i]);
    if (g->voices[i].stage == GC_V_STEAL) g->voices[i].pending_off = 1;
  }
}

void gc_all_off(GrainCore* g) {
  g->nheld = 0;
  for (int i = 0; i < GC_VOICES; i++) {
    GcVoice* v = &g->voices[i];
    v->stage = GC_V_IDLE; v->gate = 0; v->note = -1; v->env = 0.0f;
  }
  for (int i = 0; i < GC_POOL; i++) g->grains[i].active = 0;
  g->nactive = 0;
}

void gc_set_voices(GrainCore* g, int n, int mono) {
  n = n < 1 ? 1 : n > GC_VOICES ? GC_VOICES : n;
  if (mono == g->mono && n == g->nvoices) return;
  if (mono != g->mono) gc_all_notes_off(g);   /* switching mode: everything releases */
  g->mono = mono;
  g->nvoices = mono ? 1 : n;
}

int gc_active_voices(const GrainCore* g) {
  int n = 0;
  for (int i = 0; i < GC_VOICES; i++) if (g->voices[i].stage != GC_V_IDLE) n++;
  return n;
}

static void spawn(GrainCore* g, const GcParams* p, int vi, int delay) {
  GcVoice* v = &g->voices[vi];
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

  /* Pitch: note + Pitch + random Pitch Spray, pulled to the scale by Quantize; fixed for the grain. */
  float semis = (float)v->note + clampf(p->pitch, -GC_PITCH_MAX, GC_PITCH_MAX);
  const float ps = clampf(p->pitch_spray, 0.0f, GC_PSPRAY_MAX);
  if (ps > 0.0f) semis += ps * (rnd(g) * 2.0f - 1.0f);
  const float q = clampf(p->quantize, 0.0f, 1.0f);
  if (q > 0.0f) semis += q * (gc_snap(g, semis) - semis);
  g->last_pitch = semis;
  const int backwards = p->reverse > 0.0f && rnd(g) < p->reverse;

  /* Pitched up: read a band-limited level at a lower rate, so nothing folds back above Nyquist. */
  float inc = (g->src.rate > 0 ? g->src.rate / g->sr : 1.0f) * exp2f((semis - p->root) / 12.0f);
  int lv = 0;
  while (g->use_levels && lv + 1 < g->src.nlevels && inc > GC_MIP_UP) { inc *= 0.5f; start *= 0.5; lv++; }
  const int64_t frames = g->src.frames[lv];
  /* Do not run over the end (forward) or the start (backwards) of the sample: a jump would click.
   * Move the start if the grain fits in the sample at all. */
  const double span = (double)len * inc + 3.0;
  if (span < (double)frames - 2.0) {
    if (!backwards && start > (double)frames - 2.0 - span) start = (double)frames - 2.0 - span;
    if (backwards && start < span) start = span;
  }
  if (start < 0.0) start = 0.0;
  if (backwards) { inc = -inc; g->born_back++; }

  /* Loudness: grains add up by power. The overlap cannot exceed the pool. */
  float overlap = dens * size_s;
  if (overlap > (float)GC_POOL) overlap = (float)GC_POOL;
  const int shape = (int)lrintf(clampf(p->shape, 0.0f, 1.0f) * (GC_SHAPES - 1));
  const float gain = v->vel * g->win_gain[shape] / sqrtf(overlap > 1.0f ? overlap : 1.0f);
  /* Stereo Spread: random pan, balance law (as Clouds for stereo sources): the centre keeps unity gain */
  const float pan = 0.5f + clampf(p->spread, 0.0f, 1.0f) * (rnd(g) - 0.5f);

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
  gr->gl = gain * (pan > 0.5f ? 2.0f * (1.0f - pan) : 1.0f);
  gr->gr = gain * (pan < 0.5f ? 2.0f * pan : 1.0f);
  gr->fade = 1.0f;
  gr->fstep = 0.0f;
  gr->voice = vi;
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
  const float inc = gr->inc, gl = gr->gl, gn = gr->gr, wstep = gr->wstep, fstep = gr->fstep;
  const float frac = gr->frac, wph = gr->wph, fade = gr->fade;
  const int64_t idx = gr->idx;
  float* restrict dl = ol + d;
  float* restrict dr = orr + d;
  /* frames read: from idx (+frac) for cnt samples, forward or backwards (inc < 0) */
  const int64_t last = idx + (int64_t)floorf(frac + (float)(cnt - 1) * inc);
  const int64_t lo = inc < 0.0f ? last : idx, hi = inc < 0.0f ? idx : last;
  /* fast path: all frames needed (j-1 .. j+2) exist without wrapping; levels end with guard frames */
  const int fast = lo >= 1 && hi < frames;

  const int hermite_on = g->interp == GC_INTERP_HERMITE;

  if (!fast || fstep != 0.0f || inc < 0.0f) {
    /* general path: backwards grains, wrap at the ends (short samples), fade-out (sample swap) */
    for (int i = 0; i < cnt; i++) {
      const float pf = frac + (float)i * inc;
      const int k = (int)floorf(pf);
      const float t = pf - (float)k;
      int64_t j = idx + k, jm = j - 1;
      if (!fast) { j = ((j % frames) + frames) % frames; jm = j ? j - 1 : frames - 1; }   /* guard: j+1, j+2 */
      const float* p = s + 2 * j;
      const float a = w[(int)(wph + (float)i * wstep)] * (fade + (float)i * fstep);
      if (hermite_on) {
        const float* q = s + 2 * jm;
        dl[i] += a * gl * hermite(q[0], p[0], p[2], p[4], t);
        dr[i] += a * gn * hermite(q[1], p[1], p[3], p[5], t);
      } else {
        dl[i] += a * gl * (p[0] + t * (p[2] - p[0]));
        dr[i] += a * gn * (p[1] + t * (p[3] - p[1]));
      }
    }
  } else if (hermite_on) {
    for (int i = 0; i < cnt; i++) {
      const float pf = frac + (float)i * inc;
      const int k = (int)pf;
      const float t = pf - (float)k;
      /* indexed loads: with a pointer and p[-2] gcc made this loop 2x slower (measured) */
      const int64_t j = idx + k, jm = j - 1, j1 = j + 1, j2 = j + 2;
      const float a = w[(int)(wph + (float)i * wstep)];
      dl[i] += a * gl * hermite(s[2 * jm], s[2 * j], s[2 * j1], s[2 * j2], t);
      dr[i] += a * gn * hermite(s[2 * jm + 1], s[2 * j + 1], s[2 * j1 + 1], s[2 * j2 + 1], t);
    }
  } else {
    for (int i = 0; i < cnt; i++) {
      const float pf = frac + (float)i * inc;
      const int k = (int)pf;
      const float t = pf - (float)k;
      const float* p = s + 2 * (idx + k);
      const float a = w[(int)(wph + (float)i * wstep)];
      dl[i] += a * gl * (p[0] + t * (p[2] - p[0]));
      dr[i] += a * gn * (p[1] + t * (p[3] - p[1]));
    }
  }
  const float total = frac + (float)cnt * inc;
  const int64_t adv = (int64_t)floorf(total);
  gr->idx = (((idx + adv) % frames) + frames) % frames;
  gr->frac = total - (float)adv;
  gr->wph = wph + (float)cnt * wstep;
  if (gr->wph > (float)GC_WIN) gr->wph = (float)GC_WIN;
  gr->fade = fade + (float)cnt * fstep;
  gr->remain -= cnt;
  if (gr->remain <= 0) { gr->active = 0; g->nactive--; }
}

/* Envelope level at the end of a block of m samples; sets *done when the voice has finished. */
static float voice_env_end(GrainCore* g, const GcParams* p, GcVoice* v, int m, int* done) {
  float e = v->env;
  *done = 0;
  switch (v->stage) {
    case GC_V_ATTACK: {
      const float a = clampf(p->attack_ms, GC_ATTACK_MIN, GC_ATTACK_MAX) * 0.001f * g->sr;
      e += (float)m / a;
      if (e >= 1.0f) { e = 1.0f; v->stage = GC_V_SUSTAIN; }
      break;
    }
    case GC_V_SUSTAIN:
      e = 1.0f;
      break;
    case GC_V_RELEASE: {
      const float r = clampf(p->release_ms, GC_RELEASE_MIN, GC_RELEASE_MAX) * 0.001f * g->sr;
      e *= expf(-6.9077553f * (float)m / r);        /* ln(1000): -60 dB after release_ms */
      if (e < 1e-4f) { e = 0.0f; *done = 1; }       /* -80 dB: the voice is over */
      break;
    }
    case GC_V_STEAL:
      e -= v->steal_step * (float)m;
      if (e <= 0.0f) { e = 0.0f; *done = 1; }
      break;
  }
  return e;
}

void gc_process(GrainCore* g, const GcParams* p, float* out_l, float* out_r, uint32_t n) {
  memset(out_l, 0, n * sizeof(float));
  memset(out_r, 0, n * sizeof(float));
  const int have = g->src.frames[0] > 0;
  if (!have && !g->nactive && !gc_active_voices(g)) return;
  const double wstart = have ? window_start(g, p) : 0.0, wlen = have ? window_len(g, p) : 1.0;
  float scan = clampf(p->scan, -GC_SCAN_MAX, GC_SCAN_MAX);
  if (fabsf(scan) < GC_SCAN_DEAD) scan = 0.0f;
  const float ratio = g->src.rate > 0 ? g->src.rate / g->sr : 1.0f;

  for (uint32_t off = 0; off < n; off += GC_BLOCK) {
    const int m = (int)(n - off < GC_BLOCK ? n - off : GC_BLOCK);
    float e0[GC_VOICES], e1[GC_VOICES];
    int done[GC_VOICES], any = 0;
    for (int vi = 0; vi < GC_VOICES; vi++) {
      GcVoice* v = &g->voices[vi];
      done[vi] = 0;
      if (v->stage == GC_V_IDLE) continue;
      any = 1;
      if (v->stage != GC_V_STEAL) {              /* grains are born while the voice sounds, also in release */
        while (have && v->next < (float)m) {
          spawn(g, p, vi, v->next > 0.0f ? (int)v->next : 0);
          v->next += next_interval(g, p);
        }
        v->next -= (float)m;
        if (v->next < 0.0f) v->next = 0.0f;
        /* the playhead loops inside the window */
        if (have) v->playhead = wstart + wrap(v->playhead + (double)(scan * ratio * (float)m) - wstart, wlen);
      }
      e0[vi] = v->env;
      e1[vi] = voice_env_end(g, p, v, m, &done[vi]);
      v->env = e1[vi];
    }
    if (!any && !g->nactive) continue;
    memset(g->vbuf, 0, sizeof g->vbuf);

    for (int i = 0; i < GC_POOL; i++) {
      GcGrain* gr = &g->grains[i];
      if (gr->active) render_grain(g, gr, g->vbuf[gr->voice][0], g->vbuf[gr->voice][1], m);
    }

    /* the voice envelope on the sum of its grains, ramped across the block */
    for (int vi = 0; vi < GC_VOICES; vi++) {
      GcVoice* v = &g->voices[vi];
      if (v->stage == GC_V_IDLE && !done[vi]) continue;
      const float* bl = g->vbuf[vi][0];
      const float* br = g->vbuf[vi][1];
      const float a = e0[vi], step = (e1[vi] - e0[vi]) / (float)m;
      float* ol = out_l + off;
      float* orr = out_r + off;
      for (int i = 0; i < m; i++) {
        const float e = a + step * (float)(i + 1);
        ol[i] += e * bl[i];
        orr[i] += e * br[i];
      }
      if (!done[vi]) continue;
      kill_voice_grains(g, vi);
      if (v->stage == GC_V_STEAL) {               /* stolen: the waiting note starts from silence */
        v->env = 0.0f;
        voice_start(g, p, vi, v->pending_note, v->pending_vel);
        if (v->pending_off) voice_release(v);
      } else {
        v->stage = GC_V_IDLE; v->note = -1; v->gate = 0; v->env = 0.0f;
      }
    }
  }
}
