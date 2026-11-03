/* Tests of gs_sample_load: writes small files with libsndfile and loads them back. */
#include <math.h>
#include <sndfile.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../src/sample.h"

static int fails = 0;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

/* Writes `frames` frames: channel c of frame i = (i + 1000 * c) / 100000. */
static void write_file(const char* path, int format, int ch, int rate, int frames) {
  SF_INFO info = { 0 };
  info.channels = ch; info.samplerate = rate; info.format = format;
  SNDFILE* f = sf_open(path, SFM_WRITE, &info);
  if (!f) { printf("can't write %s: %s\n", path, sf_strerror(NULL)); fails++; return; }
  for (int i = 0; i < frames; i++) {
    float fr[4];
    for (int c = 0; c < ch; c++) fr[c] = (float)(i + 1000 * c) / 100000.0f;
    sf_writef_float(f, fr, 1);
  }
  sf_close(f);
}

static int near(float a, float b) { return fabsf(a - b) < 1e-4f; }
#define L(s, k, i) ((s)->data[k][2 * (i)])
#define R(s, k, i) ((s)->data[k][2 * (i) + 1])

/* Level of a stereo sine of `cycles_per_frame` after decimation: RMS * sqrt(2), 1 = unchanged. */
static float decimated_level(double cycles_per_frame) {
  const int n = 8192;
  float* in = malloc((n + GS_GUARD) * 2 * sizeof(float));
  float* out = malloc((n / 2) * 2 * sizeof(float));
  for (int i = 0; i < n; i++) in[2 * i] = in[2 * i + 1] = (float)sin(2 * M_PI * cycles_per_frame * i);
  gs_decimate2(in, n, out);
  double e = 0;
  int cnt = 0;
  for (int i = 100; i < n / 2 - 100; i++, cnt++) e += (double)out[2 * i] * out[2 * i];   /* away from the loop seam */
  free(in); free(out);
  return (float)sqrt(2.0 * e / cnt);
}

int main(void) {
  int err;
  /* stereo float WAV, longer than one read chunk */
  write_file("st.wav", SF_FORMAT_WAV | SF_FORMAT_FLOAT, 2, 44100, 10000);
  GsSample* s = gs_sample_load("st.wav", 600, &err);
  CHECK(s && err == GS_LOAD_OK);
  if (s) {
    CHECK(s->frames[0] == 10000 && s->channels == 2 && s->rate == 44100);
    CHECK(near(L(s, 0, 0), 0.0f) && near(R(s, 0, 0), 0.01f));                  /* interleaved L R */
    CHECK(near(L(s, 0, 9999), 0.09999f) && near(R(s, 0, 9999), 0.10999f));
    CHECK(L(s, 0, 10000) == L(s, 0, 0) && R(s, 0, 10003) == R(s, 0, 3));        /* guard frames = start */
    CHECK(s->frames[1] == 5000 && s->frames[2] == 2500 && s->data[1] && s->data[2]);
    CHECK(fabsf(L(s, 1, 2000) - 0.04f) < 1e-3f);                                /* level 1: every other frame */
    CHECK(L(s, 2, 2500) == L(s, 2, 0));
    CHECK(!strcmp(s->path, "st.wav"));
    gs_sample_free(s);
  }
  /* mono 16-bit WAV: stored as L = R */
  write_file("mono.wav", SF_FORMAT_WAV | SF_FORMAT_PCM_16, 1, 48000, 5000);
  s = gs_sample_load("mono.wav", 600, &err);
  CHECK(s && s->channels == 1 && s->frames[0] == 5000);
  if (s) { CHECK(fabsf(L(s, 0, 4000) - 0.04f) < 1e-3f && L(s, 0, 4000) == R(s, 0, 4000)); gs_sample_free(s); }
  /* 4 channels: the first two are kept */
  write_file("quad.wav", SF_FORMAT_WAV | SF_FORMAT_FLOAT, 4, 48000, 100);
  s = gs_sample_load("quad.wav", 600, &err);
  CHECK(s && s->channels == 4 && near(R(s, 0, 50), 0.0105f));
  gs_sample_free(s);
  /* very short file: level 0 only where halving leaves too little */
  write_file("tiny.wav", SF_FORMAT_WAV | SF_FORMAT_FLOAT, 1, 48000, 10);
  s = gs_sample_load("tiny.wav", 600, &err);
  CHECK(s && s->frames[0] == 10 && s->frames[1] == 5 && !s->data[2]);
  gs_sample_free(s);
  /* FLAC */
  write_file("st.flac", SF_FORMAT_FLAC | SF_FORMAT_PCM_16, 2, 48000, 3000);
  s = gs_sample_load("st.flac", 600, &err);
  CHECK(s && s->frames[0] == 3000);
  gs_sample_free(s);
  /* half-band decimation: low tones pass, tones above the new Nyquist are gone */
  CHECK(fabsf(decimated_level(0.02) - 1.0f) < 0.01f);      /* 960 Hz at 48 kHz */
  CHECK(fabsf(decimated_level(0.19) - 1.0f) < 0.01f);      /* 9.1 kHz: still in the passband */
  CHECK(decimated_level(0.30) < 1e-3f);                    /* 14.4 kHz -> would fold to 9.6 kHz: < -60 dB */
  CHECK(decimated_level(0.45) < 1e-3f);                    /* 21.6 kHz */
  /* errors */
  s = gs_sample_load("missing.wav", 600, &err);
  CHECK(!s && err == GS_LOAD_OPEN);
  s = gs_sample_load("st.wav", 0.1, &err);                 /* 10000 frames > 0.1 s */
  CHECK(!s && err == GS_LOAD_TOO_LONG);
  s = gs_sample_load("", 600, &err);
  CHECK(!s && err == GS_LOAD_PATH);
  FILE* f = fopen("junk.wav", "wb"); fputs("not a wav file", f); fclose(f);
  s = gs_sample_load("junk.wav", 600, &err);
  CHECK(!s && err == GS_LOAD_OPEN);
  printf(fails ? "%d FAILED\n" : "All sample tests passed\n", fails);
  return fails != 0;
}
