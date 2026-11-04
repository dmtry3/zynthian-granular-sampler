/* A tiny LV2 host for the built plugin: URID map, a worker that runs when the host decides,
 * patch:Set / patch:Get on the control port, State save and restore. No jalv needed.
 * Usage: test_plugin <path to granular_sampler.so> (run from build_test/, after test_sample made st.wav) */
#include <dlfcn.h>
#include <math.h>
#include <lv2/atom/atom.h>
#include <lv2/atom/forge.h>
#include <lv2/atom/util.h>
#include <lv2/core/lv2.h>
#include <lv2/patch/patch.h>
#include <lv2/state/state.h>
#include <lv2/urid/urid.h>
#include <lv2/worker/worker.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fails = 0;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

/* ---------- URID map ---------- */
static char* uris[512];
static int nuris = 0;
static LV2_URID map_uri(LV2_URID_Map_Handle h, const char* uri) {
  (void)h;
  for (int i = 0; i < nuris; i++) if (!strcmp(uris[i], uri)) return (LV2_URID)(i + 1);
  uris[nuris] = strdup(uri);
  return (LV2_URID)++nuris;
}
static LV2_URID_Map map = { NULL, map_uri };
#define U(s) map_uri(NULL, s)

/* ---------- worker: requests queue up and run in work_all(), like a separate thread would ---------- */
typedef struct { uint32_t size; uint8_t data[2048]; } Msg;
static Msg reqs[64], resps[64];
static int nreq = 0, nresp = 0;
static const LV2_Worker_Interface* wi;
static LV2_Handle inst;

static LV2_Worker_Status schedule(LV2_Worker_Schedule_Handle h, uint32_t size, const void* data) {
  (void)h;
  if (nreq >= 64 || size > sizeof reqs[0].data) return LV2_WORKER_ERR_NO_SPACE;
  reqs[nreq].size = size; memcpy(reqs[nreq].data, data, size); nreq++;
  return LV2_WORKER_SUCCESS;
}
static LV2_Worker_Status respond(LV2_Worker_Respond_Handle h, uint32_t size, const void* data) {
  (void)h;
  if (nresp >= 64 || size > sizeof resps[0].data) return LV2_WORKER_ERR_NO_SPACE;
  resps[nresp].size = size; memcpy(resps[nresp].data, data, size); nresp++;
  return LV2_WORKER_SUCCESS;
}
static LV2_Worker_Schedule sched = { NULL, schedule };
static void work_all(void) {
  while (nreq) {   /* responses may schedule more work (freeing the old sample) */
    int n = nreq; Msg q[64]; memcpy(q, reqs, sizeof(Msg) * n); nreq = 0;
    for (int i = 0; i < n; i++) wi->work(inst, respond, NULL, q[i].size, q[i].data);
    for (int i = 0; i < nresp; i++) wi->work_response(inst, resps[i].size, resps[i].data);
    nresp = 0;
  }
}

/* ---------- ports ---------- */
#define NFR 64
/* control inputs: position scan spray window | size density shape chaos | pitch pitch_spray reverse quantize |
 * attack release spread level | cutoff resonance ftype keytrack | lfo1 lfo2 rates, env1 env2 times */
#define NCTL 24
enum { C_SHAPE = 6, C_PITCH = 8, C_ATTACK = 12, C_RELEASE = 13, C_LEVEL = 15, C_CUTOFF = 16, C_FTYPE = 18 };
static float ctls[NCTL] = { 0, 100, 0, 100, 100, 20, 67, 0, 0, 0, 0, 0, 10, 50, 0, 0.8f, 20000, 0, 0, 0,
                            1, 0.25f, 1, 2 };
static float out_l[NFR], out_r[NFR], status, seconds, playhead, grains[16], notes[8];
enum { P_MIDI_IN = NCTL, P_OUT_L, P_OUT_R, P_NOTIFY, P_STATUS, P_SECONDS, P_PLAYHEAD, P_GRAIN0 };
static union { LV2_Atom_Sequence seq; uint8_t raw[8192]; } in_buf, notify_buf;
static LV2_Atom_Forge forge;
static LV2_Atom_Forge_Frame in_frame;

static void in_begin(void) {
  lv2_atom_forge_set_buffer(&forge, in_buf.raw, sizeof in_buf);
  lv2_atom_forge_sequence_head(&forge, &in_frame, 0);
}
static void in_set_path(const char* path) {
  LV2_Atom_Forge_Frame f;
  lv2_atom_forge_frame_time(&forge, 0);
  lv2_atom_forge_object(&forge, &f, 0, U(LV2_PATCH__Set));
  lv2_atom_forge_key(&forge, U(LV2_PATCH__property));
  lv2_atom_forge_urid(&forge, U("urn:granularsampler:sampler#sample"));
  lv2_atom_forge_key(&forge, U(LV2_PATCH__value));
  lv2_atom_forge_path(&forge, path, (uint32_t)strlen(path));
  lv2_atom_forge_pop(&forge, &f);
}
static void in_midi(uint32_t frame, uint8_t st, uint8_t d1, uint8_t d2) {
  const uint8_t msg[3] = { st, d1, d2 };
  lv2_atom_forge_frame_time(&forge, frame);
  lv2_atom_forge_atom(&forge, 3, U("http://lv2plug.in/ns/ext/midi#MidiEvent"));
  lv2_atom_forge_write(&forge, msg, 3);
}
static void in_get(void) {
  LV2_Atom_Forge_Frame f;
  lv2_atom_forge_frame_time(&forge, 0);
  lv2_atom_forge_object(&forge, &f, 0, U(LV2_PATCH__Get));
  lv2_atom_forge_pop(&forge, &f);
}

static const LV2_Descriptor* d;
static void gather_reports(void);
static void run_once(void) {
  lv2_atom_forge_pop(&forge, &in_frame);
  notify_buf.seq.atom.size = sizeof notify_buf - sizeof(LV2_Atom);
  out_l[0] = out_r[0] = 1.0f;
  d->run(inst, NFR);
  gather_reports();
  in_begin();
}

/* patch:Set replies on the notify port, gathered over runs (the plugin sends a few per run) */
typedef struct { uint32_t key, type; int ival; char path[256]; } Rep;
static Rep reps[160];
static int nreps = 0, last_reports = 0;
static void gather_reports(void) {
  last_reports = 0;
  LV2_ATOM_SEQUENCE_FOREACH(&notify_buf.seq, ev) {
    const LV2_Atom_Object* o = (const LV2_Atom_Object*)&ev->body;
    if (ev->body.type != U(LV2_ATOM__Object) || o->body.otype != U(LV2_PATCH__Set)) continue;
    const LV2_Atom *k = NULL, *v = NULL;
    lv2_atom_object_get(o, U(LV2_PATCH__property), &k, U(LV2_PATCH__value), &v, 0);
    if (!k || !v) continue;
    last_reports++;
    const uint32_t key = ((const LV2_Atom_URID*)k)->body;
    Rep* r = NULL;
    for (int i = 0; i < nreps; i++) if (reps[i].key == key) r = &reps[i];
    if (!r && nreps < 160) r = &reps[nreps++];
    if (!r) continue;
    r->key = key; r->type = v->type;
    if (v->type == U(LV2_ATOM__Int)) r->ival = ((const LV2_Atom_Int*)v)->body;
    if (v->type == U(LV2_ATOM__Path)) snprintf(r->path, sizeof r->path, "%s", (const char*)LV2_ATOM_BODY_CONST(v));
  }
}
static const Rep* reported(const char* prop) {
  for (int i = 0; i < nreps; i++) if (reps[i].key == U(prop)) return &reps[i];
  return NULL;
}
static const char* reported_path(void) {
  const Rep* r = reported("urn:granularsampler:sampler#sample");
  return r && r->type == U(LV2_ATOM__Path) ? r->path : NULL;
}
static int reported_int(const char* prop, int def) {
  const Rep* r = reported(prop);
  return r && r->type == U(LV2_ATOM__Int) ? r->ival : def;
}
static void run_once(void);
/* a fresh collection of the replies over enough runs for all of them */
static void collect(void) {
  nreps = 0;
  int total = 0;
  for (int k = 0; k < 10; k++) { run_once(); total += last_reports; }
  last_reports = total;
}
#define P_MODE "urn:granularsampler:sampler#mode"
#define P_VOICES "urn:granularsampler:sampler#voices"
#define P_SCALE "urn:granularsampler:sampler#scale"
#define P_KEY "urn:granularsampler:sampler#key"
/* patch:Set of a number, as Zynthian sends it (a float) */
static void in_set_float(const char* prop, float val) {
  LV2_Atom_Forge_Frame f;
  lv2_atom_forge_frame_time(&forge, 0);
  lv2_atom_forge_object(&forge, &f, 0, U(LV2_PATCH__Set));
  lv2_atom_forge_key(&forge, U(LV2_PATCH__property));
  lv2_atom_forge_urid(&forge, U(prop));
  lv2_atom_forge_key(&forge, U(LV2_PATCH__value));
  lv2_atom_forge_float(&forge, val);
  lv2_atom_forge_pop(&forge, &f);
}

/* ---------- state: a few keys ---------- */
typedef struct { uint32_t key, type; size_t size; char val[2048]; } StVal;
static StVal st[128];
static int nst = 0;
static StVal* st_find(uint32_t key) {
  for (int i = 0; i < nst; i++) if (st[i].key == key) return &st[i];
  return NULL;
}
static LV2_State_Status store(LV2_State_Handle h, uint32_t key, const void* v, size_t size, uint32_t type, uint32_t fl) {
  (void)h; (void)fl;
  StVal* e = st_find(key);
  if (!e) e = &st[nst++];
  e->key = key; e->size = size; e->type = type; memcpy(e->val, v, size);
  return LV2_STATE_SUCCESS;
}
static const void* retrieve(LV2_State_Handle h, uint32_t key, size_t* size, uint32_t* type, uint32_t* fl) {
  (void)h;
  const StVal* e = st_find(key);
  if (!e) return NULL;
  *size = e->size; *type = e->type; *fl = 0;
  return e->val;
}

int main(int argc, char** argv) {
  if (argc < 2) { printf("usage: test_plugin granular_sampler.so\n"); return 2; }
  void* lib = dlopen(argv[1], RTLD_NOW);
  if (!lib) { printf("dlopen: %s\n", dlerror()); return 2; }
  LV2_Descriptor_Function df = (LV2_Descriptor_Function)dlsym(lib, "lv2_descriptor");
  d = df(0);
  CHECK(d && !strcmp(d->URI, "urn:granularsampler:sampler") && !df(1));

  LV2_Feature fmap = { LV2_URID__map, &map }, fsched = { LV2_WORKER__schedule, &sched };
  const LV2_Feature* no_worker[] = { &fmap, NULL };
  CHECK(d->instantiate(d, 48000, ".", no_worker) == NULL);   /* worker is required */
  const LV2_Feature* feats[] = { &fmap, &fsched, NULL };
  inst = d->instantiate(d, 48000, ".", feats);
  CHECK(inst != NULL);
  if (!inst) return 1;
  wi = (const LV2_Worker_Interface*)d->extension_data(LV2_WORKER__interface);
  const LV2_State_Interface* si = (const LV2_State_Interface*)d->extension_data(LV2_STATE__interface);
  CHECK(wi && si);
  for (int i = 0; i < NCTL; i++) d->connect_port(inst, i, &ctls[i]);
  d->connect_port(inst, P_MIDI_IN, &in_buf);
  d->connect_port(inst, P_OUT_L, out_l);
  d->connect_port(inst, P_OUT_R, out_r);
  d->connect_port(inst, P_NOTIFY, &notify_buf);
  d->connect_port(inst, P_STATUS, &status);
  d->connect_port(inst, P_SECONDS, &seconds);
  d->connect_port(inst, P_PLAYHEAD, &playhead);
  for (int i = 0; i < 16; i++) d->connect_port(inst, P_GRAIN0 + i, &grains[i]);
  for (int i = 0; i < 8; i++) d->connect_port(inst, P_GRAIN0 + 16 + i, &notes[i]);
  lv2_atom_forge_init(&forge, &map);
  d->activate(inst);

  in_begin(); run_once();
  CHECK(status == 0 && seconds == 0 && out_l[0] == 0 && out_r[0] == 0);   /* empty, silent */

  /* load: "loading" until the worker answers */
  in_set_path("st.wav"); run_once();
  CHECK(status == 1);
  work_all(); run_once();
  CHECK(status == 2 && seconds > 0.226f && seconds < 0.227f);   /* 10000 frames at 44.1 kHz */

  /* the file selector scrolls fast: only the last of several requests is loaded */
  in_set_path("missing.wav"); in_set_path("mono.wav"); run_once();
  CHECK(nreq == 2);
  work_all(); run_once();
  CHECK(status == 2 && seconds > 0.104f && seconds < 0.105f);   /* mono.wav: 5000 frames at 48 kHz */

  /* error, then clear with "_" */
  in_set_path("missing.wav"); run_once(); work_all(); run_once();
  CHECK(status == 3 && seconds == 0);
  in_set_path("_"); run_once(); work_all(); run_once();
  CHECK(status == 0);

  /* patch:Get: the plugin answers with the current path */
  in_set_path("st.wav"); run_once(); work_all(); run_once();
  in_get(); collect();
  CHECK(reported_path() && !strcmp(reported_path(), "st.wav"));
  CHECK(reported(P_MODE) == NULL && reported_int(P_VOICES, -1) == 8);       /* default 8 voices, no mode */
  CHECK(reported_int(P_SCALE, -1) == 0 && reported_int(P_KEY, -1) == 0);     /* chromatic, C */
  CHECK(last_reports == 67);                                      /* the path and the 66 integer parameters */
  run_once();
  CHECK(last_reports == 0);                                       /* answered once */

  /* voices arrive as floats from Zynthian; out-of-range values are clamped */
  in_set_float(P_VOICES, 4.0f); run_once();
  in_get(); collect();
  CHECK(reported_int(P_VOICES, -1) == 4);
  in_set_float(P_VOICES, 99.0f); run_once();
  in_get(); collect();
  CHECK(reported_int(P_VOICES, -1) == 8);
  in_set_float(P_VOICES, 0.0f); run_once();                       /* 0 = one voice with legato (L) */
  in_get(); collect();
  CHECK(reported_int(P_VOICES, -1) == 0);
  in_set_float(P_VOICES, -3.0f); run_once();
  in_get(); collect();
  CHECK(reported_int(P_VOICES, -1) == 0);
  in_set_float(P_SCALE, 2.0f); in_set_float(P_KEY, 14.0f); run_once();     /* minor; key clamped to B */
  in_get(); collect();
  CHECK(reported_int(P_SCALE, -1) == 2 && reported_int(P_KEY, -1) == 11);
  in_set_float(P_KEY, 9.0f);
  /* modulation parameters: a grid cell, an LFO shape, Mod Depth; clamped, reported, saved */
  in_set_float("urn:granularsampler:sampler#m_lfo1_pitch", 250.0f);
  in_set_float("urn:granularsampler:sampler#m_env2_cutoff", -40.0f);
  in_set_float("urn:granularsampler:sampler#lfo2_shape", 5.0f);
  in_set_float("urn:granularsampler:sampler#mod_depth", 60.0f);
  in_get(); collect();
  CHECK(reported_int("urn:granularsampler:sampler#m_lfo1_pitch", -1) == 100);
  CHECK(reported_int("urn:granularsampler:sampler#m_env2_cutoff", 0) == -40);
  CHECK(reported_int("urn:granularsampler:sampler#m_rnd_cutoff", -1) == 0);   /* the last cell */
  CHECK(reported_int("urn:granularsampler:sampler#lfo2_shape", -1) == 5);
  CHECK(reported_int("urn:granularsampler:sampler#mod_depth", -1) == 60);
  CHECK(reported_int("urn:granularsampler:sampler#env2_shape", -1) == 50);    /* default */
  in_set_float(P_VOICES, 3.0f); run_once();

  /* state: save, clear, restore */
  const LV2_Feature* none[] = { NULL };
  CHECK(si->save(inst, store, NULL, 0, none) == LV2_STATE_SUCCESS);
  const StVal* sv = st_find(U("urn:granularsampler:sampler#sample"));
  CHECK(sv && sv->type == U(LV2_ATOM__Path) && !strcmp(sv->val, "st.wav"));
  CHECK(st_find(U(P_MODE)) == NULL);                              /* mode is gone */
  CHECK(st_find(U(P_VOICES)) && *(const int32_t*)st_find(U(P_VOICES))->val == 3);
  CHECK(st_find(U(P_SCALE)) && *(const int32_t*)st_find(U(P_SCALE))->val == 2);
  CHECK(st_find(U(P_KEY)) && *(const int32_t*)st_find(U(P_KEY))->val == 9);
  CHECK(*(const int32_t*)st_find(U("urn:granularsampler:sampler#m_env2_cutoff"))->val == -40);
  CHECK(nst == 67);                                               /* sample + 66 integer parameters */
  in_set_float(P_SCALE, 0.0f); in_set_float(P_KEY, 0.0f);
  in_set_float(P_VOICES, 8.0f); run_once();
  in_set_path("_"); run_once(); work_all(); run_once();
  CHECK(status == 0);
  CHECK(si->restore(inst, retrieve, NULL, 0, none) == LV2_STATE_SUCCESS);
  collect();
  CHECK(status == 2 && seconds > 0.226f && seconds < 0.227f);
  CHECK(reported_path() && !strcmp(reported_path(), "st.wav"));   /* host learns the restored values */
  CHECK(reported_int(P_VOICES, -1) == 3);
  CHECK(reported_int(P_SCALE, -1) == 2 && reported_int(P_KEY, -1) == 9);
  /* a state of an older version with mode = Mono comes back as one voice with legato */
  {
    const int32_t mono = 1;
    store(NULL, U(P_MODE), &mono, sizeof mono, U(LV2_ATOM__Int), 0);
    CHECK(si->restore(inst, retrieve, NULL, 0, none) == LV2_STATE_SUCCESS);
    collect();
    CHECK(reported_int(P_VOICES, -1) == 0);                       /* old Mono was legato */
    nst--;                                                        /* drop it again */
  }
  in_set_float(P_VOICES, 8.0f); run_once();

  /* a load still in the worker when a state is restored must not replace the restored sample */
  in_set_path("mono.wav"); run_once();
  CHECK(si->restore(inst, retrieve, NULL, 0, none) == LV2_STATE_SUCCESS);
  work_all(); run_once();
  CHECK(status == 2 && seconds > 0.226f && seconds < 0.227f);

  /* ---------- sound (stage 2) ---------- */
  CHECK(status == 2);                                             /* st.wav is loaded */
  in_midi(0, 0x90, 60, 100); run_once();
  float peak = 0;
  for (int k = 0; k < 20; k++) {
    for (int i = 0; i < NFR; i++) if (fabsf(out_l[i]) > peak) peak = fabsf(out_l[i]);
    run_once();
  }
  CHECK(peak > 0.0f);                                             /* a note plays the sample */
  int seen = 0;
  for (int i = 0; i < 8; i++) if (notes[i] == 60.0f) seen++;
  CHECK(seen == 1);                                               /* the sounding note on a note output */
  CHECK(playhead >= 0.0f && playhead <= 1.0f);
  CHECK(grains[0] >= 0.0f && grains[0] <= 1.0f && grains[15] == -1.0f);   /* few grains: the rest is -1 */
  ctls[C_LEVEL] = 0.0f; run_once(); run_once();
  float mx = 0;
  for (int i = 0; i < NFR; i++) mx = fmaxf(mx, fabsf(out_l[i]) + fabsf(out_r[i]));
  CHECK(mx == 0.0f);                                              /* Level 0 */
  ctls[C_LEVEL] = 0.8f;
  /* filter: a high-pass at 20 kHz takes almost everything away from the slow ramp of st.wav */
  in_midi(0, 0x90, 60, 100);
  for (int k = 0; k < 20; k++) run_once();
  float open_peak = 0;
  for (int k = 0; k < 10; k++) { run_once(); for (int i = 0; i < NFR; i++) open_peak = fmaxf(open_peak, fabsf(out_l[i])); }
  ctls[C_FTYPE] = 1; ctls[C_CUTOFF] = 20000;
  for (int k = 0; k < 20; k++) run_once();                        /* the cutoff glides there */
  float hp_peak = 0;
  for (int k = 0; k < 10; k++) { run_once(); for (int i = 0; i < NFR; i++) hp_peak = fmaxf(hp_peak, fabsf(out_l[i])); }
  CHECK(open_peak > 0.005f && hp_peak < open_peak * 0.01f);     /* st.wav is a slow ramp: -40 dB at least */
  ctls[C_FTYPE] = 0;
  in_midi(0, 0xB0, 120, 0); run_once();
  in_midi(0, 0xB0, 120, 0); run_once();                           /* All Sound Off */
  mx = 0;
  for (int i = 0; i < NFR; i++) mx = fmaxf(mx, fabsf(out_l[i]));
  CHECK(mx == 0.0f && grains[0] == -1.0f);
  run_once();
  CHECK(playhead == -1.0f);                                       /* voice idle */
  /* sample-accurate note: nothing before frame 40 of the block */
  ctls[C_SHAPE] = 0;                                                    /* square window: sound at once */
  in_midi(40, 0x90, 60, 127); run_once();
  mx = 0;
  for (int i = 0; i < 40; i++) mx = fmaxf(mx, fabsf(out_l[i]));
  float after = 0;
  for (int i = 41; i < NFR; i++) after = fmaxf(after, fabsf(out_l[i]));
  CHECK(mx == 0.0f && after > 0.0f);
  in_midi(0, 0x80, 60, 0);
  for (int k = 0; k < 100; k++) run_once();                       /* 100 ms grains finish */
  CHECK(playhead == -1.0f && grains[0] == -1.0f);
  /* a new sample while playing: no stale pointers (ASan) */
  in_midi(0, 0x90, 60, 127); run_once();
  in_set_path("mono.wav"); run_once(); work_all();
  for (int k = 0; k < 10; k++) run_once();
  CHECK(status == 2);

  work_all();
  d->deactivate(inst);
  d->cleanup(inst);
  printf(fails ? "%d FAILED\n" : "All plugin tests passed\n", fails);
  return fails != 0;
}
