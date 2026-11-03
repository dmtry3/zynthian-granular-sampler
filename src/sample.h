/* Sample file loading (libsndfile), no LV2: runs in the LV2 worker thread and in tests. */
#ifndef GS_SAMPLE_H
#define GS_SAMPLE_H
#include <stdint.h>

#define GS_PATH_MAX 1024

typedef struct {
  float* data[2];         /* planar L/R; for a mono file data[1] == data[0] */
  int64_t frames;
  int channels;           /* channels in the file (only the first two are used) */
  double rate;            /* sample rate of the file */
  char path[GS_PATH_MAX];
} GsSample;

enum { GS_LOAD_OK = 0, GS_LOAD_OPEN, GS_LOAD_EMPTY, GS_LOAD_TOO_LONG, GS_LOAD_NOMEM, GS_LOAD_PATH };

/* Loads the whole file into memory. Every page of the buffers is written here,
 * so the audio thread never takes a page fault on them. NULL on error, *err says why. */
GsSample* gs_sample_load(const char* path, double max_seconds, int* err);
void gs_sample_free(GsSample* s);

#endif
