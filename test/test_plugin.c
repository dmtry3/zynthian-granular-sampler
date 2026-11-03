/* A tiny LV2 host for the built plugin: URID map, a worker that runs when the host decides,
 * patch:Set / patch:Get on the control port, State save and restore. No jalv needed.
 * Usage: test_plugin <path to granular_sampler.so> (run from build_test/, after test_sample made st.wav) */
#include <dlfcn.h>
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
static char* uris[256];
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
static float level = 0.8f, out_l[NFR], out_r[NFR], status, seconds;
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
static void in_get(void) {
  LV2_Atom_Forge_Frame f;
  lv2_atom_forge_frame_time(&forge, 0);
  lv2_atom_forge_object(&forge, &f, 0, U(LV2_PATCH__Get));
  lv2_atom_forge_pop(&forge, &f);
}

static const LV2_Descriptor* d;
static void run_once(void) {
  lv2_atom_forge_pop(&forge, &in_frame);
  notify_buf.seq.atom.size = sizeof notify_buf - sizeof(LV2_Atom);
  out_l[0] = out_r[0] = 1.0f;
  d->run(inst, NFR);
  in_begin();
}

/* Path reported on the notify port in the last run(), or NULL. */
static const char* reported_path(void) {
  LV2_ATOM_SEQUENCE_FOREACH(&notify_buf.seq, ev) {
    const LV2_Atom_Object* o = (const LV2_Atom_Object*)&ev->body;
    if (ev->body.type != U(LV2_ATOM__Object) || o->body.otype != U(LV2_PATCH__Set)) continue;
    const LV2_Atom* v = NULL;
    lv2_atom_object_get(o, U(LV2_PATCH__value), &v, 0);
    if (v && v->type == U(LV2_ATOM__Path)) return (const char*)LV2_ATOM_BODY_CONST(v);
  }
  return NULL;
}

/* ---------- state: one stored value is enough for this plugin ---------- */
static char st_val[2048]; static size_t st_size; static uint32_t st_key, st_type;
static LV2_State_Status store(LV2_State_Handle h, uint32_t key, const void* v, size_t size, uint32_t type, uint32_t fl) {
  (void)h; (void)fl;
  st_key = key; st_size = size; st_type = type; memcpy(st_val, v, size);
  return LV2_STATE_SUCCESS;
}
static const void* retrieve(LV2_State_Handle h, uint32_t key, size_t* size, uint32_t* type, uint32_t* fl) {
  (void)h;
  if (key != st_key || !st_size) return NULL;
  *size = st_size; *type = st_type; *fl = 0;
  return st_val;
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
  d->connect_port(inst, 0, &level);
  d->connect_port(inst, 1, &in_buf);
  d->connect_port(inst, 2, out_l);
  d->connect_port(inst, 3, out_r);
  d->connect_port(inst, 4, &notify_buf);
  d->connect_port(inst, 5, &status);
  d->connect_port(inst, 6, &seconds);
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
  in_get(); run_once();
  CHECK(reported_path() && !strcmp(reported_path(), "st.wav"));
  run_once();
  CHECK(reported_path() == NULL);                                 /* answered once */

  /* state: save, clear, restore */
  const LV2_Feature* none[] = { NULL };
  CHECK(si->save(inst, store, NULL, 0, none) == LV2_STATE_SUCCESS);
  CHECK(st_key == U("urn:granularsampler:sampler#sample") && st_type == U(LV2_ATOM__Path) && !strcmp(st_val, "st.wav"));
  in_set_path("_"); run_once(); work_all(); run_once();
  CHECK(status == 0);
  CHECK(si->restore(inst, retrieve, NULL, 0, none) == LV2_STATE_SUCCESS);
  run_once();
  CHECK(status == 2 && seconds > 0.226f && seconds < 0.227f);
  CHECK(reported_path() && !strcmp(reported_path(), "st.wav"));   /* host learns the restored path */

  /* a load still in the worker when a state is restored must not replace the restored sample */
  in_set_path("mono.wav"); run_once();
  CHECK(si->restore(inst, retrieve, NULL, 0, none) == LV2_STATE_SUCCESS);
  work_all(); run_once();
  CHECK(status == 2 && seconds > 0.226f && seconds < 0.227f);

  work_all();
  d->deactivate(inst);
  d->cleanup(inst);
  printf(fails ? "%d FAILED\n" : "All plugin tests passed\n", fails);
  return fails != 0;
}
