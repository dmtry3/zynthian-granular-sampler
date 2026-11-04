/* grain_bench: CPU cost of grain_core. Only numbers measured on the Pi 5 count.
 *   grain_bench [sample file]               one voice at 16, 32 and 64 grains, linear and Hermite;
 *                                           then 8 voices (a chord) sharing the whole pool
 *   grain_bench sample.wav --wav out        also render 10 s for listening:
 *       out_linear.wav, out_hermite.wav     the two interpolations (notes from -12 to +19 semitones)
 *       out_nolevels.wav                    linear without band-limited levels: the aliasing they remove
 * Result: percent of one core for real-time audio at 48 kHz (CPU time / audio time). */
#include <math.h>
#include <sndfile.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "../src/grain_core.h"
#include "../src/sample.h"

#define SR 48000
#define PERIOD 256          /* JACK period in Zynthian */
#define SECONDS 20.0

static GrainCore G;

static double cpu_now(void) {
  struct timespec t;
  clock_gettime(CLOCK_THREAD_CPUTIME_ID, &t);
  return t.tv_sec + t.tv_nsec * 1e-9;
}

static void setup(GcParams* p, int grains) {
  gc_default_params(p);
  p->size_ms = 400.0f;                      /* overlap = density * size */
  p->density = grains / 0.4f;
  if (p->density > GC_DENS_MAX) p->density = GC_DENS_MAX;   /* 64: 160/s -> pool stays full */
  p->scan = 1.0f; p->spray = 0.3f; p->window = 1.0f; p->chaos = 0.3f;
}

static void measure(int grains, int interp) {
  static float l[PERIOD], r[PERIOD];
  GcParams p;
  setup(&p, grains);
  G.interp = interp;
  gc_set_voices(&G, 1);
  gc_note_on(&G, &p, 67, 100);              /* a fifth up: fractional read step, interpolation in use */
  for (int k = 0; k < SR / PERIOD; k++) gc_process(&G, &p, l, r, PERIOD);   /* 1 s warm-up: pool fills */
  const long periods = (long)(SECONDS * SR / PERIOD);
  double active = 0, sink = 0;
  const double t0 = cpu_now();
  for (long k = 0; k < periods; k++) {
    gc_process(&G, &p, l, r, PERIOD);
    active += G.nactive;
    sink += l[k & (PERIOD - 1)];
  }
  const double cpu = cpu_now() - t0;
  printf("%-8s %2d grains: %5.1f active, %6.2f %% of one core, %6.2f us per period%s\n",
         interp ? "hermite" : "linear", grains, active / periods, 100.0 * cpu / SECONDS, 1e6 * cpu / periods,
         sink == 12345.0 ? "!" : "");
  gc_all_off(&G);
}

/* 8 voices, a chord, each wanting pool/8 grains: the stage 3 case (8 voices, all grains busy). */
static void measure_poly(int interp) {
  static float l[PERIOD], r[PERIOD];
  GcParams p;
  setup(&p, GC_POOL / GC_VOICES);
  p.attack_ms = 5; p.release_ms = 500; p.spread = 0.5f;
  G.interp = interp;
  gc_set_voices(&G, GC_VOICES);
  const int chord[GC_VOICES] = { 48, 55, 60, 63, 67, 70, 74, 79 };
  for (int v = 0; v < GC_VOICES; v++) gc_note_on(&G, &p, chord[v], 100);
  for (int k = 0; k < SR / PERIOD; k++) gc_process(&G, &p, l, r, PERIOD);
  const long periods = (long)(SECONDS * SR / PERIOD);
  double active = 0, sink = 0;
  const double t0 = cpu_now();
  for (long k = 0; k < periods; k++) {
    gc_process(&G, &p, l, r, PERIOD);
    active += G.nactive;
    sink += l[k & (PERIOD - 1)];
  }
  const double cpu = cpu_now() - t0;
  printf("%-8s %d voices, pool %d: %5.1f active, %6.2f %% of one core, %6.2f us per period%s\n",
         interp ? "hermite" : "linear", gc_active_voices(&G), GC_POOL, active / periods, 100.0 * cpu / SECONDS,
         1e6 * cpu / periods, sink == 12345.0 ? "!" : "");
  gc_all_off(&G);
}

static void render(const char* prefix, const char* name, int interp, int levels) {
  char path[512];
  snprintf(path, sizeof path, "%s_%s.wav", prefix, name);
  GcParams p;
  setup(&p, 24);
  p.size_ms = 150.0f; p.density = 60.0f; p.scan = 0.5f; p.spray = 0.1f;
  gc_set_voices(&G, 1);
  G.interp = interp;
  G.use_levels = levels;
  G.rng = 12345;
  SF_INFO info = { 0 };
  info.samplerate = SR; info.channels = 2; info.format = SF_FORMAT_WAV | SF_FORMAT_PCM_16;
  SNDFILE* f = sf_open(path, SFM_WRITE, &info);
  if (!f) { printf("can't write %s\n", path); return; }
  static float l[PERIOD], r[PERIOD], inter[2 * PERIOD];
  const int notes[] = { 48, 60, 67, 72, 79 };          /* -12, 0, +7, +12, +19 semitones */
  const int per = 2 * SR / PERIOD;                     /* 2 s per note */
  for (int k = 0; k < 5 * per; k++) {
    if (k % per == 0) gc_note_on(&G, &p, notes[k / per], 100);
    gc_process(&G, &p, l, r, PERIOD);
    for (int i = 0; i < PERIOD; i++) { inter[2 * i] = l[i]; inter[2 * i + 1] = r[i]; }
    sf_writef_float(f, inter, PERIOD);
  }
  sf_close(f);
  gc_all_off(&G);
  G.use_levels = 1;
  printf("Rendered %s\n", path);
}

int main(int argc, char** argv) {
  const char *in = NULL, *wav = NULL;
  for (int i = 1; i < argc; i++) {
    if (!strcmp(argv[i], "--wav") && i + 1 < argc) wav = argv[++i];
    else in = argv[i];
  }
  gc_enable_ftz();
  gc_init(&G, SR, 12345);
  GsSample* s = NULL;
  float* noise[GS_LEVELS] = { NULL };
  GcSource src;
  memset(&src, 0, sizeof src);
  if (in) {
    int err;
    s = gs_sample_load(in, 600, &err);
    if (!s) { printf("can't load %s (error %d)\n", in, err); return 1; }
    for (int k = 0; k < GS_LEVELS && s->data[k]; k++) { src.data[k] = s->data[k]; src.frames[k] = s->frames[k]; src.nlevels = k + 1; }
    src.rate = (float)s->rate;
    printf("Sample: %s, %.1f s\n", in, s->frames[0] / s->rate);
  } else {
    int64_t n = 10 * SR;
    uint32_t x = 1;
    for (int k = 0; k < GS_LEVELS; k++, n /= 2) {      /* noise levels: content does not matter for timing */
      noise[k] = malloc((n + GS_GUARD) * 2 * sizeof(float));
      for (int64_t i = 0; i < (n + GS_GUARD) * 2; i++) { x = x * 1664525u + 1013904223u; noise[k][i] = (int32_t)x * (0.5f / 2147483648.0f); }
      src.data[k] = noise[k]; src.frames[k] = n; src.nlevels = k + 1;
    }
    src.rate = SR;
    printf("Sample: 10 s of noise\n");
  }
  gc_set_source(&G, &src);
  printf("48 kHz, period %d, %.0f s per measurement, pool %d, a fifth up\n", PERIOD, SECONDS, GC_POOL);
  for (int interp = 0; interp < 2; interp++) {
    measure(16, interp);
    measure(32, interp);
    measure(64, interp);
  }
  measure_poly(GC_INTERP_LINEAR);
  measure_poly(GC_INTERP_HERMITE);
  if (wav) {
    render(wav, "linear", GC_INTERP_LINEAR, 1);
    render(wav, "hermite", GC_INTERP_HERMITE, 1);
    render(wav, "nolevels", GC_INTERP_LINEAR, 0);
  }
  gs_sample_free(s);
  for (int k = 0; k < GS_LEVELS; k++) free(noise[k]);
  return 0;
}
