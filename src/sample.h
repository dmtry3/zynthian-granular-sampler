/* Sample file loading (libsndfile), no LV2: runs in the LV2 worker thread and in tests. */
#ifndef GS_SAMPLE_H
#define GS_SAMPLE_H
#include <stdint.h>

#define GS_PATH_MAX 1024
#define GS_LEVELS   3       /* level k: the sample at 1/2^k rate, band-limited: grains pitched up read it */
#define GS_GUARD    4       /* extra frames after each level's end (copies of the start) for interpolation */

typedef struct {
  /* Interleaved stereo (L R L R ...): a frame and the next one sit in one cache line.
   * A mono file is stored as L = R. Each level has frames[k] + GS_GUARD frames. */
  float* data[GS_LEVELS];
  int64_t frames[GS_LEVELS];
  int channels;           /* channels in the file (only the first two are used) */
  double rate;            /* sample rate of the file */
  float pitch;            /* detected pitch (MIDI, fractional), -1 = none found (drums, noise, a melody) */
  char path[GS_PATH_MAX];
} GsSample;

enum { GS_LOAD_OK = 0, GS_LOAD_OPEN, GS_LOAD_EMPTY, GS_LOAD_TOO_LONG, GS_LOAD_NOMEM, GS_LOAD_PATH };

/* Loads the whole file into memory and builds the levels. Every page of the buffers is written here,
 * so the audio thread never takes a page fault on them. NULL on error, *err says why. */
GsSample* gs_sample_load(const char* path, double max_seconds, int* err);
void gs_sample_free(GsSample* s);

/* Pitch of interleaved stereo frames (MIDI, fractional) or -1: YIN in several windows, they must agree.
 * Runs in the worker when a sample loads (exported for tests). */
float gs_detect_pitch(const float* data, int64_t frames, double rate);

/* Halves the rate of an interleaved stereo buffer with a half-band low-pass (exported for tests). */
void gs_decimate2(const float* in, int64_t n, float* out);

#endif
