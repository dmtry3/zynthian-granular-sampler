/* Granular engine without LV2: tested in WSL and benchmarked on the Pi as a plain program.
 * Voices (Poly/Mono) each have a playhead, a grain scheduler and an Attack/Release envelope applied to
 * the sum of their grains. Grains come from a shared pool; their parameters are fixed at birth. */
#ifndef GRAIN_CORE_H
#define GRAIN_CORE_H
#include <stdint.h>

#ifndef GC_POOL
#define GC_POOL      64     /* grains for all voices; when the pool is full, new grains are skipped */
#endif                      /* (the bench builds other sizes to find the limit on the Pi) */
#define GC_VOICES    8
#define GC_HELD      16     /* Mono: keys remembered to return to */
#define GC_BLOCK     32     /* control rate: scheduler and playhead are updated every GC_BLOCK samples */
#define GC_WIN       1024   /* samples per window table */
#define GC_SHAPES    31     /* window tables: Shape is quantized to 1/30 (square, decay, bell, reverse exact) */
#define GC_SHAPE_DECAY 10
#define GC_SHAPE_BELL  20   /* loudness reference */
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
#define GC_ATTACK_MIN  1.0f    /* ms */
#define GC_ATTACK_MAX  10000.0f
#define GC_RELEASE_MIN 10.0f   /* ms, to -60 dB */
#define GC_RELEASE_MAX 20000.0f
#define GC_PITCH_MAX   24.0f   /* semitones, both ways */
#define GC_PSPRAY_MAX  24.0f   /* semitones */
#define GC_NSCALES     12      /* see gc_scale_names in grain_core.c */
#define GC_CUTOFF_MIN  20.0f   /* Hz */
#define GC_CUTOFF_MAX  20000.0f
#define GC_FILTER_SMOOTH 0.01f /* s: cutoff glides to the knob (no zipper steps) */
#define GC_LFO_MIN     0.01f   /* Hz */
#define GC_LFO_MAX     20.0f
#define GC_ENV_MIN     0.005f  /* s: Env 1/2 Time */
#define GC_ENV_MAX     10.0f

/* Modulation grid: 7 sources x 9 destinations, each cell -1..1 (the widget shows -100..100). */
enum { GC_SRC_LFO1 = 0, GC_SRC_LFO2, GC_SRC_ENV1, GC_SRC_ENV2, GC_SRC_VEL, GC_SRC_WHEEL, GC_SRC_RANDOM, GC_NSRC };
enum { GC_DST_POSITION = 0, GC_DST_SCAN, GC_DST_SPRAY, GC_DST_SIZE, GC_DST_DENSITY, GC_DST_PITCH,
       GC_DST_PAN, GC_DST_LEVEL, GC_DST_CUTOFF, GC_NDST };
/* Position, Spray, Size, Pitch and Pan are taken at a grain's birth; Scan, Density, Level and Cutoff
 * change in the voice every GC_BLOCK samples. A full cell (+-1, Mod Depth 100 %) moves its destination by: */
#define GC_MOD_POSITION 0.5f   /* of the sample */
#define GC_MOD_SCAN     2.0f   /* x realtime */
#define GC_MOD_SPRAY    1.0f   /* of the window */
#define GC_MOD_OCTAVES  3.0f   /* Size and Density: x 2^(+-3) */
#define GC_MOD_PITCH    24.0f  /* semitones */
#define GC_MOD_PAN      0.5f
#define GC_MOD_CUTOFF   5.0f   /* octaves */
                               /* Level: gain 1 + m, 0..2 */
enum { GC_SYNC_FREE = 0, GC_SYNC_KEY, GC_SYNC_TEMPO };   /* LFO: own phase per voice / reset per note / beats */
#define GC_NDIVS 11   /* tempo LFO cycle lengths, see gc_div_beats */
enum { GC_LFO_SINE = 0, GC_LFO_TRI, GC_LFO_SAW_UP, GC_LFO_SAW_DOWN, GC_LFO_SQUARE, GC_LFO_SH, GC_LFO_SMOOTH,
       GC_NLFO_SHAPES };

enum { GC_INTERP_LINEAR = 0, GC_INTERP_HERMITE };

typedef struct {
  float position;   /* 0..1: where reading starts on a new note */
  float scan;       /* -2..2: speed of the playhead, 1 = realtime; |scan| < GC_SCAN_DEAD = freeze */
  float spray;      /* 0..1: random start offset, fraction of the window */
  float window;     /* 0..1: zone the playhead loops in, fraction of the sample from position */
  float size_ms;    /* grain length */
  float density;    /* grains per second */
  float shape;      /* 0..1: square 0 -> decay 1/3 -> bell 2/3 -> reverse 1 (Beads) */
  float chaos;      /* 0..1: even rhythm -> random (Poisson) */
  float root;       /* MIDI note played at the original pitch */
  float attack_ms;  /* voice envelope: linear rise from the current level */
  float release_ms; /* exponential fall, time to -60 dB */
  float spread;     /* 0..1: random pan of each grain */
  float pitch;      /* -24..24 semitones, added to the note */
  float pitch_spray;/* 0..24: random +-semitones per grain */
  float reverse;    /* 0..1: probability that a grain plays backwards */
  float quantize;   /* 0..1: pull of the grain pitch to the nearest note of the scale */
  float cutoff_hz;  /* voice filter after the grains, before the envelope */
  float resonance;  /* 0..1: Q 0.707 .. 20 */
  int ftype;        /* GC_LP, GC_HP, GC_BP */
  float keytrack;   /* 0..1: the cutoff follows the note (1 = semitone for semitone, from C4) */
  float lfo_rate[2];/* Hz */
  float env_time[2];/* s: attack + decay of Env 1 / Env 2 */
} GcParams;

enum { GC_LP = 0, GC_HP, GC_BP };

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
  float gl, gr;           /* left and right gain: loudness, velocity, pan */
  float fade, fstep;      /* extra gain ramp: 1 and 0, or a fade-out after a sample swap */
  int voice;              /* its voice: the voice envelope is applied to the sum of its grains */
} GcGrain;

enum { GC_V_IDLE = 0, GC_V_ATTACK, GC_V_SUSTAIN, GC_V_RELEASE, GC_V_STEAL };

typedef struct {
  int stage;              /* GC_V_*: grains are born in attack, sustain and release */
  int note;               /* -1 = idle */
  int gate;               /* key held */
  float vel;              /* 0..1 */
  double playhead;        /* in frames of level 0; double: samples are longer than float can index exactly */
  float next;             /* samples until the next grain */
  float env;              /* envelope level at the start of the next block */
  float steal_step;       /* GC_V_STEAL: fall per sample, to 0 in GC_FADE samples */
  int pending_note, pending_vel, pending_off;   /* note to start when the steal fade is over */
  uint32_t age;           /* note-on order: the oldest voice is stolen first */
  int restart;            /* mono without legato: start again (playhead, attack) in the next block */
  float pitch;            /* sounding pitch (MIDI): glides to `note` in mono with Glide, else = note */
  float glide_step;       /* semitones per sample while gliding */
  float fc_log;           /* filter cutoff, log2 Hz, smoothed toward the knob */
  float fc_hz;            /* the cutoff used in the last block (tests, widget) */
  int filter_on;          /* 0: bypassed (LP fully open / HP fully closed) */
  float ic1[2], ic2[2];   /* SVF state, left and right */
  /* modulation sources of this voice: every voice has its own LFOs and envelopes */
  float lfo_ph[2];        /* 0..1 */
  float lfo_a[2], lfo_b[2];  /* random LFO shapes: from, to (one step per cycle) */
  int64_t lfo_cycle[2];   /* tempo sync: number of the current cycle (a new one = new random step) */
  float env_t[2];         /* s since the note started */
  float rnd;              /* the newest grain's Random (-1..1), held for the continuous destinations */
  float src[GC_NSRC];     /* source values in this block */
  float mod[GC_NDST];     /* continuous destinations: summed modulation in this block */
  float level, level_prev;   /* Level destination: gain now and in the previous block */
} GcVoice;

typedef struct {
  float sr;
  GcSource src;
  uint32_t gen;           /* incremented by every gc_set_source */
  int interp;             /* GC_INTERP_* */
  int use_levels;         /* 0: always level 0 (to compare in tests and the bench) */
  int nvoices;            /* 1..GC_VOICES voices */
  int mono;               /* nvoices == 1: back to a held key on release */
  int legato;             /* mono: a new key only changes the pitch (else it starts the voice again) */
  float glide_ms;         /* mono: portamento time from the sounding pitch to a new key (0 = off) */
  int held[GC_HELD], nheld;
  uint32_t age;
  int last_voice;         /* voice of the newest note: its playhead is shown in the widget */
  uint32_t rng;
  GcVoice voices[GC_VOICES];
  float vbuf[GC_VOICES][2][GC_BLOCK];    /* sum of each voice's grains in the current block */
  GcGrain grains[GC_POOL];
  int nactive;
  uint64_t born, skipped; /* statistics: grains born and skipped because the pool was full */
  uint64_t born_back;     /* of them, born backwards (Reverse) */
  double last_start;      /* start frame (level 0) of the newest grain */
  int last_level;         /* level of the newest grain */
  float last_pitch;       /* MIDI pitch of the newest grain (after spray and quantize) */
  int scale, key;         /* Quantize: scale (GC_NSCALES) and tonic 0..11 */
  /* modulation settings (from the widget) */
  float mod_cell[GC_NSRC][GC_NDST];
  int mod_list[GC_NSRC * GC_NDST], nmod;   /* non-zero cells: empty ones cost nothing */
  float mod_depth;        /* 0..1: the whole grid */
  float modwheel;         /* CC 1, 0..1 */
  int lfo_shape[2], lfo_sync[2];           /* GC_SYNC_*: free (voices differ), key (phase 0 per note), tempo */
  int lfo_div[2];                          /* tempo: cycle length, index into gc_div_beats */
  double beat;                             /* tempo clock in beats (song position when the transport rolls) */
  float bpm;
  float env_shape[2];     /* 0 = all decay .. 1 = all attack (then holds) */
  float win_gain[GC_SHAPES];               /* loudness compensation of each window (bell = 1) */
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
void gc_all_notes_off(GrainCore* g);    /* every voice goes to release (CC 123) */
void gc_all_off(GrainCore* g);          /* silence now: grains and voices stopped (CC 120) */
/* n voices (1..GC_VOICES); 1 voice = mono, with legato or starting again on each key.
 * Voices above n finish their sound. */
void gc_set_voices(GrainCore* g, int n, int legato);
/* Mono (1 voice) portamento: a new key while the voice sounds glides there in `ms` (0 = jump). */
void gc_set_glide(GrainCore* g, float ms);
int gc_active_voices(const GrainCore* g);
/* Quantize scale: 0 chromatic, 1 major, 2 minor ... (gc_scale_names), tonic 0 = C .. 11 = B. */
void gc_set_scale(GrainCore* g, int key, int scale);
extern const char* const gc_scale_names[GC_NSCALES];
/* Nearest pitch of the scale to `semis` (MIDI, fractional allowed). */
float gc_snap(const GrainCore* g, float semis);
/* Modulation settings */
void gc_set_mod_cell(GrainCore* g, int src, int dst, float amount);   /* -1..1 */
void gc_set_mod_depth(GrainCore* g, float depth);                     /* 0..1 */
void gc_set_modwheel(GrainCore* g, float value);                      /* 0..1 */
void gc_set_lfo(GrainCore* g, int i, int shape, int sync);       /* sync: GC_SYNC_* */
void gc_set_lfo_div(GrainCore* g, int i, int div);               /* 0..GC_NDIVS-1 */
/* Tempo for synced LFOs; beat >= 0 sets the position (transport rolling), < 0 keeps the clock running. */
void gc_set_tempo(GrainCore* g, float bpm, double beat);
extern const float gc_div_beats[GC_NDIVS];
extern const char* const gc_div_names[GC_NDIVS];
void gc_set_env(GrainCore* g, int i, float shape);
float gc_lfo_value(int shape, float ph, float a, float b);            /* -1..1 (widget, tests) */
float gc_env_value(float shape, float t, float time);                 /* 0..1 */
/* Writes n samples (not adds). */
void gc_process(GrainCore* g, const GcParams* p, float* out_l, float* out_r, uint32_t n);
/* Flush denormals to zero in this thread (call at the start of run()). */
void gc_enable_ftz(void);

#endif
