/* Plays every factory preset through the built plugin, the way Zynthian loads one: control ports from the
 * preset, the rest through State restore. Each preset plays a chord (one note for mono presets), with the
 * transport rolling at 140 BPM. Prints peak and loudness, fails on silence, NaN or a missing file, and writes
 * <out dir>/<preset id>.wav to listen to.
 * Usage: preset_check <granular_sampler.so> <presets.ttl> <samples dir> [out dir] */
#include <dlfcn.h>
#include <lv2/atom/atom.h>
#include <lv2/atom/forge.h>
#include <lv2/core/lv2.h>
#include <lv2/state/state.h>
#include <lv2/urid/urid.h>
#include <lv2/worker/worker.h>
#include <math.h>
#include <sndfile.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SR 48000
#define NFR 256
#define NCTL 24
#define URI "urn:granularsampler:sampler"
#define SAMPLES_URI "file:///zynthian/zynthian-my-data/files/Samples/granular-sampler/"

static int fails = 0;

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

/* ---------- worker: runs at once, responses go in before the next run ---------- */
typedef struct { uint32_t size; uint8_t data[2048]; } Msg;
static Msg resps[64];
static int nresp = 0;
static const LV2_Worker_Interface* wi;
static LV2_Handle inst;
static LV2_Worker_Status respond(LV2_Worker_Respond_Handle h, uint32_t size, const void* data) {
  (void)h;
  if (nresp >= 64 || size > sizeof resps[0].data) return LV2_WORKER_ERR_NO_SPACE;
  resps[nresp].size = size; memcpy(resps[nresp].data, data, size); nresp++;
  return LV2_WORKER_SUCCESS;
}
static LV2_Worker_Status schedule(LV2_Worker_Schedule_Handle h, uint32_t size, const void* data) {
  (void)h;
  return wi->work(inst, respond, NULL, size, data);
}
static LV2_Worker_Schedule sched = { NULL, schedule };

/* ---------- a preset parsed from presets.ttl ---------- */
typedef struct {
  char id[64], label[96];
  float ctl[NCTL];
  char ctl_sym[NCTL][256];
  int nctl;
  char key[128][256];
  int ival[128];
  int nint;
  char sample[256];
} Preset;
static Preset presets[64];
static int npresets = 0;

static void parse(const char* path) {
  FILE* f = fopen(path, "r");
  if (!f) { printf("can't read %s\n", path); exit(2); }
  char line[1024];
  Preset* p = NULL;
  while (fgets(line, sizeof line, f)) {
    char a[256], b[256];
    if (sscanf(line, "<" URI "#preset_%63[^>]>", a) == 1) {
      p = &presets[npresets++];
      memset(p, 0, sizeof *p);
      snprintf(p->id, sizeof p->id, "%.63s", a);
    } else if (!p) continue;
    else if (sscanf(line, " rdfs:label \"%95[^\"]\"", a) == 1) snprintf(p->label, sizeof p->label, "%.95s", a);
    else if (sscanf(line, " [ lv2:symbol \"%31[^\"]\" ; pset:value %255[^ ] ]", a, b) == 2 && p->nctl < NCTL) {
      snprintf(p->ctl_sym[p->nctl], 256, "%s", a);
      p->ctl[p->nctl++] = (float)atof(b);
    } else if (sscanf(line, " <" URI "#sample> <%255[^>]>", a) == 1) {
      snprintf(p->sample, sizeof p->sample, "%s", a);
    } else if (sscanf(line, " <" URI "#%47[^>]> %255s", a, b) == 2 && p->nint < 128) {
      snprintf(p->key[p->nint], 256, "%s", a);
      p->ival[p->nint++] = atoi(b);
    }
  }
  fclose(f);
}

/* ---------- state retrieve from the current preset ---------- */
static const Preset* cur;
static char cur_path[512];
static int32_t ivals[128];
static const void* retrieve(LV2_State_Handle h, uint32_t key, size_t* size, uint32_t* type, uint32_t* fl) {
  (void)h;
  *fl = 0;
  if (key == U(URI "#sample")) {
    *size = strlen(cur_path) + 1; *type = U(LV2_ATOM__Path);
    return cur_path;
  }
  for (int i = 0; i < cur->nint; i++) {
    char uri[320];
    snprintf(uri, sizeof uri, URI "#%s", cur->key[i]);
    if (key == U(uri)) { ivals[i] = cur->ival[i]; *size = 4; *type = U(LV2_ATOM__Int); return &ivals[i]; }
  }
  return NULL;
}

/* ---------- ports ---------- */
static float ctls[NCTL], out_l[NFR], out_r[NFR], outs[40];
enum { P_MIDI_IN = NCTL, P_OUT_L, P_OUT_R, P_NOTIFY, P_STATUS, P_SECONDS, P_PLAYHEAD, P_GRAIN0 };
static union { LV2_Atom_Sequence seq; uint8_t raw[8192]; } in_buf, notify_buf;
static LV2_Atom_Forge forge;
static LV2_Atom_Forge_Frame in_frame;
static const LV2_Descriptor* d;
static float status;

static void in_begin(void) {
  lv2_atom_forge_set_buffer(&forge, in_buf.raw, sizeof in_buf);
  lv2_atom_forge_sequence_head(&forge, &in_frame, 0);
}
static void in_midi(uint8_t st, uint8_t d1, uint8_t d2) {
  const uint8_t msg[3] = { st, d1, d2 };
  lv2_atom_forge_frame_time(&forge, 0);
  lv2_atom_forge_atom(&forge, 3, U("http://lv2plug.in/ns/ext/midi#MidiEvent"));
  lv2_atom_forge_write(&forge, msg, 3);
}
static void in_position(float bpm, long bar, float beat) {
  LV2_Atom_Forge_Frame f;
  lv2_atom_forge_frame_time(&forge, 0);
  lv2_atom_forge_object(&forge, &f, 0, U("http://lv2plug.in/ns/ext/time#Position"));
  lv2_atom_forge_key(&forge, U("http://lv2plug.in/ns/ext/time#beatsPerMinute"));
  lv2_atom_forge_float(&forge, bpm);
  lv2_atom_forge_key(&forge, U("http://lv2plug.in/ns/ext/time#speed"));
  lv2_atom_forge_float(&forge, 1.0f);
  lv2_atom_forge_key(&forge, U("http://lv2plug.in/ns/ext/time#bar"));
  lv2_atom_forge_long(&forge, bar);
  lv2_atom_forge_key(&forge, U("http://lv2plug.in/ns/ext/time#barBeat"));
  lv2_atom_forge_float(&forge, beat);
  lv2_atom_forge_pop(&forge, &f);
}
static void run_once(void) {
  lv2_atom_forge_pop(&forge, &in_frame);
  notify_buf.seq.atom.size = sizeof notify_buf - sizeof(LV2_Atom);
  d->run(inst, NFR);
  for (int i = 0; i < nresp; i++) wi->work_response(inst, resps[i].size, resps[i].data);
  nresp = 0;
  in_begin();
}

int main(int argc, char** argv) {
  if (argc < 4) { printf("usage: preset_check plugin.so presets.ttl samples_dir [out_dir]\n"); return 2; }
  const char* outdir = argc > 4 ? argv[4] : NULL;
  void* lib = dlopen(argv[1], RTLD_NOW);
  if (!lib) { printf("dlopen: %s\n", dlerror()); return 2; }
  d = ((LV2_Descriptor_Function)dlsym(lib, "lv2_descriptor"))(0);
  parse(argv[2]);
  if (!npresets) { printf("no presets in %s\n", argv[2]); return 2; }
  LV2_Feature fmap = { LV2_URID__map, &map }, fsched = { LV2_WORKER__schedule, &sched };
  const LV2_Feature* feats[] = { &fmap, &fsched, NULL };
  const LV2_Feature* none[] = { NULL };
  lv2_atom_forge_init(&forge, &map);
  printf("%-26s %-18s %7s %8s  %s\n", "preset", "sample", "peak", "loudness", "");
  for (int k = 0; k < npresets; k++) {
    cur = &presets[k];
    inst = d->instantiate(d, SR, ".", feats);
    wi = (const LV2_Worker_Interface*)d->extension_data(LV2_WORKER__interface);
    const LV2_State_Interface* si = (const LV2_State_Interface*)d->extension_data(LV2_STATE__interface);
    static const char* const order[NCTL] = { "position", "scan", "spray", "window", "size", "density", "shape",
      "chaos", "pitch", "pitch_spray", "reverse", "quantize", "attack", "release", "spread", "level", "cutoff",
      "resonance", "ftype", "keytrack", "lfo1_rate", "lfo2_rate", "env1_time", "env2_time" };
    int missing = 0;
    for (int i = 0; i < NCTL; i++) {
      int found = 0;
      for (int j = 0; j < cur->nctl; j++)
        if (!strcmp(cur->ctl_sym[j], order[i])) { ctls[i] = cur->ctl[j]; found = 1; }
      missing += !found;
      d->connect_port(inst, i, &ctls[i]);
    }
    d->connect_port(inst, P_MIDI_IN, &in_buf);
    d->connect_port(inst, P_OUT_L, out_l);
    d->connect_port(inst, P_OUT_R, out_r);
    d->connect_port(inst, P_NOTIFY, &notify_buf);
    d->connect_port(inst, P_STATUS, &status);
    for (int i = P_SECONDS; i < P_GRAIN0 + 26; i++) if (i != P_STATUS) d->connect_port(inst, i, &outs[i - P_SECONDS]);
    d->activate(inst);
    const char* name = strncmp(cur->sample, SAMPLES_URI, strlen(SAMPLES_URI)) ? cur->sample
                                                                               : cur->sample + strlen(SAMPLES_URI);
    snprintf(cur_path, sizeof cur_path, "%s/%s", argv[3], name);
    si->restore(inst, retrieve, NULL, 0, none);
    in_begin();
    run_once();
    int mono = 1;
    for (int i = 0; i < cur->nint; i++) if (!strcmp(cur->key[i], "voices")) mono = cur->ival[i] <= 1;
    /* chord around C4 (the root plays the file as it is), or one note */
    const int chord[3] = { 55, 60, 64 };
    const int nn = mono ? 1 : 3;
    for (int i = 0; i < nn; i++) in_midi(0x90, mono ? 60 : chord[i], 100);
    const int hold = SR * 4 / NFR, tail = SR * 3 / NFR, nb = hold + tail;
    float* wav = outdir ? malloc(sizeof(float) * 2 * NFR * nb) : NULL;
    double e = 0; long ne = 0; float peak = 0; long bad = 0;
    for (int b = 0; b < nb; b++) {
      if (b % (SR / NFR / 4) == 0) {                   /* transport: 140 BPM, rolling */
        const double beats = (double)b * NFR / SR * 140.0 / 60.0;
        in_position(140.0f, (long)(beats / 4), (float)fmod(beats, 4.0));
      }
      if (b == hold) for (int i = 0; i < nn; i++) in_midi(0x80, mono ? 60 : chord[i], 0);
      run_once();
      for (int i = 0; i < NFR; i++) {
        if (!isfinite(out_l[i]) || !isfinite(out_r[i])) { bad++; out_l[i] = out_r[i] = 0; }
        peak = fmaxf(peak, fmaxf(fabsf(out_l[i]), fabsf(out_r[i])));
        if (b > SR / 2 / NFR && b < hold) { e += 0.5 * ((double)out_l[i] * out_l[i] + (double)out_r[i] * out_r[i]); ne++; }
        if (wav) { wav[2 * (b * NFR + i)] = out_l[i]; wav[2 * (b * NFR + i) + 1] = out_r[i]; }
      }
    }
    const double loud = 10 * log10(e / (ne ? ne : 1) + 1e-20);
    const char* problem = status != 2 ? "SAMPLE NOT LOADED" : missing ? "PORTS MISSING" : bad ? "NaN" :
                          loud < -40 ? "SILENT" : peak > 2.0f ? "LOUD (> +6 dB)" : "";
    if (status != 2 || missing || bad || loud < -40) fails++;
    printf("%-26s %-18s %+6.1f %+7.1f   %s\n", cur->label, name, 20 * log10f(peak + 1e-9f), loud, problem);
    if (wav) {
      char out[512];
      snprintf(out, sizeof out, "%s/%s.wav", outdir, cur->id);
      SF_INFO fi = { 0 };
      fi.samplerate = SR; fi.channels = 2; fi.format = SF_FORMAT_WAV | SF_FORMAT_PCM_16;
      SNDFILE* sf = sf_open(out, SFM_WRITE, &fi);
      if (sf) { sf_command(sf, SFC_SET_CLIPPING, NULL, SF_TRUE); sf_writef_float(sf, wav, NFR * nb); sf_close(sf); }
      free(wav);
    }
    d->deactivate(inst);
    d->cleanup(inst);
  }
  printf(fails ? "%d presets FAILED\n" : "All %d presets play\n", fails ? fails : npresets);
  return fails != 0;
}
