/* Granular engine without LV2: tested in WSL and benchmarked on the Pi as a plain program.
 * Stage 2: one voice. Grains come from a shared pool; their parameters are fixed at birth. */
#ifndef GRAIN_CORE_H
#define GRAIN_CORE_H
#include <stdint.h>

#define GC_POOL     64      /* grains for all voices; when the pool is full, new grains are skipped */
#define GC_BLOCK    32      /* control rate: scheduler and playhead are updated every GC_BLOCK samples */
#define GC_WIN      1024    /* samples per window table */
#define GC_SHAPES   33      /* window tables: Shape is quantized to 1/32 */
#define GC_SCAN_DEAD 0.02f  /* |Scan| below this is 0 (freeze) */

/* Parameter ranges; the LV2 ports use the same ones. */
#define GC_SIZE_MIN    5.0f    /* ms */
#define GC_SIZE_MAX    1000.0f
#define GC_DENS_MIN    0.5f    /* grains per second */
#define GC_DENS_MAX    200.0f
#define GC_SCAN_MAX    2.0f    /* x realtime, both directions */
#define GC_WINDOW_MIN  0.001f  /* fraction of the sample */

typedef struct {
  float position;   /* 0..1: where reading starts on a new note */
  float scan;       /* -2..2: speed of the playhead, 1 = realtime; |scan| < GC_SCAN_DEAD = freeze */
  float spray;      /* 0..1: random start offset, fraction of the window */
  float window;     /* 0..1: zone the playhead loops in, fraction of the sample from position */
  float size_ms;    /* grain length */
  float density;    /* grains per second */
  float shape;      /* 0..1: narrow Gauss -> Hann (0.5) -> Tukey -> almost rectangular */
  float chaos;      /* 0..1: even rhythm -> random (Poisson) */
  float root;       /* MIDI note played at the original pitch */
} GcParams;

typedef struct {
  int active;
  int delay;              /* samples to wait in the current block before starting */
  int remain;             /* samples left */
  int64_t idx;            /* read position in the sample, integer part */
  float frac;             /* and fraction */
  float inc;              /* read step per output sample (pitch and file rate) */
  float wph, wstep;       /* window phase in table samples */
  const float* win;       /* window table */
  float gain;
} GcGrain;

typedef struct {
  int note;               /* -1 = idle */
  int gate;               /* key held: grains are born */
  float vel;              /* 0..1 */
  double playhead;        /* in sample frames; double: samples are longer than float can index exactly */
  float next;             /* samples until the next grain */
} GcVoice;

typedef struct {
  float sr;
  const float* data[2];
  int64_t frames;
  float rate_ratio;       /* file rate / host rate */
  uint32_t rng;
  GcVoice voice;
  GcGrain grains[GC_POOL];
  int nactive;
  uint64_t born, skipped; /* statistics: grains born and skipped because the pool was full */
  double last_start;      /* start frame of the newest grain */
  float win[GC_SHAPES][GC_WIN + 1];
} GrainCore;

void gc_init(GrainCore* g, float sr, uint32_t seed);
void gc_default_params(GcParams* p);
/* New sample (NULL = none): all grains stop. Called from the audio thread when the worker swapped the sample. */
void gc_set_source(GrainCore* g, const float* l, const float* r, int64_t frames, float file_rate);
void gc_note_on(GrainCore* g, const GcParams* p, int note, int vel);
void gc_note_off(GrainCore* g, int note);
void gc_all_off(GrainCore* g);
/* Writes n samples (not adds). */
void gc_process(GrainCore* g, const GcParams* p, float* out_l, float* out_r, uint32_t n);
/* Flush denormals to zero in this thread (call at the start of run()). */
void gc_enable_ftz(void);

#endif
