/* grain_bench: CPU cost of grain_core. Only numbers measured on the Pi 5 count.
 *   grain_bench [sample file]            measure at 16, 32 and 64 active grains (default: 10 s of noise)
 *   grain_bench sample.wav --wav out.wav  also render 10 s at 32 grains, to listen to the engine
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
#define SECONDS 30.0

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

static void measure(int grains) {
  static float l[PERIOD], r[PERIOD];
  GcParams p;
  setup(&p, grains);
  gc_note_on(&G, &p, 67, 100);              /* a fifth up: fractional read step, interpolation in use */
  for (int k = 0; k < SR / PERIOD; k++) gc_process(&G, &p, l, r, PERIOD);   /* 1 s warm-up: pool fills */
  const long periods = (long)(SECONDS * SR / PERIOD);
  double active = 0, sink = 0;
  const uint64_t skipped0 = G.skipped;
  const double t0 = cpu_now();
  for (long k = 0; k < periods; k++) {
    gc_process(&G, &p, l, r, PERIOD);
    active += G.nactive;
    sink += l[k & (PERIOD - 1)];
  }
  const double cpu = cpu_now() - t0;
  printf("%2d grains wanted: %5.1f active, %6.2f %% of one core, %.2f us per period, skipped %llu%s\n",
         grains, active / periods, 100.0 * cpu / SECONDS, 1e6 * cpu / periods,
         (unsigned long long)(G.skipped - skipped0), sink == 12345.0 ? "!" : "");
  gc_all_off(&G);
}

static void render(const char* path) {
  GcParams p;
  setup(&p, 32);
  SF_INFO info = { 0 };
  info.samplerate = SR; info.channels = 2; info.format = SF_FORMAT_WAV | SF_FORMAT_PCM_16;
  SNDFILE* f = sf_open(path, SFM_WRITE, &info);
  if (!f) { printf("can't write %s\n", path); return; }
  static float l[PERIOD], r[PERIOD], inter[2 * PERIOD];
  const int notes[] = { 60, 63, 67, 72 };
  for (int k = 0; k < 10 * SR / PERIOD; k++) {
    if (k % (int)(2.5 * SR / PERIOD) == 0) gc_note_on(&G, &p, notes[(k / (int)(2.5 * SR / PERIOD)) % 4], 100);
    gc_process(&G, &p, l, r, PERIOD);
    for (int i = 0; i < PERIOD; i++) { inter[2 * i] = l[i]; inter[2 * i + 1] = r[i]; }
    sf_writef_float(f, inter, PERIOD);
  }
  sf_close(f);
  gc_all_off(&G);
  printf("Rendered 10 s (4 notes, 32 grains): %s\n", path);
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
  float* noise = NULL;
  if (in) {
    int err;
    s = gs_sample_load(in, 600, &err);
    if (!s) { printf("can't load %s (error %d)\n", in, err); return 1; }
    gc_set_source(&G, s->data[0], s->data[1], s->frames, (float)s->rate);
    printf("Sample: %s, %.1f s\n", in, s->frames / s->rate);
  } else {
    const int n = 10 * SR;
    noise = malloc(2 * n * sizeof(float));
    uint32_t x = 1;
    for (int i = 0; i < 2 * n; i++) { x = x * 1664525u + 1013904223u; noise[i] = (int32_t)x * (0.5f / 2147483648.0f); }
    gc_set_source(&G, noise, noise + n, n, SR);
    printf("Sample: 10 s of noise\n");
  }
  printf("48 kHz, period %d, %.0f s per measurement, pool %d\n", PERIOD, SECONDS, GC_POOL);
  measure(16);
  measure(32);
  measure(64);
  if (wav) render(wav);
  gs_sample_free(s);
  free(noise);
  return 0;
}
