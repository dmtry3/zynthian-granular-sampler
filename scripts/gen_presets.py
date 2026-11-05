#!/usr/bin/env python3
"""Generate the factory presets: lv2/presets.ttl and lv2/manifest.ttl (edit this script, not those files).
Each preset sets every control port and every LV2 parameter (missing values would keep the old ones).
Samples are the ones install.sh links into Zynthian's User Samples: an absolute file URI in the state
reaches restore() as an atom:Path (lilv/sratom), unchanged."""
import os

URI = "urn:granularsampler:sampler"
ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..")
SAMPLES = "/zynthian/zynthian-my-data/files/Samples/granular-sampler"

# control ports in index order, with their defaults (granular_sampler.ttl)
PORT_DEFAULTS = [("position", 0), ("scan", 100), ("spray", 0), ("window", 100),
                 ("size", 100), ("density", 20), ("shape", 67), ("chaos", 0),
                 ("pitch", 0), ("pitch_spray", 0), ("reverse", 0), ("quantize", 0),
                 ("attack", 10), ("release", 500), ("spread", 0), ("level", 0.8),
                 ("cutoff", 20000), ("resonance", 0), ("ftype", 0), ("keytrack", 0),
                 ("lfo1_rate", 1), ("lfo2_rate", 0.25), ("env1_time", 1), ("env2_time", 2)]
# integer parameters in the plugin's order (granular_sampler.c), cells m_<src>_<dst> default 0
SRCS = ["lfo1", "lfo2", "env1", "env2", "vel", "wheel", "rnd"]
DSTS = ["position", "scan", "spray", "size", "density", "pitch", "pan", "level", "cutoff"]
INT_DEFAULTS = [("voices", 8), ("scale", 0), ("key", 0), ("lfo1_shape", 0), ("lfo2_shape", 1),
                ("lfo1_sync", 0), ("lfo2_sync", 0), ("env1_shape", 10), ("env2_shape", 50), ("mod_depth", 100),
                ("glide", 0), ("lfo1_div", 4), ("lfo2_div", 4)] + \
               [("m_%s_%s" % (a, b), 0) for a in SRCS for b in DSTS]
PARAM_DEFAULTS = dict([("sample", "test_pad.wav")] + INT_DEFAULTS)   # voices 1 mono, 0 legato
SINE, TRI, SAW_UP, SAW_DOWN, SQUARE_LFO, SH, SMOOTH = range(7)
CHROMATIC, MAJOR, MINOR, DORIAN, MAJ_PENT, MIN_PENT = 0, 1, 2, 3, 9, 10
LP, HP, BP = 0, 1, 2
SQUARE, DECAY, BELL, REVERSE = 0, 33.3, 67, 100

# (id, label, ports, parameters): each one shows one side of the engine
PRESETS = [
    ("init", "Init", {}, {}),
    ("pad_freeze", "Pad Freeze",
     {"scan": 0, "position": 40, "size": 300, "density": 30, "spray": 10, "chaos": 30,
      "attack": 300, "release": 2000, "spread": 60}, {}),
    ("pad_drift", "Pad Slow Drift",
     {"scan": 10, "size": 200, "density": 25, "chaos": 50, "spread": 40, "attack": 150, "release": 1500}, {}),
    ("pad_dense", "Pad Dense",
     {"scan": 30, "size": 1000, "density": 200, "spray": 40, "chaos": 70, "spread": 100,
      "attack": 400, "release": 3000, "level": 0.6}, {}),
    ("pluck_cloud", "Pluck Cloud",
     {"size": 80, "density": 60, "spray": 30, "chaos": 80, "shape": DECAY, "spread": 100, "release": 800},
     {"sample": "test_pluck.wav"}),
    ("pluck_mono", "Pluck Mono Lead",
     {"scan": 50, "size": 120, "density": 40, "attack": 5, "release": 300, "spread": 20},
     {"sample": "test_pluck.wav", "voices": 0, "glide": 120}),
    ("drums_stutter", "Drums Stutter",
     {"window": 25, "size": 60, "density": 16, "shape": SQUARE, "release": 200},
     {"sample": "test_drums.wav"}),
    ("drums_reverse", "Drums Reverse",
     {"scan": -100, "size": 250, "density": 8, "shape": REVERSE, "spread": 50, "release": 400},
     {"sample": "test_drums.wav"}),
    ("drums_freeze", "Drums Freeze Spray",
     {"scan": 0, "position": 25, "window": 50, "spray": 100, "size": 40, "density": 40, "chaos": 100,
      "shape": BELL, "spread": 80, "release": 600},
     {"sample": "test_drums.wav", "voices": 4}),
    ("pad_shimmer", "Pad Shimmer Octaves",
     {"scan": 20, "size": 250, "density": 50, "pitch_spray": 12, "quantize": 100, "chaos": 60, "spread": 100,
      "attack": 500, "release": 3000, "level": 0.7},
     {"scale": MAJ_PENT, "key": 0}),
    ("pluck_scale", "Pluck Spray in D dorian",
     {"size": 90, "density": 45, "pitch_spray": 7, "quantize": 100, "spray": 20, "chaos": 70, "shape": DECAY,
      "spread": 80, "release": 900},
     {"sample": "test_pluck.wav", "scale": DORIAN, "key": 2}),
    ("pad_backwards", "Pad Half Reverse",
     {"scan": 25, "size": 400, "density": 20, "reverse": 50, "chaos": 40, "spread": 60, "attack": 200,
      "release": 2500},
     {}),
    ("drums_pitch_down", "Drums Octave Down",
     {"pitch": -12, "size": 120, "density": 20, "shape": SQUARE, "release": 300},
     {"sample": "test_drums.wav"}),
    ("pad_dark", "Pad Dark Resonant",
     {"scan": 15, "size": 300, "density": 30, "chaos": 40, "spread": 70, "attack": 300, "release": 2500,
      "cutoff": 600, "resonance": 60, "keytrack": 50}, {}),
    ("drums_band", "Drums Band-pass",
     {"window": 50, "size": 80, "density": 24, "shape": SQUARE, "release": 250,
      "cutoff": 1500, "resonance": 40, "ftype": BP, "keytrack": 100},
     {"sample": "test_drums.wav"}),
    ("pad_sweep", "Pad LFO Filter Sweep",
     {"scan": 15, "size": 300, "density": 30, "chaos": 40, "spread": 60, "attack": 300, "release": 2500,
      "cutoff": 900, "resonance": 50, "lfo1_rate": 0.2, "lfo2_rate": 0.13},
     {"m_lfo1_cutoff": 50, "m_lfo2_pan": 80, "lfo2_shape": SMOOTH}),
    ("pluck_random", "Pluck Random Cloud",
     {"size": 70, "density": 50, "chaos": 80, "shape": DECAY, "release": 900, "quantize": 100},
     {"sample": "test_pluck.wav", "scale": MIN_PENT, "key": 9,
      "m_rnd_pitch": 25, "m_rnd_pan": 100, "m_rnd_position": 10, "m_vel_level": 50}),
    ("drums_env", "Drums Env Size and Pitch",
     {"window": 50, "size": 60, "density": 20, "shape": SQUARE, "release": 600, "env1_time": 1.5, "env2_time": 0.8},
     {"sample": "test_drums.wav", "env1_shape": 0, "env2_shape": 0, "m_env1_size": 60, "m_env2_pitch": -25,
      "m_wheel_density": 50}),
    ("pad_tempo", "Pad Tempo Gate",
     {"scan": 20, "size": 200, "density": 40, "chaos": 30, "spread": 50, "attack": 50, "release": 1500,
      "cutoff": 1200, "resonance": 30},
     {"lfo1_shape": SQUARE_LFO, "lfo1_sync": 2, "lfo1_div": 6, "m_lfo1_level": -80,
      "lfo2_shape": SAW_UP, "lfo2_sync": 2, "lfo2_div": 2, "m_lfo2_cutoff": 40}),
]


def preset_ttl(pid, label, ports, params):
    out = ["<%s#preset_%s>" % (URI, pid), "    a pset:Preset ;", "    lv2:appliesTo <%s> ;" % URI,
           '    rdfs:label "%s" ;' % label, "    pset:bank <%s#bank_factory> ;" % URI, "    lv2:port"]
    plines = ['        [ lv2:symbol "%s" ; pset:value %s ]' % (sym, repr(float(ports.get(sym, dflt))))
              for sym, dflt in PORT_DEFAULTS]
    out.append(" ,\n".join(plines) + " ;")
    p = dict(PARAM_DEFAULTS, **params)
    out.append("    state:state [")
    lines = ["        <%s#sample> <file://%s/%s>" % (URI, SAMPLES, p["sample"])]
    lines += ["        <%s#%s> %d" % (URI, sym, int(p[sym])) for sym, _ in INT_DEFAULTS]
    out.append(" ;\n".join(lines))
    out.append("    ] .\n")
    return "\n".join(out)


HEAD = """@prefix lv2:   <http://lv2plug.in/ns/lv2core#> .
@prefix pset:  <http://lv2plug.in/ns/ext/presets#> .
@prefix rdfs:  <http://www.w3.org/2000/01/rdf-schema#> .
@prefix state: <http://lv2plug.in/ns/ext/state#> .
"""

with open(os.path.join(ROOT, "lv2", "presets.ttl"), "w") as f:
    f.write("# Generated by scripts/gen_presets.py - edit the script, not this file.\n" + HEAD + "\n")
    for p in PRESETS:
        f.write(preset_ttl(*p) + "\n")

with open(os.path.join(ROOT, "lv2", "manifest.ttl"), "w") as f:
    f.write("# Generated by scripts/gen_presets.py - edit the script, not this file.\n" + HEAD + "\n")
    f.write("<%s>\n    a lv2:Plugin ;\n    lv2:binary <granular_sampler.so> ;\n"
            "    rdfs:seeAlso <granular_sampler.ttl> .\n\n" % URI)
    f.write('<%s#bank_factory>\n    a pset:Bank ;\n    lv2:appliesTo <%s> ;\n    rdfs:label "Factory" .\n\n'
            % (URI, URI))
    for pid, label, _, _ in PRESETS:
        f.write("<%s#preset_%s>\n    a pset:Preset ;\n    lv2:appliesTo <%s> ;\n    rdfs:label \"%s\" ;\n"
                "    rdfs:seeAlso <presets.ttl> .\n\n" % (URI, pid, URI, label))
print("Wrote lv2/presets.ttl and lv2/manifest.ttl (%d presets)" % len(PRESETS))
