/* Granular Sampler: LV2 wrapper.
 * Stage 1a: an empty instrument, takes MIDI and outputs silence. */
#include <lv2/core/lv2.h>
#include <lv2/atom/atom.h>
#include <lv2/atom/util.h>
#include <lv2/midi/midi.h>
#include <lv2/urid/urid.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define GS_URI "urn:granularsampler:sampler"

/* Control input ports come first (0..P_END-1), see granular_sampler.ttl. */
enum { P_LEVEL = 0, P_END, P_MIDI_IN = P_END, P_OUT_L, P_OUT_R };
#define N_CTL P_END

typedef struct {
  const float* ctl[N_CTL];
  const LV2_Atom_Sequence* in;
  float* out_l;
  float* out_r;
  LV2_URID_Map* map;
  LV2_URID midi_Event;
  double sr;
} Sampler;

static LV2_Handle instantiate(const LV2_Descriptor* d, double rate, const char* path,
                              const LV2_Feature* const* f) {
  (void)d; (void)path;
  Sampler* S = (Sampler*)calloc(1, sizeof(Sampler));
  if (!S) return NULL;
  for (int i = 0; f[i]; i++)
    if (!strcmp(f[i]->URI, LV2_URID__map)) S->map = (LV2_URID_Map*)f[i]->data;
  if (!S->map) { free(S); return NULL; }
  S->midi_Event = S->map->map(S->map->handle, LV2_MIDI__MidiEvent);
  S->sr = rate;
  return S;
}

static void connect_port(LV2_Handle h, uint32_t port, void* data) {
  Sampler* S = (Sampler*)h;
  if (port < P_END) S->ctl[port] = (const float*)data;
  else if (port == P_MIDI_IN) S->in = (const LV2_Atom_Sequence*)data;
  else if (port == P_OUT_L) S->out_l = (float*)data;
  else if (port == P_OUT_R) S->out_r = (float*)data;
}

static void activate(LV2_Handle h) { (void)h; }
static void deactivate(LV2_Handle h) { (void)h; }

static void run(LV2_Handle h, uint32_t n) {
  Sampler* S = (Sampler*)h;
  LV2_ATOM_SEQUENCE_FOREACH(S->in, ev) {
    (void)ev;   /* stage 2: notes start voices */
  }
  memset(S->out_l, 0, n * sizeof(float));
  memset(S->out_r, 0, n * sizeof(float));
}

static void cleanup(LV2_Handle h) { free(h); }

static const void* extension_data(const char* uri) { (void)uri; return NULL; }

static const LV2_Descriptor desc = { GS_URI, instantiate, connect_port, activate,
                                     run, deactivate, cleanup, extension_data };
LV2_SYMBOL_EXPORT const LV2_Descriptor* lv2_descriptor(uint32_t i) { return i == 0 ? &desc : NULL; }
