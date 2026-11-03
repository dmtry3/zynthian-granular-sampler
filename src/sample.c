#include "sample.h"
#include <math.h>
#include <sndfile.h>
#include <stdlib.h>
#include <string.h>

#define CHUNK 4096

/* Half-band low-pass for decimation by 2: Kaiser-windowed sinc, cutoff at the new Nyquist.
 * Every other tap is zero, so only HB_HALF taps per side are used, plus the centre 0.5. */
#define HB_HALF 16            /* 4*16-1 = 63 taps */

static double bessel_i0(double x) {
  double s = 1.0, t = 1.0;
  for (int k = 1; k < 30; k++) { t *= (x / (2 * k)) * (x / (2 * k)); s += t; }
  return s;
}

/* hb[k]: tap at offset +-(2k+1). Computed per call (16 values): no shared state between threads. */
static void hb_taps(float* hb) {
  const double beta = 8.0, m = 2.0 * HB_HALF;    /* window half-length in taps */
  for (int k = 0; k < HB_HALF; k++) {
    const int o = 2 * k + 1;
    const double x = (double)o / m;
    const double w = bessel_i0(beta * sqrt(1.0 - x * x)) / bessel_i0(beta);
    hb[k] = (float)(sin(M_PI * o / 2.0) / (M_PI * o) * w);
  }
}

void gs_decimate2(const float* in, int64_t n, float* out) {
  float hb[HB_HALF];
  hb_taps(hb);
  const int64_t m = n / 2;
  for (int64_t i = 0; i < m; i++) {
    const int64_t c = 2 * i;
    float l = 0.5f * in[2 * c], r = 0.5f * in[2 * c + 1];
    for (int k = 0; k < HB_HALF; k++) {
      const int64_t o = 2 * k + 1;
      const int64_t a = c - o, b = c + o;
      /* the sample is treated as a loop, like grains read it */
      const int64_t ia = a >= 0 ? a : ((a % n) + n) % n, ib = b < n ? b : b % n;
      l += hb[k] * (in[2 * ia] + in[2 * ib]);
      r += hb[k] * (in[2 * ia + 1] + in[2 * ib + 1]);
    }
    out[2 * i] = l;
    out[2 * i + 1] = r;
  }
}

static void add_guard(float* d, int64_t frames) {
  for (int64_t i = 0; i < GS_GUARD; i++) {
    d[2 * (frames + i)] = d[2 * (i % frames)];
    d[2 * (frames + i) + 1] = d[2 * (i % frames) + 1];
  }
}

void gs_sample_free(GsSample* s) {
  if (!s) return;
  for (int k = 0; k < GS_LEVELS; k++) free(s->data[k]);
  free(s);
}

GsSample* gs_sample_load(const char* path, double max_seconds, int* err) {
  int e = GS_LOAD_OK;
  GsSample* s = NULL;
  float* tmp = NULL;
  SNDFILE* f = NULL;
  SF_INFO info;
  memset(&info, 0, sizeof info);
  if (!path || !*path || strlen(path) >= GS_PATH_MAX) { e = GS_LOAD_PATH; goto out; }
  f = sf_open(path, SFM_READ, &info);
  if (!f) { e = GS_LOAD_OPEN; goto out; }
  if (info.frames <= 0 || info.channels <= 0 || info.samplerate <= 0) { e = GS_LOAD_EMPTY; goto out; }
  if ((double)info.frames > max_seconds * info.samplerate) { e = GS_LOAD_TOO_LONG; goto out; }

  const int ch = info.channels;
  s = (GsSample*)calloc(1, sizeof(GsSample));
  tmp = (float*)malloc((size_t)CHUNK * ch * sizeof(float));
  if (!s || !tmp) { e = GS_LOAD_NOMEM; goto out; }
  s->frames[0] = info.frames;
  s->data[0] = (float*)malloc((size_t)(info.frames + GS_GUARD) * 2 * sizeof(float));
  if (!s->data[0]) { e = GS_LOAD_NOMEM; goto out; }

  float* d = s->data[0];
  int64_t n = 0;
  while (n < info.frames) {
    sf_count_t want = info.frames - n < CHUNK ? info.frames - n : CHUNK;
    sf_count_t got = sf_readf_float(f, tmp, want);
    if (got <= 0) break;
    float* o = d + 2 * n;
    if (ch == 1) for (sf_count_t i = 0; i < got; i++) { o[2 * i] = o[2 * i + 1] = tmp[i]; }
    else for (sf_count_t i = 0; i < got; i++) { o[2 * i] = tmp[i * ch]; o[2 * i + 1] = tmp[i * ch + 1]; }
    n += got;
  }
  if (n == 0) { e = GS_LOAD_EMPTY; goto out; }
  /* The header may promise more frames than the file holds: zero the rest (this also touches its pages). */
  memset(d + 2 * n, 0, (size_t)(info.frames - n) * 2 * sizeof(float));
  add_guard(d, info.frames);

  for (int k = 1; k < GS_LEVELS; k++) {
    const int64_t m = s->frames[k - 1] / 2;
    if (m < 4) break;                                  /* too short: grains stay on the levels there are */
    s->data[k] = (float*)malloc((size_t)(m + GS_GUARD) * 2 * sizeof(float));
    if (!s->data[k]) { e = GS_LOAD_NOMEM; goto out; }
    gs_decimate2(s->data[k - 1], s->frames[k - 1], s->data[k]);
    s->frames[k] = m;
    add_guard(s->data[k], m);
  }
  s->channels = ch;
  s->rate = info.samplerate;
  strcpy(s->path, path);

out:
  if (f) sf_close(f);
  free(tmp);
  if (e != GS_LOAD_OK) { gs_sample_free(s); s = NULL; }
  if (err) *err = e;
  return s;
}
