/* Tests of gs_sample_load: writes small files with libsndfile and loads them back. */
#include <math.h>
#include <sndfile.h>
#include <stdio.h>
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

int main(void) {
  int err;
  /* stereo float WAV, longer than one read chunk */
  write_file("st.wav", SF_FORMAT_WAV | SF_FORMAT_FLOAT, 2, 44100, 10000);
  GsSample* s = gs_sample_load("st.wav", 600, &err);
  CHECK(s && err == GS_LOAD_OK);
  if (s) {
    CHECK(s->frames == 10000 && s->channels == 2 && s->rate == 44100);
    CHECK(s->data[0] != s->data[1]);
    CHECK(near(s->data[0][0], 0.0f) && near(s->data[1][0], 0.01f));
    CHECK(near(s->data[0][9999], 0.09999f) && near(s->data[1][9999], 0.10999f));
    CHECK(!strcmp(s->path, "st.wav"));
    gs_sample_free(s);
  }
  /* mono 16-bit WAV: right channel is the left one */
  write_file("mono.wav", SF_FORMAT_WAV | SF_FORMAT_PCM_16, 1, 48000, 5000);
  s = gs_sample_load("mono.wav", 600, &err);
  CHECK(s && s->channels == 1 && s->data[0] == s->data[1] && s->frames == 5000);
  if (s) { CHECK(fabsf(s->data[0][4000] - 0.04f) < 1e-3f); gs_sample_free(s); }
  /* 4 channels: the first two are kept */
  write_file("quad.wav", SF_FORMAT_WAV | SF_FORMAT_FLOAT, 4, 48000, 100);
  s = gs_sample_load("quad.wav", 600, &err);
  CHECK(s && s->channels == 4 && near(s->data[1][50], 0.0105f));
  gs_sample_free(s);
  /* FLAC */
  write_file("st.flac", SF_FORMAT_FLAC | SF_FORMAT_PCM_16, 2, 48000, 3000);
  s = gs_sample_load("st.flac", 600, &err);
  CHECK(s && s->frames == 3000);
  gs_sample_free(s);
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
