#include "sample.h"
#include <sndfile.h>
#include <stdlib.h>
#include <string.h>

#define CHUNK 4096

void gs_sample_free(GsSample* s) {
  if (!s) return;
  if (s->data[1] != s->data[0]) free(s->data[1]);
  free(s->data[0]);
  free(s);
}

GsSample* gs_sample_load(const char* path, double max_seconds, int* err) {
  int e = GS_LOAD_OK;
  GsSample* s = NULL;
  float* tmp = NULL;
  SF_INFO info;
  memset(&info, 0, sizeof info);
  if (!path || !*path || strlen(path) >= GS_PATH_MAX) { e = GS_LOAD_PATH; goto out; }
  SNDFILE* f = sf_open(path, SFM_READ, &info);
  if (!f) { e = GS_LOAD_OPEN; goto out; }
  if (info.frames <= 0 || info.channels <= 0 || info.samplerate <= 0) { e = GS_LOAD_EMPTY; goto close; }
  if ((double)info.frames > max_seconds * info.samplerate) { e = GS_LOAD_TOO_LONG; goto close; }

  const int ch = info.channels;
  s = (GsSample*)calloc(1, sizeof(GsSample));
  tmp = (float*)malloc((size_t)CHUNK * ch * sizeof(float));
  if (!s || !tmp) { e = GS_LOAD_NOMEM; goto close; }
  s->data[0] = (float*)malloc((size_t)info.frames * sizeof(float));
  s->data[1] = ch > 1 ? (float*)malloc((size_t)info.frames * sizeof(float)) : s->data[0];
  if (!s->data[0] || !s->data[1]) { e = GS_LOAD_NOMEM; goto close; }

  int64_t n = 0;
  while (n < info.frames) {
    sf_count_t want = info.frames - n < CHUNK ? info.frames - n : CHUNK;
    sf_count_t got = sf_readf_float(f, tmp, want);
    if (got <= 0) break;
    float* l = s->data[0] + n;
    if (ch == 1) memcpy(l, tmp, (size_t)got * sizeof(float));
    else {
      float* r = s->data[1] + n;
      for (sf_count_t i = 0; i < got; i++) { l[i] = tmp[i * ch]; r[i] = tmp[i * ch + 1]; }
    }
    n += got;
  }
  if (n == 0) { e = GS_LOAD_EMPTY; goto close; }
  /* The header may promise more frames than the file holds: zero the rest (this also touches its pages). */
  for (int64_t i = n; i < info.frames; i++) { s->data[0][i] = 0.0f; s->data[1][i] = 0.0f; }
  s->frames = info.frames;
  s->channels = ch;
  s->rate = info.samplerate;
  strcpy(s->path, path);

close:
  sf_close(f);
out:
  free(tmp);
  if (e != GS_LOAD_OK) { gs_sample_free(s); s = NULL; }
  if (err) *err = e;
  return s;
}
