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
  g->mod_depth = 1.0f;
  g->bpm = 120.0f;
  g->bend_ratio = 1.0f;
  g->lfo_div[0] = g->lfo_div[1] = 4;       /* 1/4 */
  g->env_shape[0] = g->env_shape[1] = 0.1f;
  for (int i = 0; i < GC_VOICES; i++) {
    GcVoice* v = &g->voices[i];
    v->note = -1;
    v->last_grain = -1;
    v->level = v->level_prev = 1.0f;
    for (int k = 0; k < 2; k++) {          /* free LFOs: each voice from its own phase, chords move apart */
      v->lfo_ph[k] = rnd(g);
      v->lfo_a[k] = rnd(g) * 2.0f - 1.0f;
      v->lfo_b[k] = rnd(g) * 2.0f - 1.0f;
    }
  }
  g->nvoices = GC_VOICES;
  g->interp = GC_INTERP_LINEAR;
  g->use_levels = 1;
  g->align = 1;
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
  p->mark_start = 0.0f; p->mark_end = 1.0f;
  p->attack_ms = 10.0f; p->release_ms = 500.0f; p->spread = 0.0f;
  p->pitch = 0.0f; p->pitch_spray = 0.0f; p->reverse = 0.0f; p->quantize = 0.0f;
  p->cutoff_hz = GC_CUTOFF_MAX; p->resonance = 0.0f; p->ftype = GC_LP; p->keytrack = 0.0f;
  p->lfo_rate[0] = 1.0f; p->lfo_rate[1] = 0.25f; p->env_time[0] = 1.0f; p->env_time[1] = 2.0f;
}

/* ---------- modulation ---------- */
void gc_set_mod_cell(GrainCore* g, int src, int dst, float amount) {
  if (src < 0 || src >= GC_NSRC || dst < 0 || dst >= GC_NDST) return;
  g->mod_cell[src][dst] = clampf(amount, -1.0f, 1.0f);
  g->nmod = 0;
  for (int s = 0; s < GC_NSRC; s++)
    for (int d = 0; d < GC_NDST; d++)
      if (g->mod_cell[s][d] != 0.0f) g->mod_list[g->nmod++] = s * GC_NDST + d;
}
void gc_set_mod_depth(GrainCore* g, float depth) { g->mod_depth = clampf(depth, 0.0f, 1.0f); }
void gc_set_modwheel(GrainCore* g, float value) { g->modwheel = clampf(value, 0.0f, 1.0f); }
void gc_set_lfo(GrainCore* g, int i, int shape, int sync) {
  if (i < 0 || i > 1) return;
  g->lfo_shape[i] = shape < 0 || shape >= GC_NLFO_SHAPES ? GC_LFO_SINE : shape;
  g->lfo_sync[i] = sync < GC_SYNC_FREE || sync > GC_SYNC_TEMPO ? GC_SYNC_FREE : sync;
}

/* Tempo LFO cycle lengths in beats (quarter notes): 4 bars .. 1/32, with triplets */
const float gc_div_beats[GC_NDIVS] = { 16.0f, 8.0f, 4.0f, 2.0f, 1.0f, 2.0f / 3.0f, 0.5f, 1.0f / 3.0f, 0.25f,
                                       1.0f / 6.0f, 0.125f };
const char* const gc_div_names[GC_NDIVS] = { "4 bars", "2 bars", "1 bar", "1/2", "1/4", "1/4T", "1/8", "1/8T",
                                             "1/16", "1/16T", "1/32" };
void gc_set_lfo_div(GrainCore* g, int i, int div) {
  if (i >= 0 && i <= 1) g->lfo_div[i] = div < 0 || div >= GC_NDIVS ? 4 : div;
}
void gc_set_tempo(GrainCore* g, float bpm, double beat) {
  if (bpm >= 20.0f && bpm <= 400.0f) g->bpm = bpm;
  if (beat >= 0.0) g->beat = beat;
}
void gc_set_env(GrainCore* g, int i, float shape) {
  if (i >= 0 && i <= 1) g->env_shape[i] = clampf(shape, 0.0f, 1.0f);
}

float gc_lfo_value(int shape, float ph, float a, float b) {
  switch (shape) {
    case GC_LFO_TRI:       return ph < 0.5f ? 4.0f * ph - 1.0f : 3.0f - 4.0f * ph;
    case GC_LFO_SAW_UP:    return 2.0f * ph - 1.0f;
    case GC_LFO_SAW_DOWN:  return 1.0f - 2.0f * ph;
    case GC_LFO_SQUARE:    return ph < 0.5f ? 1.0f : -1.0f;
    case GC_LFO_SH:        return a;
    case GC_LFO_SMOOTH:    return a + (b - a) * ph * ph * (3.0f - 2.0f * ph);
    default:               return sinf(6.2831853f * ph);
  }
}

/* One-shot from the note: rise over shape*time, fall over the rest (curved); all attack holds at 1. */
float gc_env_value(float shape, float t, float time) {
  const float att = shape * time, dec = (1.0f - shape) * time;
  if (t < att) return t / att;
  if (dec < 0.001f) return 1.0f;
  const float u = (t - att) / dec;
  if (u >= 1.0f) return 0.0f;
  return (1.0f - u) * (1.0f - u);
}

/* Sums the non-empty cells for the sources in src[] into out[] (times Mod Depth). */
static void mod_eval(const GrainCore* g, const float* src, float* out) {
  for (int d = 0; d < GC_NDST; d++) out[d] = 0.0f;
  for (int i = 0; i < g->nmod; i++) {
    const int s = g->mod_list[i] / GC_NDST, d = g->mod_list[i] % GC_NDST;
    out[d] += g->mod_cell[s][d] * src[s];
  }
  for (int d = 0; d < GC_NDST; d++) out[d] *= g->mod_depth;
}

/* Advance the voice's LFOs and envelopes by m samples; source values and continuous destinations. */
static void voice_mod(GrainCore* g, const GcParams* p, GcVoice* v, int m) {
  v->level_prev = v->level;
  if (!g->nmod) {
    for (int d = 0; d < GC_NDST; d++) v->mod[d] = 0.0f;
    v->level = 1.0f;
    return;
  }
  const float dt = (float)m / g->sr;
  for (int k = 0; k < 2; k++) {
    int new_cycle = 0;
    if (g->lfo_sync[k] == GC_SYNC_TEMPO) {      /* phase from the beat clock: every voice the same */
      const double c = g->beat / (double)gc_div_beats[g->lfo_div[k]];
      const int64_t n = (int64_t)floor(c);
      v->lfo_ph[k] = (float)(c - (double)n);
      new_cycle = n != v->lfo_cycle[k];
      v->lfo_cycle[k] = n;
    } else {
      v->lfo_ph[k] += clampf(p->lfo_rate[k], GC_LFO_MIN, GC_LFO_MAX) * dt;
      if (v->lfo_ph[k] >= 1.0f) { v->lfo_ph[k] -= floorf(v->lfo_ph[k]); new_cycle = 1; }
    }
    if (new_cycle) {                            /* a new cycle: next random step */
      v->lfo_a[k] = v->lfo_b[k];
      v->lfo_b[k] = rnd(g) * 2.0f - 1.0f;
      if (g->lfo_shape[k] == GC_LFO_SH) v->lfo_a[k] = v->lfo_b[k];
    }
    v->src[GC_SRC_LFO1 + k] = gc_lfo_value(g->lfo_shape[k], v->lfo_ph[k], v->lfo_a[k], v->lfo_b[k]);
    v->env_t[k] += dt;
    v->src[GC_SRC_ENV1 + k] = gc_env_value(g->env_shape[k], v->env_t[k],
                                           clampf(p->env_time[k], GC_ENV_MIN, GC_ENV_MAX));
  }
  v->src[GC_SRC_VEL] = v->vel;
  v->src[GC_SRC_WHEEL] = g->modwheel;
  v->src[GC_SRC_RANDOM] = v->rnd;
  mod_eval(g, v->src, v->mod);
  v->level = clampf(1.0f + v->mod[GC_DST_LEVEL], 0.0f, 2.0f);
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

/* The part of the sample between the markers, in frames of level 0: [*r0, *r0 + *rl). */
static void region(const GrainCore* g, const GcParams* p, double* r0, double* rl) {
  const double n = (double)g->src.frames[0];
  double a = (double)clampf(p->mark_start, 0.0f, 1.0f) * n, b = (double)clampf(p->mark_end, 0.0f, 1.0f) * n;
  if (b < a) { const double t = a; a = b; b = t; }
  if (b - a < 2.0) { b = a + 2.0; if (b > n) { b = n; a = n - 2.0 > 0.0 ? n - 2.0 : 0.0; } }
  *r0 = a;
  *rl = b - a;
}
static double window_start(const GrainCore* g, const GcParams* p) {
  double r0, rl;
  region(g, p, &r0, &rl);
  return r0 + (double)clampf(p->position, 0.0f, 1.0f) * (rl - 1.0);
}
static double window_len(const GrainCore* g, const GcParams* p) {
  double r0, rl;
  region(g, p, &r0, &rl);
  double w = (double)clampf(p->window, GC_WINDOW_MIN, 1.0f) * rl;
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
static float filter_target_log(const GcParams* p, float pitch, float mod) {
  const float c = clampf(p->cutoff_hz, GC_CUTOFF_MIN, GC_CUTOFF_MAX);
  return log2f(c) + clampf(p->keytrack, 0.0f, 1.0f) * (pitch - 60.0f) / 12.0f + mod * GC_MOD_CUTOFF;
}

/* A new key for a voice: in mono with Glide, a sounding voice slides from its pitch, else it jumps. */
static void voice_set_note(GrainCore* g, GcVoice* v, int note) {
  if (g->mono && g->glide_ms > 0.0f && v->stage != GC_V_IDLE && v->pitch != (float)note) {
    v->glide_step = fabsf((float)note - v->pitch) / (g->glide_ms * 0.001f * g->sr);
  } else {
    v->pitch = (float)note;
    v->glide_step = 0.0f;
  }
  v->note = note;
}

void gc_set_glide(GrainCore* g, float ms) { g->glide_ms = clampf(ms, 0.0f, 10000.0f); }

static void voice_start(GrainCore* g, const GcParams* p, int vi, int note, int vel) {
  GcVoice* v = &g->voices[vi];
  if (v->stage == GC_V_IDLE) {             /* a fresh voice: filter state empty, cutoff right at the knob */
    v->ic1[0] = v->ic1[1] = v->ic2[0] = v->ic2[1] = 0.0f;
    v->fc_log = filter_target_log(p, (float)note, 0.0f);
    v->filter_on = 0;
    v->level = v->level_prev = 1.0f;
  }
  for (int k = 0; k < 2; k++) {
    v->env_t[k] = 0.0f;                    /* Env 1/2 start with the note */
    if (g->lfo_sync[k] == GC_SYNC_KEY) v->lfo_ph[k] = 0.0f;
  }
  voice_set_note(g, v, note);             /* before the stage changes: a fresh voice does not glide */
  v->stage = GC_V_ATTACK;
  v->gate = 1;
  v->sus = 0;
  v->vel = clampf(vel / 127.0f, 0.0f, 1.0f);
  v->playhead = g->src.frames[0] ? window_start(g, p) : 0.0;   /* every note starts reading at Position */
  v->next = 0.0f;                                              /* first grain right away */
  v->age = ++g->age;
  g->last_voice = vi;
}

static void voice_release(GcVoice* v) {
  v->gate = 0;
  v->sus = 0;
  if (v->stage == GC_V_ATTACK || v->stage == GC_V_SUSTAIN) v->stage = GC_V_RELEASE;
}

/* The key is let go: release, or with the sustain pedal down keep sounding until the pedal is up. */
static void voice_key_up(GrainCore* g, GcVoice* v) {
  if (g->sustain && (v->stage == GC_V_ATTACK || v->stage == GC_V_SUSTAIN)) { v->gate = 0; v->sus = 1; }
  else voice_release(v);
}

void gc_set_sustain(GrainCore* g, int down) {
  g->sustain = down != 0;
  if (g->sustain) return;
  for (int i = 0; i < GC_VOICES; i++)
    if (g->voices[i].sus) voice_release(&g->voices[i]);
}

void gc_set_bend(GrainCore* g, float semis) {
  g->bend = clampf(semis, -GC_BEND_MAX, GC_BEND_MAX);
  const float r = exp2f(g->bend / 12.0f);
  if (r == g->bend_ratio) return;
  g->bend_ratio = r;
  for (int i = 0; i < GC_POOL; i++)          /* sounding grains bend too, not only new ones */
    if (g->grains[i].active) g->grains[i].inc = g->grains[i].inc0 * r;
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
    if (g->legato && (v->gate || v->sus) && (v->stage == GC_V_ATTACK || v->stage == GC_V_SUSTAIN)) {
      voice_set_note(g, v, note);         /* legato: new pitch for new grains, envelope and playhead go on */
      return;
    }
    voice_start(g, p, 0, note, vel);      /* else: start again, the envelope rises from where it is */
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
    if (g->nheld > 0) {
      voice_set_note(g, v, g->held[g->nheld - 1]);   /* back to the key still held */
      if (!g->legato) v->restart = 1;     /* without legato it starts again (gc_process knows Position) */
    } else voice_key_up(g, v);
    return;
  }
  for (int i = 0; i < GC_VOICES; i++) {
    GcVoice* v = &g->voices[i];
    if (v->stage == GC_V_STEAL && v->pending_note == note) v->pending_off = 1;
    else if (v->note == note && v->gate) voice_key_up(g, v);
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
    v->stage = GC_V_IDLE; v->gate = 0; v->sus = 0; v->note = -1; v->env = 0.0f;
  }
  for (int i = 0; i < GC_POOL; i++) g->grains[i].active = 0;
  g->nactive = 0;
}

void gc_set_voices(GrainCore* g, int n, int legato) {
  n = n < 1 ? 1 : n > GC_VOICES ? GC_VOICES : n;
  const int mono = n == 1;                     /* one voice = mono: back to a held key */
  g->legato = mono && legato;
  if (n == g->nvoices && mono == g->mono) return;
  if (mono != g->mono) gc_all_notes_off(g);   /* to or from mono: everything releases */
  g->mono = mono;
  g->nvoices = n;
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

  /* modulation at birth: the voice's sources, with a new Random for this grain */
  float bm[GC_NDST] = { 0 };
  if (g->nmod) {
    v->rnd = rnd(g) * 2.0f - 1.0f;
    float src[GC_NSRC];
    memcpy(src, v->src, sizeof src);
    src[GC_SRC_RANDOM] = v->rnd;
    mod_eval(g, src, bm);
  }
  const float size_s = clampf(p->size_ms * exp2f(bm[GC_DST_SIZE] * GC_MOD_OCTAVES), GC_SIZE_MIN, GC_SIZE_MAX) * 0.001f;
  const float dens = clampf(p->density * exp2f(v->mod[GC_DST_DENSITY] * GC_MOD_OCTAVES), GC_DENS_MIN, GC_DENS_MAX);
  const double wlen = window_len(g, p);
  const float spray = clampf(p->spray + bm[GC_DST_SPRAY] * GC_MOD_SPRAY, 0.0f, 1.0f);
  double start = v->playhead + (double)((rnd(g) * 2.0f - 1.0f) * spray * 0.5f) * wlen;
  double r0, rl;                                  /* between the markers; the window wraps inside them */
  region(g, p, &r0, &rl);
  start += (double)(bm[GC_DST_POSITION] * GC_MOD_POSITION) * rl;
  start = r0 + wrap(start - r0, rl);
  g->last_start = start;
  const int len = (int)(size_s * g->sr) > 1 ? (int)(size_s * g->sr) : 1;

  /* Pitch: note + Pitch + random Pitch Spray, pulled to the scale by Quantize; fixed for the grain. */
  float semis = v->pitch + clampf(p->pitch, -GC_PITCH_MAX, GC_PITCH_MAX) + bm[GC_DST_PITCH] * GC_MOD_PITCH;
  const float ps = clampf(p->pitch_spray, 0.0f, GC_PSPRAY_MAX);
  if (ps > 0.0f) semis += ps * (rnd(g) * 2.0f - 1.0f);
  const float q = clampf(p->quantize, 0.0f, 1.0f);
  if (q > 0.0f) semis += q * (gc_snap(g, semis) - semis);
  g->last_pitch = semis;
  const int backwards = p->reverse > 0.0f && rnd(g) < p->reverse;

  /* Pitched up: read a band-limited level at a lower rate, so nothing folds back above Nyquist. */
  float inc = (g->src.rate > 0 ? g->src.rate / g->sr : 1.0f) * exp2f((semis - p->root) / 12.0f);

  /* In phase with the sounding grain (PSOLA-like): two grains reading a periodic sound at the same speed
   * add up cleanly only if their read positions differ by whole periods; else they comb and beat (heard
   * as a warble on every transposed note, and on frozen sounds). Move the start by less than half a period. */
  const float per = g->src.period;
  if (g->align && per > 2.0f && v->last_grain >= 0) {
    const GcGrain* pg = &g->grains[v->last_grain];
    const float pstep = pg->inc0 * (float)(1 << pg->lv);           /* its speed in level-0 frames */
    const float mine = backwards ? -inc : inc;
    if (pg->active && pg->voice == vi && pg->gen == g->gen && fabsf(pstep - mine) < 0.01f * fabsf(mine)) {
      const int el = delay > pg->delay ? delay - pg->delay : 0;      /* its samples played when this one starts */
      const double ppos = ((double)pg->idx + pg->frac + (double)pg->inc * el) * (double)(1 << pg->lv);
      double d = fmod(ppos - start, (double)per);
      if (d > 0.5 * per) d -= per;
      if (d < -0.5 * per) d += per;
      /* at the ends of the sample a whole period the other way: wrapping round would move it far */
      const double room = r0 + rl - 2.0 - ((double)len * fabsf(inc) + 3.0);
      if (start + d < r0) d += per;
      else if (!backwards && start + d > room && start + d - per >= r0) d -= per;
      start += d;
    }
  }
  int lv = 0;
  while (g->use_levels && lv + 1 < g->src.nlevels && inc > GC_MIP_UP) { inc *= 0.5f; start *= 0.5; lv++; }
  const int64_t frames = g->src.frames[lv];
  /* Do not run past the end marker (forward) or the start marker (backwards): past the end of the sample a
   * jump would click, past a marker the grain would play what was cut away. Move the start if the grain
   * fits between the markers at all. */
  const double sc = 1.0 / (double)(1 << lv), m0 = r0 * sc, m1 = (r0 + rl) * sc;
  const double span = (double)len * inc + 3.0;
  if (span < m1 - m0 - 2.0) {
    if (!backwards && start > m1 - 2.0 - span) start = m1 - 2.0 - span;
    if (backwards && start < m0 + span) start = m0 + span;
  }
  if (start < 0.0) start = 0.0;
  if (start > (double)frames - 1.0) start = (double)frames - 1.0;
  if (backwards) { inc = -inc; g->born_back++; }

  /* Loudness: grains add up by power. The overlap cannot exceed the pool. */
  float overlap = dens * size_s;
  if (overlap > (float)GC_POOL) overlap = (float)GC_POOL;
  const int shape = (int)lrintf(clampf(p->shape, 0.0f, 1.0f) * (GC_SHAPES - 1));
  const float gain = v->vel * g->win_gain[shape] / sqrtf(overlap > 1.0f ? overlap : 1.0f);
  /* Stereo Spread: random pan, balance law (as Clouds for stereo sources): the centre keeps unity gain */
  const float pan = clampf(0.5f + clampf(p->spread, 0.0f, 1.0f) * (rnd(g) - 0.5f) + bm[GC_DST_PAN] * GC_MOD_PAN,
                           0.0f, 1.0f);

  gr->active = 1;
  gr->delay = delay;
  gr->remain = len;
  gr->gen = g->gen;
  gr->data = g->src.data[lv];
  gr->frames = frames;
  gr->idx = (int64_t)start;
  gr->frac = (float)(start - (double)gr->idx);
  gr->inc0 = inc;
  gr->inc = inc * g->bend_ratio;
  gr->win = g->win[shape];
  gr->wph = 0.0f;
  gr->wstep = (float)GC_WIN / (float)len;
  gr->gl = gain * (pan > 0.5f ? 2.0f * (1.0f - pan) : 1.0f);
  gr->gr = gain * (pan < 0.5f ? 2.0f * pan : 1.0f);
  gr->fade = 1.0f;
  gr->fstep = 0.0f;
  gr->voice = vi;
  gr->lv = lv;
  v->last_grain = (int)(gr - g->grains);
  g->last_level = lv;
  g->nactive++;
  g->born++;
}

/* Next grain distance in samples: even at chaos 0, exponential (Poisson) at chaos 1. */
static float next_interval(GrainCore* g, const GcParams* p, const GcVoice* v) {
  const float base = g->sr / clampf(p->density * exp2f(v->mod[GC_DST_DENSITY] * GC_MOD_OCTAVES),
                                    GC_DENS_MIN, GC_DENS_MAX);
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

/* Voice filter on the sum of its grains: TPT state variable filter (Zavalishin; Simper's form),
 * stable when the cutoff moves. Coefficients once per block, cutoff smoothed in log2 Hz.
 * A low-pass fully open or a high-pass fully closed is bypassed (no cost). */
static void voice_filter(GrainCore* g, const GcParams* p, GcVoice* v, float* bl, float* br, int m) {
  const float target = filter_target_log(p, v->pitch, v->mod[GC_DST_CUTOFF]);
  const float a = 1.0f - expf(-(float)m / (GC_FILTER_SMOOTH * g->sr));
  v->fc_log += (target - v->fc_log) * a;
  float fc = exp2f(v->fc_log);
  const float top = 0.45f * g->sr;
  fc = clampf(fc, GC_CUTOFF_MIN, top);
  v->fc_hz = fc;
  const int type = p->ftype;
  const float res = clampf(p->resonance, 0.0f, 1.0f);
  const int open = (type == GC_LP && fc >= fminf(GC_CUTOFF_MAX * 0.97f, top) && res < 0.01f) ||
                   (type == GC_HP && fc <= GC_CUTOFF_MIN * 1.05f && res < 0.01f);
  if (open) { v->filter_on = 0; return; }
  if (!v->filter_on) {                      /* switching on: start from an empty state */
    v->ic1[0] = v->ic1[1] = v->ic2[0] = v->ic2[1] = 0.0f;
    v->filter_on = 1;
  }
  const float gg = tanf((float)M_PI * fc / g->sr);
  const float k = 1.4142136f * (1.0f - res) + 0.05f * res;   /* 1/Q: Butterworth .. Q 20 */
  const float a1 = 1.0f / (1.0f + gg * (gg + k)), a2 = gg * a1, a3 = gg * a2;
  for (int ch = 0; ch < 2; ch++) {
    float* x = ch ? br : bl;
    float s1 = v->ic1[ch], s2 = v->ic2[ch];
    for (int i = 0; i < m; i++) {
      const float v0 = x[i];
      const float v3 = v0 - s2;
      const float v1 = a1 * s1 + a2 * v3;
      const float v2 = s2 + a2 * s1 + a3 * v3;
      s1 = 2.0f * v1 - s1;
      s2 = 2.0f * v2 - s2;
      x[i] = type == GC_LP ? v2 : type == GC_HP ? v0 - k * v1 - v2 : v1;
    }
    v->ic1[ch] = s1; v->ic2[ch] = s2;
  }
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
  const double beats_per_sample = (double)g->bpm / (60.0 * (double)g->sr);
  if (!have && !g->nactive && !gc_active_voices(g)) { g->beat += beats_per_sample * n; return; }
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
      if (v->restart) {                         /* mono without legato, back to a held key */
        v->restart = 0;
        v->stage = GC_V_ATTACK;
        v->playhead = have ? wstart : 0.0;
        v->next = 0.0f;
      }
      if (v->pitch != (float)v->note) {         /* glide toward the key */
        const float d = (float)v->note - v->pitch, step = v->glide_step * (float)m;
        v->pitch = fabsf(d) <= step || step <= 0.0f ? (float)v->note : v->pitch + (d > 0 ? step : -step);
      }
      voice_mod(g, p, v, m);
      if (v->stage != GC_V_STEAL) {              /* grains are born while the voice sounds, also in release */
        while (have && v->next < (float)m) {
          spawn(g, p, vi, v->next > 0.0f ? (int)v->next : 0);
          v->next += next_interval(g, p, v);
        }
        v->next -= (float)m;
        if (v->next < 0.0f) v->next = 0.0f;
        /* the playhead loops inside the window; Scan modulated per voice */
        float vs = scan;
        if (v->mod[GC_DST_SCAN] != 0.0f) {
          vs = clampf(scan + v->mod[GC_DST_SCAN] * GC_MOD_SCAN, -GC_SCAN_MAX, GC_SCAN_MAX);
          if (fabsf(vs) < GC_SCAN_DEAD) vs = 0.0f;
        }
        if (have) v->playhead = wstart + wrap(v->playhead + (double)(vs * ratio * (float)m) - wstart, wlen);
      }
      e0[vi] = v->env;
      e1[vi] = voice_env_end(g, p, v, m, &done[vi]);
      v->env = e1[vi];
    }
    g->beat += beats_per_sample * m;             /* tempo clock: once per block, after the voices read it */
    if (!any && !g->nactive) continue;
    memset(g->vbuf, 0, sizeof g->vbuf);

    for (int i = 0; i < GC_POOL; i++) {
      GcGrain* gr = &g->grains[i];
      if (gr->active) render_grain(g, gr, g->vbuf[gr->voice][0], g->vbuf[gr->voice][1], m);
    }

    /* the voice filter, then the voice envelope on the sum of its grains, ramped across the block */
    for (int vi = 0; vi < GC_VOICES; vi++) {
      GcVoice* v = &g->voices[vi];
      if (v->stage == GC_V_IDLE && !done[vi]) continue;
      voice_filter(g, p, v, g->vbuf[vi][0], g->vbuf[vi][1], m);
      const float* bl = g->vbuf[vi][0];
      const float* br = g->vbuf[vi][1];
      /* envelope times the Level destination, both ramped across the block */
      const float a = e0[vi] * v->level_prev, step = (e1[vi] * v->level - a) / (float)m;
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
        if (v->pending_off) voice_key_up(g, v);
      } else {
        v->stage = GC_V_IDLE; v->note = -1; v->gate = 0; v->env = 0.0f;
      }
    }
  }
}
