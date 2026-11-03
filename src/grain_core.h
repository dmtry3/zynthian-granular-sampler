/* Granular engine without LV2: tested in WSL and benchmarked on the Pi as a plain program.
 * Stage 2: one voice. Grains come from a shared pool; their parameters are fixed at birth. */
#ifndef GRAIN_CORE_H
#define GRAIN_CORE_H
#include <stdint.h>

#define GC_POOL      64     /* grains for all voices; when the pool is full, new grains are skipped */
#define GC_BLOCK     32     /* control rate: scheduler and playhead are updated every GC_BLOCK samples */
#define GC_WIN       1024   /* samples per window table */
#define GC_SHAPES    33     /* window tables: Shape is quantized to 1/32 */
#define GC_SCAN_DEAD 0.02f  /* |Scan| below this is 0 (freeze) */
#define GC_LEVELS    3      /* source levels: level k is the sample at 1/2^k rate, band-limited */
#define GC_MIP_UP    1.5f   /* a grain reading faster than this switches to the next level */
#define GC_FADE      240    /* samples: grains of a replaced sample fade out (5 ms at 48 kHz) */

/* Parameter ranges; the LV2 ports use the same ones. */
#define GC_SIZE_MIN    5.0f    /* ms */
#define GC_SIZE_MAX    1000.0f
#define GC_DENS_MIN    0.5f    /* grains per second */
#define GC_DENS_MAX    200.0f
#define GC_SCAN_MAX    2.0f    /* x realtime, both directions */
#define GC_WINDOW_MIN  0.001f  /* fraction of the sample */

enum { GC_INTERP_LINEAR = 0, GC_INTERP_HERMITE };

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

/* The sample as the engine reads it: interleaved stereo, GC_LEVELS rates, each level followed by
 * at least 2 guard frames that repeat its start (see sample.h). */
typedef struct {
  const float* data[GC_LEVELS];
  int64_t frames[GC_LEVELS];
  int nlevels;
  float rate;             /* sample rate of the file */
} GcSource;

typedef struct {
  int active;
  int delay;              /* samples to wait in the current block before starting */
  int remain;             /* samples left */
  uint32_t gen;           /* source generation it reads (see gc_set_source) */
  const float* data;      /* its own source level: it may outlive a sample swap by GC_FADE samples */
  int64_t frames;
  int64_t idx;            /* read position in frames of its level, integer part */
  float frac;             /* and fraction */
  float inc;              /* read step per output sample (pitch, file rate, level) */
  float wph, wstep;       /* window phase in table samples */
  const float* win;       /* window table */
  float gain;
  float fade, fstep;      /* extra gain ramp: 1 and 0, or a fade-out after a sample swap */
} GcGrain;

typedef struct {
  int note;               /* -1 = idle */
  int gate;               /* key held: grains are born */
  float vel;              /* 0..1 */
  double playhead;        /* in frames of level 0; double: samples are longer than float can index exactly */
  float next;             /* samples until the next grain */
} GcVoice;

typedef struct {
  float sr;
  GcSource src;
  uint32_t gen;           /* incremented by every gc_set_source */
  int interp;             /* GC_INTERP_* */
  int use_levels;         /* 0: always level 0 (to compare in tests and the bench) */
  uint32_t rng;
  GcVoice voice;
  GcGrain grains[GC_POOL];
  int nactive;
  uint64_t born, skipped; /* statistics: grains born and skipped because the pool was full */
  double last_start;      /* start frame (level 0) of the newest grain */
  int last_level;         /* level of the newest grain */
  float win_gain[GC_SHAPES];               /* loudness compensation of each window (Hann = 1) */
  float win[GC_SHAPES][GC_WIN + 1];
} GrainCore;

void gc_init(GrainCore* g, float sr, uint32_t seed);
void gc_default_params(GcParams* p);
/* New sample (NULL = none). Grains of the old one fade out over GC_FADE samples, still reading it:
 * free it only when gc_gen_busy(g, old generation) is 0. Returns the generation of the new source. */
uint32_t gc_set_source(GrainCore* g, const GcSource* s);
int gc_gen_busy(const GrainCore* g, uint32_t gen);
void gc_note_on(GrainCore* g, const GcParams* p, int note, int vel);
void gc_note_off(GrainCore* g, int note);
void gc_all_off(GrainCore* g);
/* Writes n samples (not adds). */
void gc_process(GrainCore* g, const GcParams* p, float* out_l, float* out_r, uint32_t n);
/* Flush denormals to zero in this thread (call at the start of run()). */
void gc_enable_ftz(void);

#endif
