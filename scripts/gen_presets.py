#!/usr/bin/env python3
"""Generate the factory presets: lv2/presets.ttl and lv2/manifest.ttl (edit this script, not those files).
Each preset sets every control port and every LV2 parameter (missing values would keep the old ones).
Samples are the ones install.sh links into Zynthian's User Samples: an absolute file URI in the state
reaches restore() as an atom:Path (lilv/sratom), unchanged."""
import math
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
                 ("lfo1_rate", 61), ("lfo2_rate", 42), ("env1_time", 1), ("env2_time", 2)]
# integer parameters in the plugin's order (granular_sampler.c), cells m_<src>_<dst> default 0
SRCS = ["lfo1", "lfo2", "env1", "env2", "vel", "wheel", "rnd"]
XDSTS = ["lfo1_rate", "lfo2_rate", "lfo1_amt", "lfo2_amt", "env1_time", "env2_time"]   # X-Mod tab
DSTS = ["position", "scan", "spray", "size", "density", "pitch", "pan", "level", "cutoff"]
INT_DEFAULTS = [("voices", 8), ("scale", 0), ("key", 0), ("lfo1_shape", 0), ("lfo2_shape", 1),
                ("lfo1_sync", 0), ("lfo2_sync", 0), ("env1_shape", 10), ("env2_shape", 50), ("mod_depth", 100),
                ("glide", 0), ("bend_range", 2), ("root", 60), ("tune", 0), ("gain", 0), ("mark_start", 0), ("mark_end", 10000),
                ("lfo1_uni", 0), ("lfo2_uni", 0), ("filter_mix", 100), ("vowel_lows", 25), ("vowel_highs", 50)] + \
               [("m_%s_%s" % (a, b), 0) for a in SRCS for b in DSTS] + \
               [("x_%s_%s" % (a, b), 0) for a in SRCS for b in XDSTS]
PARAM_DEFAULTS = dict([("sample", "kalimba_c.wav")] + INT_DEFAULTS)   # voices 1 mono, 0 legato
SINE, TRI, SAW_UP, SAW_DOWN, SQUARE_LFO, SH, SMOOTH = range(7)
CHROMATIC, MAJOR, MINOR, DORIAN, MAJ_PENT, MIN_PENT = 0, 1, 2, 3, 9, 10
LP, HP, BP = 0, 1, 2
SQUARE, DECAY, BELL, REVERSE = 0, 33.3, 67, 100

# (id, label, ports, parameters): each one shows one side of the engine
# Per sample: what the Wave tab would set. Root and Tune = the detected pitch (Auto), Gain = normalize (peak at
# -1 dBFS, tenths of a dB). Files without a single pitch keep C4: the key C4 plays them as they are.
SAMPLE = {
    "cello_d2.wav":      {"root": 38, "tune": -7,  "gain": 116},
    "kalimba_c.wav":     {"root": 60, "tune": 18,  "gain": 42},
    "kalimba_scale.wav": {"root": 60, "tune": 0,   "gain": -8},
    "singing_bowl.wav":  {"root": 51, "tune": -25, "gain": -10},
    "cymbal_bow.wav":    {"root": 60, "tune": 0,   "gain": -9},
    "breakbeat_140.wav": {"root": 60, "tune": 0,   "gain": -10},
    "spoken_word.wav":   {"root": 60, "tune": 0,   "gain": 13},
    "whisper.wav":       {"root": 60, "tune": 0,   "gain": 35},
}
# breakbeat_140: 4 bars at 140 BPM; a 1/16 note is 107 ms, 9.33 of them per second
SIXTEENTH_MS, SIXTEENTHS = 60000.0 / 140 / 4, 140 * 4 / 60.0
TEMPO = 2
DIVS = ["8 bars", "4 bars", "2 bars", "1 bar", "1/2", "1/4", "1/4T", "1/8", "1/8T", "1/16", "1/16T", "1/32"]  # gc_div_names


def hz(f):
    """LFO Rate knob (0..100) for f Hz in Free/Key: gc_hz_rate"""
    return int(round(100 * math.log(f / 0.01) / math.log(2000)))


def div(name):
    """LFO Rate knob (0..100) for a cycle length in Tempo: gc_rate_div"""
    return int(round(DIVS.index(name) * 100.0 / (len(DIVS) - 1)))


def S(name, boost=0.0, **params):
    """Parameters of a preset on `name`: the sample's own settings, then the preset's.
    boost: dB on top of the normalized gain, to bring quiet presets to the others (test/preset_check)."""
    p = dict(SAMPLE[name], **params)
    p["gain"] = SAMPLE[name]["gain"] + int(round(boost * 10))
    return dict({"sample": name}, **p)


# (id, label, ports, parameters)
PRESETS = [
    ("init", "Init", {}, S("kalimba_c.wav")),

    # cello: bowed, steady; the grains are in phase, so it transposes cleanly
    ("cello_ensemble", "Cello Ensemble",
     {"position": 10, "scan": 50, "window": 80, "size": 300, "density": 30, "spray": 10, "chaos": 30,
      "attack": 400, "release": 2500, "spread": 60, "level": 0.35},
     S("cello_d2.wav", mark_start=500, mark_end=9000, m_lfo1_pan=40)),
    ("cello_frozen", "Cello Frozen Bow",
     {"position": 45, "scan": 0, "window": 10, "size": 500, "density": 25, "chaos": 40, "spray": 15,
      "attack": 800, "release": 4000, "spread": 80, "cutoff": 3000, "resonance": 20, "lfo1_rate": hz(0.1),
      "level": 0.4},
     S("cello_d2.wav", m_lfo1_cutoff=25, m_lfo2_position=10)),
    ("cello_legato", "Cello Mono Legato",
     {"position": 15, "scan": 100, "window": 70, "size": 120, "density": 40, "attack": 40, "release": 500,
      "spread": 20, "level": 0.6},
     S("cello_d2.wav", voices=0, glide=150, m_wheel_cutoff=-40, m_vel_level=40)),

    # kalimba: plucked; decay window, the attack in the grains
    ("kalimba_shimmer", "Kalimba Shimmer",
     {"size": 150, "density": 40, "chaos": 60, "shape": DECAY, "pitch_spray": 12, "quantize": 100,
      "spread": 100, "release": 2500, "level": 0.7},
     S("kalimba_c.wav", boost=4, scale=MAJ_PENT, key=0)),
    ("kalimba_box", "Kalimba Music Box",
     {"scan": 100, "size": 60, "density": 25, "shape": DECAY, "attack": 1, "release": 1500, "spread": 30},
     S("kalimba_c.wav", mark_end=6000)),
    ("kalimba_cloud", "Kalimba Melody Cloud",
     {"scan": 30, "spray": 30, "size": 200, "density": 30, "chaos": 70, "reverse": 30, "spread": 80,
      "attack": 100, "release": 2000},
     S("kalimba_scale.wav", boost=13)),

    # singing bowl: long and inharmonic; drones and slow textures
    ("bowl_drone", "Bowl Drone",
     {"position": 30, "scan": 0, "window": 20, "size": 800, "density": 15, "spray": 20, "chaos": 50,
      "spread": 100, "attack": 2000, "release": 6000, "lfo2_rate": hz(0.05)},
     S("singing_bowl.wav", boost=4, m_lfo2_spray=40)),
    ("bowl_octaves", "Bowl Octaves",
     {"position": 20, "scan": 5, "window": 30, "size": 600, "density": 20, "pitch_spray": 12, "quantize": 100,
      "chaos": 60, "spread": 100, "attack": 1500, "release": 5000, "cutoff": 5000, "level": 0.7},
     S("singing_bowl.wav", boost=7, scale=MIN_PENT, key=3)),

    # bowed cymbal: noise; wash and swell
    ("cymbal_wash", "Cymbal Wash",
     {"scan": 10, "size": 400, "density": 40, "spray": 50, "chaos": 80, "spread": 100, "attack": 600,
      "release": 5000, "cutoff": 300, "resonance": 30, "ftype": HP, "level": 0.6},
     S("cymbal_bow.wav", boost=7, m_lfo1_cutoff=30)),
    ("cymbal_swell", "Cymbal Reverse Swell",
     {"scan": 20, "size": 600, "density": 15, "shape": REVERSE, "reverse": 100, "spread": 70,
      "attack": 1500, "release": 3000, "level": 0.6},
     S("cymbal_bow.wav")),

    # break at 140 BPM: 1/16 grains, square window, tempo-synced LFOs
    ("break_stutter", "Break Stutter",
     {"scan": 100, "window": 25, "size": SIXTEENTH_MS, "density": SIXTEENTHS, "shape": SQUARE, "attack": 1,
      "release": 150},
     S("breakbeat_140.wav", boost=5, voices=1, m_rnd_position=6)),
    ("break_scatter", "Break Slice Scatter",
     {"scan": 100, "spray": 100, "size": SIXTEENTH_MS, "density": SIXTEENTHS, "shape": SQUARE, "reverse": 25,
      "attack": 1, "release": 150, "spread": 40},
     S("breakbeat_140.wav", boost=7, voices=1)),
    ("break_filter", "Break Tempo Filter",
     {"scan": 100, "size": 2 * SIXTEENTH_MS, "density": SIXTEENTHS / 2, "shape": SQUARE, "attack": 1,
      "release": 200, "cutoff": 800, "resonance": 50, "ftype": LP, "lfo1_rate": div("1 bar"), "lfo2_rate": div("1/8")},
     S("breakbeat_140.wav", boost=7, voices=1, lfo1_shape=TRI, lfo1_sync=TEMPO, m_lfo1_cutoff=50,
       lfo2_shape=SQUARE_LFO, lfo2_sync=TEMPO, m_lfo2_level=-30)),

    # voices
    ("voice_choir", "Voice Granular Choir",
     {"scan": 20, "size": 250, "density": 40, "pitch_spray": 7, "quantize": 100, "chaos": 50, "spread": 100,
      "attack": 300, "release": 3000, "cutoff": 4000},
     S("spoken_word.wav", boost=3, scale=MINOR, key=9)),
    ("voice_robot", "Voice Robot",
     {"scan": 100, "size": 30, "density": 60, "shape": SQUARE, "release": 300, "cutoff": 1500,
      "resonance": 50, "ftype": BP},
     S("spoken_word.wav", boost=4)),
    ("whisper_texture", "Whisper Texture",
     {"scan": 5, "spray": 80, "size": 300, "density": 50, "chaos": 100, "reverse": 50, "spread": 100,
      "attack": 1500, "release": 5000, "cutoff": 400, "ftype": HP},
     S("whisper.wav", boost=12)),
    ("whisper_pulse", "Whisper Pulse",
     {"scan": 40, "size": 80, "density": 20, "shape": BELL, "spray": 20, "spread": 60, "release": 800,
      "lfo1_rate": div("1/8")},
     S("whisper.wav", boost=3, lfo1_shape=SQUARE_LFO, lfo1_sync=TEMPO, m_lfo1_level=-70,
       m_env1_size=50)),
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
