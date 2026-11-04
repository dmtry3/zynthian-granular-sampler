/* Granular Sampler: LV2 wrapper around grain_core.
 * The sample file is an LV2 parameter (atom:Path), loaded by the LV2 worker, saved in LV2 State.
 * Knob pages Position, Grain, Voice; Poly/Mono and the number of voices are LV2 parameters set in the widget.
 * Read positions go to output ports for the widget. */
#include <lv2/core/lv2.h>
#include <lv2/atom/atom.h>
#include <lv2/atom/forge.h>
#include <lv2/atom/util.h>
#include <lv2/midi/midi.h>
#include <lv2/patch/patch.h>
#include <lv2/state/state.h>
#include <lv2/urid/urid.h>
#include <lv2/worker/worker.h>
#include <math.h>
#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "grain_core.h"
#include "sample.h"

#define GS_URI "urn:granularsampler:sampler"
#define MAX_SECONDS 600.0   /* longest sample, in seconds of the file */

/* Control input ports come first (0..P_END-1): Zynthian numbers LV2 parameters from len(control ports)+1,
 * so a control port with a higher index would be overwritten by a parameter in its list. */
enum { P_POSITION = 0, P_SCAN, P_SPRAY, P_WINDOW,      /* page Position */
       P_SIZE, P_DENSITY, P_SHAPE, P_CHAOS,           /* page Grain */
       P_PITCH, P_PSPRAY, P_REVERSE, P_QUANTIZE,      /* page Pitch */
       P_ATTACK, P_RELEASE, P_SPREAD, P_LEVEL,        /* page Voice */
       P_END,
       P_MIDI_IN = P_END, P_OUT_L, P_OUT_R, P_NOTIFY,
       P_STATUS, P_SECONDS, P_PLAYHEAD, P_GRAIN0,     /* outputs for the widget */
       P_NOTE0 = P_GRAIN0 + 16 };                     /* note1..8: MIDI note of each sounding voice */
#define N_GRAIN_PORTS 16
#define N_RETIRED 8

enum { IP_VOICES = 0, IP_SCALE, IP_KEY, N_IPARAMS };
static const char* const ip_names[N_IPARAMS] = { "voices", "scale", "key" };
static const int ip_defaults[N_IPARAMS] = { GC_VOICES, 0, 0 };                              /* grain1..16: read positions of some active grains */
#define N_CTL P_END

/* Value of the status output port. */
enum { ST_EMPTY = 0, ST_LOADING, ST_LOADED, ST_ERROR };

/* Worker messages (run -> worker) and responses (worker -> run). */
enum { MSG_LOAD = 1, MSG_FREE };
typedef struct { uint32_t type, seq; char path[GS_PATH_MAX]; } MsgLoad;
typedef struct { uint32_t type; GsSample* s; } MsgFree;
typedef struct { uint32_t seq; int err; GsSample* s; } MsgDone;

typedef struct {
  const float* ctl[N_CTL];
  const LV2_Atom_Sequence* in;
  LV2_Atom_Sequence* notify;
  float* out_l;
  float* out_r;
  float* status_out;
  float* seconds_out;
  float* playhead_out;
  float* grain_out[N_GRAIN_PORTS];
  float* note_out[GC_VOICES];

  LV2_URID_Map* map;
  LV2_Worker_Schedule* sched;
  LV2_Atom_Forge forge;
  LV2_URID midi_Event, atom_Object, atom_Path, atom_String, atom_URID;
  LV2_URID patch_Set, patch_Get, patch_property, patch_value, p_sample;
  LV2_URID atom_Int, atom_Float, atom_Double, atom_Long;

  GsSample* sample;            /* owned by the audio thread; swapped in work_response() */
  /* Replaced samples: their grains fade out for GC_FADE samples, then they go to the worker to be freed. */
  struct { GsSample* s; uint32_t gen; } retired[N_RETIRED];
  int nretired;
  char path[GS_PATH_MAX];      /* last requested path ("" = none), saved in State */
  atomic_uint req_seq;         /* number of the newest load request; older ones are skipped */
  int status;
  int report;                  /* tell the host the path and the integer parameters in the next run() */
  /* integer LV2 parameters set in the widget: voices 1..8 (1 = mono, legato), scale, key */
  LV2_URID ip_urid[N_IPARAMS];
  LV2_URID p_mode_legacy;      /* "mode" of older versions: Mono (1) is restored as 1 voice */
  int ip[N_IPARAMS];
  double sr;
  float level;                 /* last applied level, ramped to the port value over a block */
  GcParams params;
  GrainCore core;
} Sampler;

/* The engine plays the current sample (audio thread, or restore() which is not concurrent with run()). */
static uint32_t use_sample(Sampler* S) {
  const GsSample* s = S->sample;
  if (!s) return gc_set_source(&S->core, NULL);
  GcSource src;
  memset(&src, 0, sizeof src);
  for (int k = 0; k < GC_LEVELS && k < GS_LEVELS && s->data[k]; k++) {
    src.data[k] = s->data[k];
    src.frames[k] = s->frames[k];
    src.nlevels = k + 1;
  }
  src.rate = (float)s->rate;
  return gc_set_source(&S->core, &src);
}

static void free_in_worker(Sampler* S, GsSample* s) {
  MsgFree fm = { MSG_FREE, s };
  S->sched->schedule_work(S->sched->handle, sizeof fm, &fm);
}

/* Audio thread: the new sample plays, the old one waits until its grains have faded out. */
static void swap_sample(Sampler* S, GsSample* s) {
  GsSample* old = S->sample;
  const uint32_t old_gen = S->core.gen;
  S->sample = s;
  use_sample(S);
  if (!old) return;
  if (S->nretired == N_RETIRED) {                    /* very fast swaps: cut the fading grains short */
    gc_all_off(&S->core);
    for (int i = 0; i < S->nretired; i++) free_in_worker(S, S->retired[i].s);
    S->nretired = 0;
  }
  S->retired[S->nretired].s = old;
  S->retired[S->nretired].gen = old_gen;
  S->nretired++;
}

static void free_retired(Sampler* S) {
  for (int i = 0; i < S->nretired;)
    if (!gc_gen_busy(&S->core, S->retired[i].gen)) {
      free_in_worker(S, S->retired[i].s);
      S->retired[i] = S->retired[--S->nretired];
    } else i++;
}

static int is_empty_path(const char* p) { return !p[0] || !strcmp(p, "_"); }   /* Zynthian sends "_" to clear */

static LV2_Handle instantiate(const LV2_Descriptor* d, double rate, const char* bundle,
                              const LV2_Feature* const* f) {
  (void)d; (void)bundle;
  Sampler* S = (Sampler*)calloc(1, sizeof(Sampler));
  if (!S) return NULL;
  for (int i = 0; f[i]; i++) {
    if (!strcmp(f[i]->URI, LV2_URID__map)) S->map = (LV2_URID_Map*)f[i]->data;
    else if (!strcmp(f[i]->URI, LV2_WORKER__schedule)) S->sched = (LV2_Worker_Schedule*)f[i]->data;
  }
  if (!S->map || !S->sched) { free(S); return NULL; }
  LV2_URID_Map* m = S->map;
  S->midi_Event     = m->map(m->handle, LV2_MIDI__MidiEvent);
  S->atom_Object    = m->map(m->handle, LV2_ATOM__Object);
  S->atom_Path      = m->map(m->handle, LV2_ATOM__Path);
  S->atom_String    = m->map(m->handle, LV2_ATOM__String);
  S->atom_URID      = m->map(m->handle, LV2_ATOM__URID);
  S->patch_Set      = m->map(m->handle, LV2_PATCH__Set);
  S->patch_Get      = m->map(m->handle, LV2_PATCH__Get);
  S->patch_property = m->map(m->handle, LV2_PATCH__property);
  S->patch_value    = m->map(m->handle, LV2_PATCH__value);
  S->p_sample       = m->map(m->handle, GS_URI "#sample");
  for (int i = 0; i < N_IPARAMS; i++) {
    char uri[96];
    snprintf(uri, sizeof uri, GS_URI "#%s", ip_names[i]);
    S->ip_urid[i] = m->map(m->handle, uri);
    S->ip[i] = ip_defaults[i];
  }
  S->p_mode_legacy = m->map(m->handle, GS_URI "#mode");
  S->atom_Int       = m->map(m->handle, LV2_ATOM__Int);
  S->atom_Float     = m->map(m->handle, LV2_ATOM__Float);
  S->atom_Double    = m->map(m->handle, LV2_ATOM__Double);
  S->atom_Long      = m->map(m->handle, LV2_ATOM__Long);
  lv2_atom_forge_init(&S->forge, m);
  atomic_init(&S->req_seq, 0);
  S->sr = rate;
  S->level = -1.0f;
  gc_init(&S->core, (float)rate, (uint32_t)(uintptr_t)S ^ 0x5EEDu);
  gc_default_params(&S->params);
  return S;
}

static void connect_port(LV2_Handle h, uint32_t port, void* data) {
  Sampler* S = (Sampler*)h;
  if (port < P_END) S->ctl[port] = (const float*)data;
  else if (port == P_MIDI_IN) S->in = (const LV2_Atom_Sequence*)data;
  else if (port == P_OUT_L) S->out_l = (float*)data;
  else if (port == P_OUT_R) S->out_r = (float*)data;
  else if (port == P_NOTIFY) S->notify = (LV2_Atom_Sequence*)data;
  else if (port == P_STATUS) S->status_out = (float*)data;
  else if (port == P_SECONDS) S->seconds_out = (float*)data;
  else if (port == P_PLAYHEAD) S->playhead_out = (float*)data;
  else if (port >= P_GRAIN0 && port < P_GRAIN0 + N_GRAIN_PORTS) S->grain_out[port - P_GRAIN0] = (float*)data;
  else if (port >= P_NOTE0 && port < P_NOTE0 + GC_VOICES) S->note_out[port - P_NOTE0] = (float*)data;
}

static void activate(LV2_Handle h) { (void)h; }
static void deactivate(LV2_Handle h) { (void)h; }

/* ---------- worker (non-realtime thread) ---------- */
/* Messages are copied out with memcpy: the host does not promise any alignment of `data`. */
static LV2_Worker_Status work(LV2_Handle h, LV2_Worker_Respond_Function respond, LV2_Worker_Respond_Handle rh,
                              uint32_t size, const void* data) {
  Sampler* S = (Sampler*)h;
  uint32_t type;
  if (size < sizeof type) return LV2_WORKER_ERR_UNKNOWN;
  memcpy(&type, data, sizeof type);
  if (type == MSG_FREE && size == sizeof(MsgFree)) {
    MsgFree fm;
    memcpy(&fm, data, sizeof fm);
    gs_sample_free(fm.s);
    return LV2_WORKER_SUCCESS;
  }
  if (type != MSG_LOAD || size <= offsetof(MsgLoad, path) || size > sizeof(MsgLoad)) return LV2_WORKER_ERR_UNKNOWN;
  MsgLoad msg;
  memcpy(&msg, data, size);
  msg.path[size - offsetof(MsgLoad, path) - 1] = 0;
  /* The file selector sends every file the cursor stops on: skip requests that are already replaced. */
  if (msg.seq != atomic_load(&S->req_seq)) return LV2_WORKER_SUCCESS;
  MsgDone r = { msg.seq, GS_LOAD_OK, NULL };
  if (!is_empty_path(msg.path)) r.s = gs_sample_load(msg.path, MAX_SECONDS, &r.err);
  respond(rh, sizeof r, &r);
  return LV2_WORKER_SUCCESS;
}

/* Called in the audio thread after work() responded: swap the sample in, free the old one in the worker. */
static LV2_Worker_Status work_response(LV2_Handle h, uint32_t size, const void* data) {
  Sampler* S = (Sampler*)h;
  if (size != sizeof(MsgDone)) return LV2_WORKER_ERR_UNKNOWN;
  MsgDone r;
  memcpy(&r, data, sizeof r);
  if (r.seq != atomic_load(&S->req_seq)) {          /* stale result: thrown away */
    if (r.s) free_in_worker(S, r.s);
    return LV2_WORKER_SUCCESS;
  }
  S->status = r.s ? ST_LOADED : (r.err ? ST_ERROR : ST_EMPTY);
  swap_sample(S, r.s);
  return LV2_WORKER_SUCCESS;
}

/* ---------- audio thread ---------- */
static void request_load(Sampler* S, const char* path, uint32_t len) {
  if (len >= GS_PATH_MAX) len = GS_PATH_MAX - 1;
  memcpy(S->path, path, len);
  S->path[len] = 0;
  while (len && !S->path[len - 1]) len--;            /* atom strings carry their own terminator */
  MsgLoad msg;
  msg.type = MSG_LOAD;
  msg.seq = atomic_load(&S->req_seq) + 1;
  memcpy(msg.path, S->path, len + 1);
  atomic_store(&S->req_seq, msg.seq);
  S->status = is_empty_path(S->path) ? ST_EMPTY : ST_LOADING;
  S->sched->schedule_work(S->sched->handle, (uint32_t)(offsetof(MsgLoad, path) + len + 1), &msg);
}

/* A number of any atom type (Zynthian sends floats), or `def`. */
static int atom_int(const Sampler* S, const LV2_Atom* a, int def) {
  if (a->type == S->atom_Int) return ((const LV2_Atom_Int*)a)->body;
  if (a->type == S->atom_Long) return (int)((const LV2_Atom_Long*)a)->body;
  if (a->type == S->atom_Float) return (int)lrintf(((const LV2_Atom_Float*)a)->body);
  if (a->type == S->atom_Double) return (int)lrint(((const LV2_Atom_Double*)a)->body);
  return def;
}

static int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

/* Integer parameters into range, then into the engine. */
static void apply_iparams(Sampler* S) {
  S->ip[IP_VOICES] = clampi(S->ip[IP_VOICES], 1, GC_VOICES);
  S->ip[IP_SCALE] = clampi(S->ip[IP_SCALE], 0, GC_NSCALES - 1);
  S->ip[IP_KEY] = clampi(S->ip[IP_KEY], 0, 11);
  gc_set_voices(&S->core, S->ip[IP_VOICES]);
  gc_set_scale(&S->core, S->ip[IP_KEY], S->ip[IP_SCALE]);
}

static void handle_patch_set(Sampler* S, const LV2_Atom_Object* obj) {
  const LV2_Atom *prop = NULL, *val = NULL;
  lv2_atom_object_get(obj, S->patch_property, &prop, S->patch_value, &val, 0);
  if (!prop || prop->type != S->atom_URID || !val) return;
  const LV2_URID key = ((const LV2_Atom_URID*)prop)->body;
  for (int i = 0; i < N_IPARAMS; i++)
    if (key == S->ip_urid[i]) { S->ip[i] = atom_int(S, val, S->ip[i]); apply_iparams(S); return; }
  if (key != S->p_sample) return;
  if (val->type != S->atom_Path && val->type != S->atom_String) return;
  request_load(S, (const char*)LV2_ATOM_BODY_CONST(val), val->size);
}

/* patch:Set of the parameters on the notify port: jalv prints them, Zynthian's controllers follow. */
static int report_param(Sampler* S, LV2_URID key, int ival) {
  LV2_Atom_Forge* F = &S->forge;
  LV2_Atom_Forge_Frame fr;
  if (!lv2_atom_forge_frame_time(F, 0)) return 0;
  if (!lv2_atom_forge_object(F, &fr, 0, S->patch_Set)) return 0;
  lv2_atom_forge_key(F, S->patch_property);
  lv2_atom_forge_urid(F, key);
  lv2_atom_forge_key(F, S->patch_value);
  if (key == S->p_sample) {
    const uint32_t len = (uint32_t)strlen(S->path);
    if (!lv2_atom_forge_path(F, len ? S->path : "_", len ? len : 1)) return 0;
  } else if (!lv2_atom_forge_int(F, ival)) return 0;
  lv2_atom_forge_pop(F, &fr);
  return 1;
}

static void report_path(Sampler* S) {
  /* all or nothing: when the buffer is full, everything is sent again in the next run */
  if (!report_param(S, S->p_sample, 0)) return;
  for (int i = 0; i < N_IPARAMS; i++)
    if (!report_param(S, S->ip_urid[i], S->ip[i])) return;
  S->report = 0;
}

static float ctl(const Sampler* S, int port, float def) {
  const float* p = S->ctl[port];
  return p ? *p : def;
}

static void read_params(Sampler* S) {
  GcParams* p = &S->params;
  p->position = ctl(S, P_POSITION, 0.0f) * 0.01f;
  p->scan     = ctl(S, P_SCAN, 100.0f) * 0.01f;
  p->spray    = ctl(S, P_SPRAY, 0.0f) * 0.01f;
  p->window   = ctl(S, P_WINDOW, 100.0f) * 0.01f;
  p->size_ms  = ctl(S, P_SIZE, 100.0f);
  p->density  = ctl(S, P_DENSITY, 20.0f);
  p->shape    = ctl(S, P_SHAPE, 67.0f) * 0.01f;
  p->chaos    = ctl(S, P_CHAOS, 0.0f) * 0.01f;
  p->attack_ms  = ctl(S, P_ATTACK, 10.0f);
  p->release_ms = ctl(S, P_RELEASE, 500.0f);
  p->spread     = ctl(S, P_SPREAD, 0.0f) * 0.01f;
  p->pitch       = ctl(S, P_PITCH, 0.0f);
  p->pitch_spray = ctl(S, P_PSPRAY, 0.0f);
  p->reverse     = ctl(S, P_REVERSE, 0.0f) * 0.01f;
  p->quantize    = ctl(S, P_QUANTIZE, 0.0f) * 0.01f;
}

static void handle_midi(Sampler* S, const LV2_Atom_Event* ev) {
  const uint8_t* b = (const uint8_t*)LV2_ATOM_BODY_CONST(&ev->body);
  if (ev->body.size < 3) return;
  const uint8_t type = b[0] & 0xF0;
  if (type == 0x90 && b[2] > 0) gc_note_on(&S->core, &S->params, b[1] & 0x7F, b[2]);
  else if (type == 0x80 || type == 0x90) gc_note_off(&S->core, b[1] & 0x7F);
  else if (type == 0xB0 && b[1] == 120) gc_all_off(&S->core);                       /* All Sound Off */
  else if (type == 0xB0 && b[1] == 123) gc_all_notes_off(&S->core);               /* All Notes Off */
}

static void render(Sampler* S, uint32_t from, uint32_t to) {
  if (to > from) gc_process(&S->core, &S->params, S->out_l + from, S->out_r + from, to - from);
}

/* Read positions for the widget: 0..1 of the sample, -1 = none. */
static void write_monitors(Sampler* S) {
  const GrainCore* g = &S->core;
  const int64_t f0 = g->src.frames[0];
  const GcVoice* v = &g->voices[g->last_voice];                  /* the newest note's voice */
  if (S->playhead_out)
    *S->playhead_out = (f0 && v->stage != GC_V_IDLE) ? (float)(v->playhead / (double)f0) : -1.0f;
  int k = 0;
  for (int i = 0; i < GC_POOL && k < N_GRAIN_PORTS; i++) {
    const GcGrain* gr = &g->grains[i];
    if (gr->active && gr->gen == g->gen && gr->frames) {      /* grains of the current sample, on any level */
      if (S->grain_out[k]) *S->grain_out[k] = (float)((double)gr->idx / (double)gr->frames);
      k++;
    }
  }
  for (; k < N_GRAIN_PORTS; k++)
    if (S->grain_out[k]) *S->grain_out[k] = -1.0f;
  for (int i = 0; i < GC_VOICES; i++)
    if (S->note_out[i])
      *S->note_out[i] = g->voices[i].stage != GC_V_IDLE && g->voices[i].stage != GC_V_STEAL ?
                        (float)g->voices[i].note : -1.0f;
}

static void run(LV2_Handle h, uint32_t n) {
  Sampler* S = (Sampler*)h;
  gc_enable_ftz();
  read_params(S);
  const uint32_t cap = S->notify->atom.size;
  lv2_atom_forge_set_buffer(&S->forge, (uint8_t*)S->notify, cap);
  LV2_Atom_Forge_Frame seq;
  lv2_atom_forge_sequence_head(&S->forge, &seq, 0);

  uint32_t done = 0;                                 /* audio rendered up to here: events are sample-accurate */
  LV2_ATOM_SEQUENCE_FOREACH(S->in, ev) {
    uint32_t f = (uint32_t)ev->time.frames;
    if (f > n) f = n;
    if (f > done) { render(S, done, f); done = f; }
    if (ev->body.type == S->midi_Event) handle_midi(S, ev);
    else if (ev->body.type == S->atom_Object) {
      const LV2_Atom_Object* o = (const LV2_Atom_Object*)&ev->body;
      if (o->body.otype == S->patch_Set) handle_patch_set(S, o);
      else if (o->body.otype == S->patch_Get) S->report = 1;
    }
  }
  render(S, done, n);
  if (S->report) report_path(S);
  lv2_atom_forge_pop(&S->forge, &seq);

  /* Level, ramped over the block so turning the knob does not click */
  const float target = ctl(S, P_LEVEL, 0.8f);
  const float start = S->level < 0.0f ? target : S->level;
  const float step = n ? (target - start) / (float)n : 0.0f;
  for (uint32_t i = 0; i < n; i++) {
    const float a = start + step * (float)(i + 1);
    S->out_l[i] *= a;
    S->out_r[i] *= a;
  }
  S->level = target;

  if (S->status_out) *S->status_out = (float)S->status;
  if (S->seconds_out) *S->seconds_out = S->sample ? (float)(S->sample->frames[0] / S->sample->rate) : 0.0f;
  if (S->nretired) free_retired(S);
  write_monitors(S);
}

static void cleanup(LV2_Handle h) {
  Sampler* S = (Sampler*)h;
  gs_sample_free(S->sample);
  for (int i = 0; i < S->nretired; i++) gs_sample_free(S->retired[i].s);
  free(S);
}

/* ---------- state: the sample path goes into presets ---------- */
static const void* get_feature(const LV2_Feature* const* f, const char* uri) {
  for (int i = 0; f && f[i]; i++)
    if (!strcmp(f[i]->URI, uri)) return f[i]->data;
  return NULL;
}

static LV2_State_Status save(LV2_Handle h, LV2_State_Store_Function store, LV2_State_Handle sh,
                             uint32_t flags, const LV2_Feature* const* features) {
  (void)flags;
  Sampler* S = (Sampler*)h;
  for (int i = 0; i < N_IPARAMS; i++) {
    const int32_t v = S->ip[i];
    store(sh, S->ip_urid[i], &v, sizeof v, S->atom_Int, LV2_STATE_IS_POD | LV2_STATE_IS_PORTABLE);
  }
  if (is_empty_path(S->path)) return LV2_STATE_SUCCESS;
  const LV2_State_Map_Path* mp = (const LV2_State_Map_Path*)get_feature(features, LV2_STATE__mapPath);
  const LV2_State_Free_Path* fp = (const LV2_State_Free_Path*)get_feature(features, LV2_STATE__freePath);
  char* apath = mp ? mp->abstract_path(mp->handle, S->path) : NULL;
  const char* p = apath ? apath : S->path;
  store(sh, S->p_sample, p, strlen(p) + 1, S->atom_Path, LV2_STATE_IS_POD | LV2_STATE_IS_PORTABLE);
  if (apath) { if (fp) fp->free_path(fp->handle, apath); else free(apath); }
  return LV2_STATE_SUCCESS;
}

/* Not called concurrently with run(): load the file right here. */
static LV2_State_Status restore(LV2_Handle h, LV2_State_Retrieve_Function retrieve, LV2_State_Handle sh,
                                uint32_t flags, const LV2_Feature* const* features) {
  (void)flags;
  Sampler* S = (Sampler*)h;
  size_t size = 0; uint32_t type = 0, vflags = 0;
  const char* v = (const char*)retrieve(sh, S->p_sample, &size, &type, &vflags);
  char* apath = NULL;
  S->path[0] = 0;
  if (v && size && (type == S->atom_Path || type == S->atom_String)) {
    const LV2_State_Map_Path* mp = (const LV2_State_Map_Path*)get_feature(features, LV2_STATE__mapPath);
    const LV2_State_Free_Path* fp = (const LV2_State_Free_Path*)get_feature(features, LV2_STATE__freePath);
    apath = mp ? mp->absolute_path(mp->handle, v) : NULL;
    const char* p = apath ? apath : v;
    strncpy(S->path, p, GS_PATH_MAX - 1);
    S->path[GS_PATH_MAX - 1] = 0;
    if (apath) { if (fp) fp->free_path(fp->handle, apath); else free(apath); }
  }
  atomic_fetch_add(&S->req_seq, 1);                  /* a load still in the worker is now stale */
  int err = GS_LOAD_OK;
  GsSample* s = is_empty_path(S->path) ? NULL : gs_sample_load(S->path, MAX_SECONDS, &err);
  /* not concurrent with run(): stop the grains, then free the old sample and the retired ones here */
  gc_all_off(&S->core);
  gs_sample_free(S->sample);
  for (int i = 0; i < S->nretired; i++) gs_sample_free(S->retired[i].s);
  S->nretired = 0;
  S->sample = s;
  S->status = s ? ST_LOADED : (err ? ST_ERROR : ST_EMPTY);
  use_sample(S);
  /* integer parameters; missing in old presets: defaults */
  for (int i = 0; i < N_IPARAMS; i++) {
    S->ip[i] = ip_defaults[i];
    const void* iv = retrieve(sh, S->ip_urid[i], &size, &type, &vflags);
    if (iv && type == S->atom_Int && size >= sizeof(int32_t)) S->ip[i] = *(const int32_t*)iv;
  }
  const void* mv = retrieve(sh, S->p_mode_legacy, &size, &type, &vflags);
  if (mv && type == S->atom_Int && size >= sizeof(int32_t) && *(const int32_t*)mv == 1) S->ip[IP_VOICES] = 1;
  apply_iparams(S);
  S->report = 1;                                     /* tell the host the restored values */
  return LV2_STATE_SUCCESS;
}

static const void* extension_data(const char* uri) {
  static const LV2_State_Interface state = { save, restore };
  static const LV2_Worker_Interface worker = { work, work_response, NULL };
  if (!strcmp(uri, LV2_STATE__interface)) return &state;
  if (!strcmp(uri, LV2_WORKER__interface)) return &worker;
  return NULL;
}

static const LV2_Descriptor desc = { GS_URI, instantiate, connect_port, activate,
                                     run, deactivate, cleanup, extension_data };
LV2_SYMBOL_EXPORT const LV2_Descriptor* lv2_descriptor(uint32_t i) { return i == 0 ? &desc : NULL; }
