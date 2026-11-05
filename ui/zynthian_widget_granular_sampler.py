#!/usr/bin/python3
# -*- coding: utf-8 -*-
# Granular Sampler widget. Tabs (top left): Wave, Grain, Pitch, Filter, Mod.
# Wave:
# - The sample path is the "sample" LV2 parameter (atom:Path, notOnGUI): a tap opens Zynthian's file selector.
# - The waveform is read from the file here, in a background thread, and drawn once into an image:
#   marks move over a plain image, which is cheap to redraw (a big filled polygon was not, on the Pi).
# - Zynthian calls update() every 32 ms; monitors and marks are refreshed only every UPDATE_PERIOD.
# - The plugin's own state comes from its output ports: status (0 empty, 1 loading, 2 loaded, 3 error), seconds.
# - Window zone (from the Position and Window knobs), playhead and grain read positions (output ports
#   playhead, grain1..16: 0..1 of the sample, -1 = none). The marks are moved, not recreated, and only
#   when they changed.
# Grain:
# - The window shape of the Shape knob (same formula as grain_core.c, after Mutable Instruments Beads),
#   Size, Density and the mean overlap; the number of voices (LV2 parameter voices 0..8): 1 = mono,
#   a second tap on 1 makes it L = mono with legato (voices 0); Glide (mono portamento, ms): finger up/down.
# Pitch:
# - One octave: notes of the Quantize scale lit, the tonic filled, sounding notes (outputs note1..8) dotted;
#   a tap on a key sets the tonic (LV2 parameter key); scale buttons set the scale (LV2 parameter scale).
# Filter:
# - The response of the voice filter for the knobs (Cutoff, Resonance, Type, Key Track at C4): the TPT SVF of
#   grain_core.c is the bilinear transform of the analog prototype, so |H| is the analog formula at
#   w = tan(pi f / sr) / tan(pi fc / sr).
# Mod:
# - The 7 x 9 grid (LV2 parameters m_<source>_<destination>, -100..100): a finger up/down changes a cell,
#   a double tap resets it; colour = sign, brightness = depth; sources in use are lit.
# - LFO 1/2: a tap changes the shape; the small button in its corner: +- (LFO -1..1) or + (0..1); the sync button: a tap = Free / Key / Tempo. It shows what the LFO Rate
#   knob (0..100) means in that mode: Hz in Free/Key, a cycle length (8 bars .. 1/32) in Tempo.
#   Env 1/2 shape and Mod Depth: finger up/down.
# X-Mod:
# - The same 7 sources onto the modulators (LV2 parameters x_<source>_<destination>, -100..100): LFO 1/2 Rate and
#   Env 1/2 Time (x 2^(+-3) at 100), LFO 1/2 Amount (the LFO times 0..1: a positive cell fades it in with the
#   source, only negative cells fade it out). Rate cells of a tempo LFO are greyed out (they do nothing).
# Tabs and knob pages go together: a tap on a tab selects its knob page (Wave -> Position, Grain, Pitch,
# Filter, Mod) through Zynthian's control screen; turning to such a page shows its tab.
# - At the top right, in a small font: the name of the current knob page (screen_title of Zynthian's control screen).
import logging
import os
import threading
import time
import tkinter
import tkinter.font

from zyngui import zynthian_gui_config
from zyngui import zynthian_widget_base

MULTI_INSTANCE = True

NPEAKS = 1024                   # min/max pairs over the whole file
ST_EMPTY, ST_LOADING, ST_LOADED, ST_ERROR = range(4)
WAVE_COLOR = "#4dabf7"
CLIP_COLOR = "#ff6b6b"           # waveform columns past 0 dBFS after the sample gain
MARKER_COLOR = "#ffd43b"         # start and end markers
MARK_MAX = 10000                 # marker unit: 1/10000 of the sample
MARK_MIN_GAP = 10                # the markers keep at least 0.1 % apart
NORM_COLOR = "#51cf66"           # gain slider: the normalize point
GAIN_MAX = 240                   # sample gain, tenths of a dB, both ways
NORM_DB = -1.0                   # normalize: the sample peak goes here
GAIN_REBUILD = 0.15              # s: waveform redrawn at most this often while the gain is dragged
ZONE_COLOR = "#1d2a38"
PLAYHEAD_COLOR = "#ffffff"
GRAIN_COLOR = "#ffd43b"
NGRAIN_MARKS = 16
TABS = ["Wave", "Grain", "Pitch", "Filter", "Mod", "X-Mod"]
MOD_SRCS = [("lfo1", "LFO1"), ("lfo2", "LFO2"), ("env1", "Env1"), ("env2", "Env2"), ("vel", "Vel"),
            ("wheel", "Wheel"), ("rnd", "Rnd")]
MOD_DSTS = [("position", "Pos"), ("scan", "Scan"), ("spray", "Spray"), ("size", "Size"), ("density", "Dens"),
            ("pitch", "Pitch"), ("pan", "Pan"), ("level", "Lvl"), ("cutoff", "Cut")]
LFO_SHAPES = ["Sine", "Tri", "Saw up", "Saw dn", "Square", "S&H", "Smooth"]
LFO_SYNCS = ["Free", "Key", "Tempo"]
LFO_DIVS = ["8 bars", "4 bars", "2 bars", "1 bar", "1/2", "1/4", "1/4T", "1/8", "1/8T", "1/16", "1/16T", "1/32"]  # gc_div_names
LFO_MIN, LFO_MAX = 0.01, 20.0   # Hz at Rate 0 and 100 (GC_LFO_MIN/MAX, gc_rate_hz)
TAB_GROUPS = ["g_position", "g_grain", "g_pitch", "g_filter", "g_mod", "g_mod"]   # knob page (port group) of each tab
MOD_DSTS_SHORT = ["Pos", "Scn", "Spr", "Siz", "Dns", "Pit", "Pan", "Lvl", "Cut"]   # when the full names do not fit
# X-Mod tab: the same sources onto the modulators (LV2 parameters x_<source>_<destination>, GC_XDST_*)
XMOD_DSTS = [("lfo1_rate", "L1 Rate"), ("lfo2_rate", "L2 Rate"), ("lfo1_amt", "L1 Amt"), ("lfo2_amt", "L2 Amt"),
             ("env1_time", "E1 Time"), ("env2_time", "E2 Time")]
XMOD_DSTS_SHORT = ["L1R", "L2R", "L1A", "L2A", "E1T", "E2T"]
MOD_POS = (77, 171, 247)        # positive cells: blue
MOD_NEG = (255, 146, 43)        # negative cells: orange
DOUBLE_TAP = 0.35               # s
DRAG_PX = 1.5                   # px of finger movement per step (1 %)
GLIDE_MAX = 2000
BEND_MAX = 24
BEND_DEFAULT = 2
ROOT_PER_PX = 0.125            # semitones of Root per px
TUNE_PER_PX = 0.5              # cents of Tune per px
FILTER_TYPES = ["LP12", "HP12", "BP", "LP24", "HP24", "Notch", "Comb", "Vowel"]   # 0..2 kept (snapshots)
SR = 48000.0
NOTE_NAMES = ["C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"]


def note_name(n):
    """MIDI note number -> name with octave, C4 = 60."""
    n = int(n)
    return "%s%d" % (NOTE_NAMES[n % 12], n // 12 - 1)
# the same scales as gc_scale_names / scale_steps in grain_core.c
SCALES = [("Chromatic", list(range(12))), ("Major", [0, 2, 4, 5, 7, 9, 11]), ("Minor", [0, 2, 3, 5, 7, 8, 10]),
          ("Dorian", [0, 2, 3, 5, 7, 9, 10]), ("Phrygian", [0, 1, 3, 5, 7, 8, 10]),
          ("Lydian", [0, 2, 4, 6, 7, 9, 11]), ("Mixolydian", [0, 2, 4, 5, 7, 9, 10]),
          ("Harm. minor", [0, 2, 3, 5, 7, 8, 11]), ("Mel. minor", [0, 2, 3, 5, 7, 9, 11]),
          ("Major pent.", [0, 2, 4, 7, 9]), ("Minor pent.", [0, 3, 5, 7, 10]), ("Blues", [0, 3, 5, 6, 7, 10])]
BLACK_KEYS = (1, 3, 6, 8, 10)
NOTE_COLOR = "#ffd43b"
TAB_COLOR = "#4dabf7"
BUTTON_ON = "#4dabf7"
BUTTON_OFF = "#2a2a2a"
MAX_VOICES = 8
UPDATE_PERIOD = 0.08            # s: monitors and marks at ~12 per second, not at Zynthian's 30
ZONE_BAND = 5                   # px: the Window zone is a band above the waveform
GRAIN_STRIP = 9                 # px: grain marks are ticks in a strip between the band and the waveform


def window_value(x, shape):
    """Grain window at x in 0..1 for Shape 0..1: square, decay (1/3), bell (2/3), reverse (1).
    The same formula as envelope_shape()/window_value() in grain_core.c."""
    sustain = max(1.0 - 3.0 * shape, 0.0)
    t = (shape - 1.0 / 3.0) * 1.5
    t = 0.0 if t < 0.0 else (t * t * t * 4.0 if t < 0.5 else t)
    attack = t
    release = (1.0 - attack) * (1.0 - sustain)
    smooth = 2.0 * attack if attack < 0.5 else 2.0 - 2.0 * attack
    a, u, r = max(attack, 0.02), sustain, max(release, 0.04)
    extra = a + u + r - 1.0
    if extra > 0.0:
        if u >= extra:
            u -= extra
        else:
            over = extra - u
            u = 0.0
            if a > r:
                a -= over
            else:
                r -= over
    r = 1.0 - a - u
    if x <= 0.0 or x >= 1.0:
        return 0.0
    if x < a:
        q = x / a
        return q + (q * q * (3 - 2 * q) - q) * smooth
    if x < a + u:
        return 1.0
    q = (1.0 - x) / r
    base = q ** 4
    return base + (q * q * (3 - 2 * q) - base) * smooth


def rate_hz(units):
    """LFO Rate knob (0..100) in Free/Key: Hz, logarithmic (gc_rate_hz)"""
    x = min(max(units, 0.0), 100.0) / 100.0
    return LFO_MIN * (LFO_MAX / LFO_MIN) ** x


def rate_div(units):
    """LFO Rate knob (0..100) in Tempo: index into LFO_DIVS (gc_rate_div; C lrintf rounds half to even too)"""
    return int(round(min(max(units, 0.0), 100.0) * (len(LFO_DIVS) - 1) / 100.0))


def hz_text(hz):
    return "%.2f Hz" % hz if hz < 0.995 else ("%.1f Hz" % hz if hz < 9.95 else "%.0f Hz" % hz)


def lfo_value(shape, ph, a=0.6, b=-0.4):
    """LFO shapes of grain_core.c (gc_lfo_value); a, b: example random steps for S&H and smooth."""
    import math
    if shape == 1:
        return 4 * ph - 1 if ph < 0.5 else 3 - 4 * ph
    if shape == 2:
        return 2 * ph - 1
    if shape == 3:
        return 1 - 2 * ph
    if shape == 4:
        return 1.0 if ph < 0.5 else -1.0
    if shape == 5:
        return a if ph < 0.5 else b
    if shape == 6:
        q = (ph * 2) % 1.0
        x, y = (a, b) if ph < 0.5 else (b, -a)
        return x + (y - x) * q * q * (3 - 2 * q)
    return math.sin(2 * math.pi * ph)


def env_value(shape, t, time=1.0):
    """Env 1/2 of grain_core.c (gc_env_value)."""
    att, dec = shape * time, (1.0 - shape) * time
    if t < att:
        return t / att
    if dec < 0.001:
        return 1.0
    u = (t - att) / dec
    return 0.0 if u >= 1.0 else (1.0 - u) ** 2


def mix(rgb, f, bg=(42, 42, 42)):
    """Colour between the cell background and rgb, f in 0..1."""
    return "#%02x%02x%02x" % tuple(int(bg[k] + (rgb[k] - bg[k]) * f) for k in range(3))


VOWEL_F = [(650, 1080, 2650, 3500, 4500), (400, 1700, 2600, 3500, 4500), (290, 1870, 2800, 3600, 4600),
           (400, 800, 2600, 3300, 4300), (350, 600, 2700, 3300, 4300)]
VOWEL_A = [(1, 0.70, 0.50, 0.35, 0.25), (1, 0.55, 0.45, 0.35, 0.25), (1, 0.50, 0.35, 0.35, 0.25),
           (1, 0.60, 0.30, 0.30, 0.20), (1, 0.40, 0.25, 0.25, 0.20)]
VOWEL_GAIN = 3.0
VOWEL_BW = (80, 90, 120, 150, 200)
VOWEL_AIR_HZ, VOWEL_AIR = 6000.0, 0.5             # the input's top added back: the voice is not dull


def vowel_pos(fc):
    """Vowel filter: 0..4 = A E I O U, evenly over the Cutoff knob's travel in Zynthian
    (hz = 20 + 19980 (10^x - 1) / 9; gc_vowel_pos)"""
    import math
    fc = min(max(fc, 20.0), 20000.0)
    return 4.0 * math.log10(9.0 * (fc - 20.0) / 19980.0 + 1.0)


def filter_gain(f, fc, res, ftype, sr=SR, mix=1.0):
    """|H| of the voice filter at f Hz, as grain_core.c computes it: the TPT SVF is the bilinear transform of
    the analog prototype, so H is the analog formula at w = tan(pi f / sr) / tan(pi fc / sr).
    mix: the dry/wet Mix 0..1, |dry + mix (H - dry)|."""
    return abs(1.0 + mix * (filter_resp(f, fc, res, ftype, sr) - 1.0))


def filter_resp(f, fc, res, ftype, sr=SR):
    """Complex H of the voice filter at f Hz (see filter_gain)."""
    import cmath
    import math
    top = 0.45 * sr
    fc = min(max(fc, 20.0), top)
    fw = math.tan(math.pi * min(f, 0.499 * sr) / sr)

    def svf(fcut, k):
        s = 1j * fw / math.tan(math.pi * fcut / sr)
        d = s * s + k * s + 1.0
        return 1.0 / d, s / d, s * s / d          # low, band, high

    k = 1.4142136 * (1.0 - res) + 0.05 * res
    if ftype in (3, 4):                           # 24 dB: two Butterworth stages, Resonance on the second
        a, b = svf(fc, 1.847759), svf(fc, 0.765367 * (1.0 - res) + 0.05 * res)
        return a[0] * b[0] if ftype == 3 else a[2] * b[2]
    if ftype == 6:                                # comb: y = x + fb y[n - sr/fc], scaled by sqrt(1 - fb^2)
        fb = 0.3 + 0.67 * res
        return math.sqrt(1.0 - fb * fb) / (1.0 - fb * cmath.exp(-2j * math.pi * f / fc))
    if ftype == 7:                                # five formants, two band-passes each, gliding; plus the air
        pos = vowel_pos(fc)
        i0 = min(int(pos), 3)
        t = pos - i0
        bw = 1.6 - 1.2 * res
        h = 0j
        for n in range(5):
            ff = min(VOWEL_F[i0][n] + t * (VOWEL_F[i0 + 1][n] - VOWEL_F[i0][n]), top)
            kk = min(max(VOWEL_BW[n] * bw / ff, 0.02), 2.0)
            amp = VOWEL_GAIN * kk * kk * (VOWEL_A[i0][n] + t * (VOWEL_A[i0 + 1][n] - VOWEL_A[i0][n]))
            h += amp * svf(ff, kk)[1] ** 2
        return h + VOWEL_AIR * svf(min(VOWEL_AIR_HZ, top), 1.4142136)[2]
    lo, band, hi = svf(fc, k)
    if ftype == 1:
        return hi
    if ftype == 2:
        return band
    if ftype == 5:
        return lo + hi                       # notch = low + high
    return lo


def read_peaks(path, npeaks=NPEAKS):
    """(frames, rate, channels, [(min, max)] * n) of a sound file, channels mixed by their extremes."""
    try:
        import numpy as np
        import soundfile
    except ImportError:
        return _read_peaks_wave(path, npeaks)
    with soundfile.SoundFile(path) as f:
        frames, rate, ch = f.frames, f.samplerate, f.channels
        if frames <= 0:
            return frames, rate, ch, []
        n = min(npeaks, frames)
        bucket = -(-frames // n)                       # ceil: frames per peak
        peaks = []
        # read many buckets at a time, never the whole file at once
        for block in f.blocks(blocksize=bucket * 256, dtype="float32", always_2d=True):
            k = len(block) // bucket
            if k:
                b = block[:k * bucket].reshape(k, bucket * ch)
                peaks.extend(zip(b.min(axis=1).tolist(), b.max(axis=1).tolist()))
            if len(block) % bucket:
                rest = block[k * bucket:]
                peaks.append((float(rest.min()), float(rest.max())))
        return frames, rate, ch, peaks


def _read_peaks_wave(path, npeaks):
    """Fallback without numpy/soundfile (widget preview): 16-bit PCM WAV only."""
    import array
    import wave
    with wave.open(path, "rb") as w:
        ch, rate, frames, width = w.getnchannels(), w.getframerate(), w.getnframes(), w.getsampwidth()
        if width != 2:
            raise ValueError("preview reads 16-bit WAV only")
        data = array.array("h", w.readframes(frames))
    if frames <= 0:
        return frames, rate, ch, []
    n = min(npeaks, frames)
    bucket = -(-frames // n)
    step = bucket * ch
    peaks = []
    for i in range(0, len(data), step):
        s = data[i:i + step]
        peaks.append((min(s) / 32768.0, max(s) / 32768.0))
    return frames, rate, ch, peaks


class zynthian_widget_granular_sampler(zynthian_widget_base.zynthian_widget_base):

    def __init__(self, parent):
        super().__init__(parent)
        self.peaks = None            # peaks of the file in peaks_path
        self.peaks_path = None
        self.loading_path = None     # file read by the thread now
        self.result = None           # (path, peaks or None, error) handed over by the thread
        self.wave_size = None        # (width, height, gain) the waveform was drawn for
        self.wave_built = 0.0        # when it was drawn
        self.gain_view = None        # values the gain slider was drawn for
        self.mark_view = None        # values the start and end markers were drawn for
        self.info = None             # text shown now, to redraw it only when it changes
        self.title = None            # knob page name shown now
        self.zone = None             # (position, window, size) the zone was drawn for
        self.marks = None            # canvas items: [playhead] + grain marks
        self.mark_state = []         # last (x, shown) of each mark
        self.wave_img = None         # PhotoImage of the waveform (kept: Tk drops images without a reference)
        self.header_h = {}           # font size -> line height
        self.next_update = 0.0
        self.tab = 0
        self.tabs_drawn = None       # (tab, size) the tab bar was drawn for
        self.tab_boxes = []          # (x0, x1) of each tab
        self.wave_shown = False      # the waveform image is on the canvas
        self.grain_view = None       # values the Grain tab was drawn for
        self.buttons = []            # Grain and Pitch tabs: (x0, y0, x1, y1, symbol, value)
        self.pitch_view = None       # values the Pitch tab was drawn for
        self.filter_view = None      # values the Filter tab was drawn for
        self.mod_items = None        # Mod tab: {(s, d): (rect, text)} and side controls
        self.mod_vals = {}           # values the cells were drawn with
        self.mod_side = None         # values the side controls were drawn for
        self.drag = None             # (symbol, start value, start y or x, lo, hi, per px[, "x"]) while a finger is down
        self.last_tap = (None, 0.0)  # (symbol, time) for a double tap
        self.last_group = None       # knob page (port group symbol) seen on the previous refresh
        self.pitch_notes = None      # sounding pitch classes drawn
        self.widget_canvas = tkinter.Canvas(
            self, highlightthickness=0, relief='flat',
            bg=zynthian_gui_config.color_bg)
        self.widget_canvas.grid(sticky='news')
        self.widget_canvas.bind("<ButtonPress-1>", self._on_press)
        self.widget_canvas.bind("<B1-Motion>", self._on_motion)
        self.widget_canvas.bind("<ButtonRelease-1>", self._on_release)

    def update(self):
        """Zynthian calls this every 32 ms: each call also asks jalv for all monitors. Slow it down."""
        now = time.monotonic()
        if now < self.next_update:
            return
        self.next_update = now + UPDATE_PERIOD
        super().update()

    def on_size(self, event):
        if not super().on_size(event):
            return
        self.widget_canvas.grid(row=0, column=0, sticky='news')
        self.wave_size = None
        self._clear()
        self.refresh_gui()

    def _clear(self):
        """Empty canvas: everything is drawn again on the next refresh (tab switch, resize)."""
        self.widget_canvas.delete("all")
        self.info = None
        self.title = None
        self.zone = None
        self.marks = None
        self.mark_state = []
        self.tabs_drawn = None
        self.wave_shown = False
        self.grain_view = None
        self.pitch_view = None
        self.pitch_notes = None
        self.filter_view = None
        self.mod_items = None
        self.mod_vals = {}
        self.mod_side = None
        self.drag = None
        self.glide_box = None
        self.mix_box = None
        self.bend_box = None
        self.wave_boxes = {}
        self.wave_ctl = None
        self.gain_view = None
        self.mark_view = None
        self.buttons = []

    # ---------- plugin access ----------
    def _sample_ctrl(self):
        try:
            return self.processor.controllers_dict["sample"]
        except Exception:
            return None

    def _path(self):
        z = self._sample_ctrl()
        p = str(z.value) if z is not None and z.value else ""
        return "" if p == "_" else p

    def _page_title(self):
        try:
            return str(self.zyngui_control.screen_title or "")
        except Exception:
            return ""

    def _header_h(self):
        """Height of the page name line, from the real font metrics (Tk sizes are points, not pixels)."""
        fs = self._font_size()
        if fs not in self.header_h:
            try:
                f = tkinter.font.Font(family=zynthian_gui_config.font_family, size=fs)
                self.header_h[fs] = f.metrics("linespace") + 4
            except Exception:
                self.header_h[fs] = 2 * fs
        return self.header_h[fs]

    def _font_size(self):
        return max(8, min(max(self.height, 20) // 14, 13))

    # ---------- tab <-> knob page ----------
    def _page_rows(self):
        """Zynthian's list of control pages: (index, group symbol) of this processor's pages."""
        try:
            rows = self.zyngui_control.list_data
        except Exception:
            return []
        out = []
        for i, row in enumerate(rows):
            if len(row) >= 5 and row[0] and row[3] is self.processor:
                out.append((i, row[0]))
        return out

    def _select_page(self, tab):
        """A tab shows its knob page: Wave -> Position, Grain, Pitch, Filter, Mod."""
        for i, group in self._page_rows():
            if group == TAB_GROUPS[tab]:
                try:
                    self.zyngui_control.select(i)
                except Exception as e:
                    logging.debug("Granular Sampler widget: can't select page %s => %s", group, e)
                return

    def _current_group(self):
        try:
            info = self.zyngui_control.screen_info
            if info and len(info) >= 5 and info[3] is self.processor:
                return info[0]
        except Exception:
            pass
        return None

    def _follow_page(self):
        """The knob page changed (a knob or the menu): show its tab. Pages without a tab keep the tab."""
        group = self._current_group()
        if group == self.last_group:
            return
        self.last_group = group
        if group in TAB_GROUPS and TAB_GROUPS.index(group) != self.tab:
            self.tab = TAB_GROUPS.index(group)
            self._clear()

    def _zval(self, sym, default):
        try:
            return float(self.processor.controllers_dict[sym].value)
        except Exception:
            return default

    def _zset(self, sym, value):
        try:
            z = self.processor.controllers_dict[sym]
            if z.value != value:
                z.set_value(value)
            return True
        except Exception as e:
            logging.debug("Granular Sampler widget: can't set %s => %s", sym, e)
            return False

    def _area(self):
        """Top and bottom of the waveform area (zone band and grain strip are just above it).
        Below it one row: the file info and the sample controls (Root, Tune, Auto)."""
        h = max(self.height, 20)
        return self._header_h() + ZONE_BAND + GRAIN_STRIP + 4, h - self._button_h() - 6

    def _gain_w(self):
        """Width of the gain slider at the right of the waveform."""
        return max(16, self._font_size() * 2)

    def _wave_w(self):
        return max(int(self.width), 40) - self._gain_w() - 4

    def _gain(self):
        return min(max(int(round(self._zval("gain", 0))), -GAIN_MAX), GAIN_MAX)

    def _norm_gain(self):
        """Gain (tenths of a dB) that puts the sample peak at NORM_DB, or None."""
        pk = self._mon("peak", -100.0)
        if pk <= -99.0:
            return None
        return min(max(int(round((NORM_DB - pk) * 10)), -GAIN_MAX), GAIN_MAX)

    def _strip(self):
        top = self._area()[0]
        return top - GRAIN_STRIP - 1, top - 1

    def _mon(self, key, default=0.0):
        try:
            return float((self.monitors or {}).get(key, default))
        except Exception:
            return default

    # ---------- touch ----------
    def _on_press(self, event):
        if event is not None and event.y < self._header_h():          # tab bar
            for i, (x0, x1) in enumerate(self.tab_boxes):
                if x0 <= event.x < x1 and i != self.tab:
                    self.tab = i
                    self.last_group = TAB_GROUPS[i]
                    self._clear()
                    self._select_page(i)
                    self.refresh_gui()
            return
        if self.tab in (1, 2, 3) and self._slider_press(event):
            return
        if self.tab in (1, 2):
            self._button_press(event)
            return
        if self.tab in (4, 5):
            self._mod_press(event)
            return
        if self.tab != 0:
            return
        if self._wave_press(event):
            return
        z = self._sample_ctrl()
        if z is None:
            return
        try:
            z.nudge(0)                    # a path controller opens Zynthian's file selector
        except Exception as e:
            logging.warning("Granular Sampler widget: can't open file selector => %s", e)

    # ---------- waveform reading (background thread) ----------
    def _start_reading(self, path):
        self.loading_path = path
        threading.Thread(target=self._read_thread, args=(path,), daemon=True,
                         name="granular_sampler_peaks").start()

    def _read_thread(self, path):
        try:
            res = (path, read_peaks(path), None)
        except Exception as e:
            res = (path, None, str(e))
        self.result = res                 # picked up by refresh_gui() in the GUI thread

    # ---------- drawing ----------
    def refresh_gui(self):
        if not self.processor:
            return
        path = self._path()
        res, self.result = self.result, None
        if res is not None:
            rpath, info, err = res
            self.loading_path = None
            if err:
                logging.warning("Granular Sampler widget: can't read %s => %s", rpath, err)
            if rpath == path:
                self.peaks, self.peaks_path = (info[3] if info else []), rpath
                self.wave_size = None
        if path != self.peaks_path:
            if self.peaks:
                self.peaks = None
                self.wave_size = None
            if path and self.loading_path is None:
                self._start_reading(path)
            elif not path:
                self.peaks_path = ""
        self._follow_page()
        self._draw_tabs()
        self._draw_title()
        if self.tab == 1:
            self._draw_grain()
            return
        if self.tab == 2:
            self._draw_pitch()
            return
        if self.tab == 3:
            self._draw_filter()
            return
        if self.tab in (4, 5):
            self._draw_mod()
            return
        size = (self.width, self.height, self._gain())
        dragging = bool(self.drag) and self.drag[0] == "gain"
        if self.wave_size != size and (not dragging or self.wave_size is None or
                                       time.monotonic() - self.wave_built >= GAIN_REBUILD):
            self.wave_built = time.monotonic()
            self._build_wave()
            self.wave_size = size
            self.wave_shown = False
        if not self.wave_shown:
            self.widget_canvas.delete("wave")
            self.widget_canvas.create_image(0, self._area()[0], anchor="nw", image=self.wave_img, tags="wave")
            self.wave_shown = True
            self.widget_canvas.tag_raise("zone")
            self.widget_canvas.tag_raise("marks")
            self.widget_canvas.tag_raise("info")
        self._draw_zone()
        self._draw_marks()
        self._draw_markers()
        self._draw_wave_ctl()
        self._draw_gain()
        self._draw_info(path)

    def _markers(self):
        """Start and end markers, 0..1 of the sample, start <= end."""
        a = min(max(self._zval("mark_start", 0) / MARK_MAX, 0.0), 1.0)
        b = min(max(self._zval("mark_end", MARK_MAX) / MARK_MAX, 0.0), 1.0)
        return (a, b) if a <= b else (b, a)

    def _draw_markers(self):
        """Yellow lines with a grab tab in the zone band; what is cut away is shaded."""
        a, b = self._markers()
        view = (a, b, self.width, self.height, bool(self.peaks))
        if view == self.mark_view:
            return
        self.mark_view = view
        c = self.widget_canvas
        c.delete("markers")
        if not self.peaks:
            return
        w = self._wave_w()
        top, bottom = self._area()
        s0 = self._strip()[0] - ZONE_BAND - 1
        for x0, x1 in ((0, a * w), (b * w, w)):
            if x1 - x0 >= 1:
                c.create_rectangle(x0, top, x1, bottom, fill=zynthian_gui_config.color_bg, stipple="gray50",
                                   width=0, tags="markers")
        tab = max(6, self._font_size() // 2 + 2)
        for x, d in ((a * w, 1), (b * w, -1)):
            x = min(max(x, 1), w - 1)
            c.create_line(x, s0, x, bottom, fill=MARKER_COLOR, width=2, tags="markers")
            c.create_polygon(x, s0, x + d * tab, s0, x, s0 + tab, fill=MARKER_COLOR, outline="", tags="markers")
        c.tag_raise("marks")
        c.tag_raise("info")

    def _marker_press(self, event):
        """A press near a marker drags it (finger left/right); a double tap puts it back at the file's edge."""
        if not self.peaks:
            return False
        top, bottom = self._area()
        if not (self._strip()[0] - ZONE_BAND - 1 <= event.y <= bottom):
            return False
        w = self._wave_w()
        a, b = self._markers()
        grab = max(10, self._font_size())
        da, db = abs(event.x - a * w), abs(event.x - b * w)
        if min(da, db) > grab:
            return False
        if da < db or (da == db and event.x <= a * w):
            sym, dflt = "mark_start", 0
            lo, hi = 0, int(round(b * MARK_MAX)) - MARK_MIN_GAP
        else:
            sym, dflt = "mark_end", MARK_MAX
            lo, hi = int(round(a * MARK_MAX)) + MARK_MIN_GAP, MARK_MAX
        now = time.monotonic()
        if self.last_tap[0] == sym and now - self.last_tap[1] < DOUBLE_TAP:
            self._zset(sym, dflt)
            self.last_tap = (None, 0.0)
            self.drag = None
            self.refresh_gui()
            return True
        self.last_tap = (sym, now)
        cur = int(round(self._zval(sym, dflt)))
        self.drag = (sym, cur, event.x, max(lo, 0), min(max(hi, 0), MARK_MAX), MARK_MAX / float(max(w, 20)), "x")
        return True

    def _gain_box(self):
        top, bottom = self._area()
        x1 = max(int(self.width), 40) - 2
        return (x1 - self._gain_w(), self._strip()[0], x1, bottom)

    def _gain_y(self, gain, box):
        return box[1] + (GAIN_MAX - gain) / (2.0 * GAIN_MAX) * (box[3] - box[1])

    def _draw_gain(self):
        """Sample gain: a vertical bar at the right of the waveform, 0 dB in the middle; the normalize
        point is a green tick. Finger up/down, double tap = normalize."""
        gain, norm = self._gain(), self._norm_gain()
        view = (gain, norm, self.width, self.height)
        if view == self.gain_view:
            return
        self.gain_view = view
        c = self.widget_canvas
        c.delete("gain")
        box = self._gain_box()
        x0, y0, x1, y1 = box
        c.create_rectangle(x0, y0, x1, y1, fill=BUTTON_OFF, outline="#505050", tags="gain")
        yz, yg = self._gain_y(0, box), self._gain_y(gain, box)
        c.create_line(x0, yz, x1, yz, fill="#606060", tags="gain")              # 0 dB
        if gain:
            c.create_rectangle(x0 + 2, min(yz, yg), x1 - 2, max(yz, yg), fill="#2d6c99", width=0, tags="gain")
        if norm is not None:
            yn = self._gain_y(norm, box)
            c.create_line(x0, yn, x1, yn, fill=NORM_COLOR, width=2, tags="gain")
        c.create_line(x0, yg, x1, yg, fill=zynthian_gui_config.color_tx, width=3, tags="gain")

    def _draw_wave_ctl(self):
        """Root, Tune and Auto at the right of the bottom row; redrawn only when their values change."""
        root = int(round(self._zval("root", 60)))
        tune = int(round(self._zval("tune", 0)))
        det = self._mon("detected", -1.0)
        det_r = round(det, 2) if det >= 0 else -1
        view = (root, tune, det_r, self.width, self.height)
        if view == self.wave_ctl:
            return
        self.wave_ctl = view
        c = self.widget_canvas
        c.delete("wctl")
        w = max(self.width, 80)
        fam, fs = zynthian_gui_config.font_family, self._font_size()
        tx = zynthian_gui_config.color_tx
        bh = self._button_h()
        y0 = self._area()[1] + 3
        y1 = y0 + bh
        try:
            f = tkinter.font.Font(family=fam, size=fs)
            tw = lambda t: f.measure(t)
        except Exception:
            tw = lambda t: len(t) * fs * 0.6
        if det >= 0:
            dn = int(round(det))
            auto_t = "Auto %s" % note_name(dn)
        else:
            auto_t = "Auto -"
        items = [("root", "Root %s" % note_name(root), tw("Root C#4") + 10),
                 ("tune", "%+dc" % tune, tw("+50c") + 10),
                 ("auto", auto_t, tw("Auto C#4") + 10)]
        x = w - 4
        self.wave_boxes = {}
        for sym, text, bw in reversed(items):
            x0 = x - bw
            if sym == "auto":
                on = det >= 0 and root == int(round(det)) and tune == int(round((det - round(det)) * 100))
                fill = BUTTON_ON if on else BUTTON_OFF
                tcol = "#000000" if on else (tx if det >= 0 else "#505050")
            else:
                fill, tcol = BUTTON_OFF, tx
            c.create_rectangle(x0, y0, x, y1, fill=fill, outline="#505050", tags="wctl")
            c.create_text((x0 + x) / 2, (y0 + y1) / 2, text=text, fill=tcol, font=(fam, fs), tags="wctl")
            self.wave_boxes[sym] = (x0, y0, x, y1)
            x = x0 - 4
        self.wave_ctl_left = x

    def _build_wave(self):
        """The waveform image, built once per file and size (kept across tab switches)."""
        self.wave_img = None
        w = self._wave_w()
        top, bottom = self._area()
        hgt = max(int(bottom - top), 2)
        g = 10.0 ** (self._gain() / 200.0)            # the waveform as it sounds: with the sample gain
        img = tkinter.PhotoImage(width=w, height=hgt)
        img.put(zynthian_gui_config.color_bg, to=(0, 0, w, hgt))
        mid, half = hgt / 2.0, hgt / 2.0
        img.put("#303030", to=(0, int(mid), w, int(mid) + 1))
        p = self.peaks
        if p:
            # one min/max pair per screen column; columns with the same extent go in one put()
            n = len(p)
            run = None                                    # [x0, y0, y1]
            for x in range(w + 1):
                if x < w:
                    seg = p[x * n // w:max(x * n // w + 1, (x + 1) * n // w)]
                    mx = max(s[1] for s in seg) * g
                    mn = min(s[0] for s in seg) * g
                    y0 = int(mid - max(min(mx, 1.0), 0.0) * half)
                    y1 = max(int(mid - min(max(mn, -1.0), 0.0) * half), y0) + 1
                    clip = mx > 1.0 or mn < -1.0
                if run and (x == w or (y0, y1, clip) != (run[1], run[2], run[3])):
                    img.put(CLIP_COLOR if run[3] else WAVE_COLOR, to=(run[0], run[1], x, min(run[2], hgt)))
                    run = None
                if x < w and run is None:
                    run = [x, y0, y1, clip]
        self.wave_img = img

    def _draw_zone(self):
        """Where the playhead loops: from Position, Window wide; wraps past the end like the engine.
        A band above the waveform and a line at Position (on top of the image: drawn once per change)."""
        pos = min(max(self._zval("position", 0.0) / 100.0, 0.0), 1.0)
        win = min(max(self._zval("window", 100.0) / 100.0, 0.001), 1.0)
        ma, mb = self._markers()
        key = (pos, win, ma, mb, self.width, self.height, bool(self.peaks))
        if key == self.zone:
            return
        self.zone = key
        c = self.widget_canvas
        c.delete("zone")
        if not self.peaks:
            return
        w = self._wave_w()
        top, bottom = self._area()
        b1 = self._strip()[0] - 1
        b0 = b1 - ZONE_BAND
        ra, rl = ma * w, max(mb - ma, 0.0002) * w          # Position and Window live between the markers
        x0, x1 = ra + pos * rl, ra + (pos + win) * rl
        c.create_rectangle(x0, b0, min(x1, ra + rl), b1, fill=ZONE_COLOR, width=0, tags="zone")
        if x1 > ra + rl:
            c.create_rectangle(ra, b0, x1 - rl, b1, fill=ZONE_COLOR, width=0, tags="zone")
        c.create_line(x0, b0, x0, bottom, fill="#5c7a99", tags="zone")      # Position
        c.tag_raise("marks")

    def _draw_marks(self):
        c = self.widget_canvas
        if self.marks is None:
            self.marks = [c.create_line(0, 0, 0, 0, fill=PLAYHEAD_COLOR, width=2, state="hidden", tags="marks")]
            for _ in range(NGRAIN_MARKS):
                self.marks.append(c.create_line(0, 0, 0, 0, fill=GRAIN_COLOR, width=1, state="hidden", tags="marks"))
            self.mark_state = [None] * len(self.marks)
        w = self._wave_w()
        top, bottom = self._area()
        s0, s1 = self._strip()
        vals = [self._mon("playhead", -1.0)] + [self._mon("grain%d" % (i + 1), -1.0) for i in range(NGRAIN_MARKS)]
        show = bool(self.peaks)
        # Tk redraws one rectangle around everything that changed: grain ticks (thin strip, full width) are
        # flushed first, so the playhead (full height, narrow) is redrawn as a separate small rectangle.
        for k in list(range(1, len(self.marks))) + [0]:
            item, v = self.marks[k], vals[k]
            if k == 0:
                c.update_idletasks()
            st = (int(v * w), top, bottom) if show and 0.0 <= v <= 1.0 else None
            if st == self.mark_state[k]:
                continue                                  # unchanged: no canvas work at all
            prev = self.mark_state[k]
            self.mark_state[k] = st
            if st is None:
                c.itemconfigure(item, state="hidden")
                continue
            x = st[0]
            if k == 0:
                c.coords(item, x, s0, x, bottom)
            else:
                c.coords(item, x, s0, x, s1)
            if prev is None:
                c.itemconfigure(item, state="normal")

    def _tab_metrics(self, title):
        """(font size, padding, widths, title shown): the tabs and the page title share the top line. The padding
        shrinks first, then the font; if the tabs still do not fit beside the title, the title goes."""
        fam, fs = zynthian_gui_config.font_family, self._font_size()

        def measure(size, text):
            try:
                return tkinter.font.Font(family=fam, size=size).measure(text)
            except Exception:
                return len(text) * size
        w = max(self.width, 80)
        for show in ((True, False) if title else (False,)):
            avail = w - 4 - (measure(fs, title) + 14 if show else 0)
            for size in range(fs, max(7, fs - 4) - 1, -1):
                for pad in (2 * size, size, size // 2):
                    widths = [measure(size, t) + pad for t in TABS]
                    if sum(widths) + 4 * len(TABS) <= avail:
                        return size, pad, widths, show
        size = max(7, fs - 4)
        return size, size // 2, [measure(size, t) + size // 2 for t in TABS], False

    def _draw_tabs(self):
        title = self._page_title()
        key = (self.tab, self.width, self.height, title)
        if key == self.tabs_drawn:
            return
        self.tabs_drawn = key
        c = self.widget_canvas
        c.delete("tabs")
        hh = self._header_h()
        size, pad, widths, show = self._tab_metrics(title)
        self.title_shown = show
        self.title = None                                 # the title follows (shown or not)
        font = (zynthian_gui_config.font_family, size)
        x = 2
        self.tab_boxes = []
        for i, (t, tw) in enumerate(zip(TABS, widths)):
            sel = i == self.tab
            c.create_rectangle(x, 1, x + tw, hh - 2, outline=TAB_COLOR, fill=(TAB_COLOR if sel else ""), tags="tabs")
            c.create_text(x + tw / 2, hh / 2, text=t, font=font, tags="tabs",
                          fill=("#000000" if sel else TAB_COLOR))
            self.tab_boxes.append((x, x + tw + 2))
            x += tw + 4

    # ---------- Grain tab ----------
    def _grain_layout(self):
        w, h = max(self.width, 80), max(self.height, 60)
        top = self._header_h() + 6
        pad = 8
        plot = (pad, top, int(w * 0.5) - pad, h - pad - self._font_size() * 2)
        right = int(w * 0.5) + pad
        return w, h, top, pad, plot, right

    def _draw_grain(self):
        shape = min(max(self._zval("shape", 67.0) / 100.0, 0.0), 1.0)
        size = self._zval("size", 100.0)
        dens = self._zval("density", 20.0)
        voices = int(round(self._zval("voices", MAX_VOICES)))
        glide = int(round(self._zval("glide", 0)))
        key = (round(shape * 30), round(size, 1), round(dens, 2), voices, glide, self.width, self.height)
        if key == self.grain_view:
            return
        self.grain_view = key
        c = self.widget_canvas
        c.delete("grain")
        w, h, top, pad, (px0, py0, px1, py1), right = self._grain_layout()
        fam, fs = zynthian_gui_config.font_family, self._font_size()
        tx, tx_off = zynthian_gui_config.color_tx, zynthian_gui_config.color_tx_off
        # window shape (as the engine quantizes it: 1/30 steps)
        q = round(shape * 30) / 30.0
        c.create_rectangle(px0, py0, px1, py1, outline="#303030", tags="grain")
        n = 64
        pts = []
        for i in range(n + 1):
            x = i / float(n)
            pts.extend((px0 + x * (px1 - px0), py1 - window_value(x, q) * (py1 - py0 - 4) - 2))
        c.create_line(pts, fill=WAVE_COLOR, width=2, tags="grain")
        names = {0: "square", 10: "decay", 20: "bell", 30: "reverse"}
        label = names.get(round(q * 30), "")
        c.create_text(px0, py1 + 3, anchor="nw", text="Shape %d %%  %s" % (round(shape * 100), label),
                      fill=tx_off, font=(fam, fs), tags="grain")
        # size, density, overlap
        overlap = dens * size / 1000.0
        c.create_text(right, py1 + 3, anchor="nw", tags="grain", fill=tx_off, font=(fam, fs),
                      text="%d ms  %.1f /s  overlap %.1f" % (round(size), dens, overlap))
        # voices 1..8; the first button toggles 1 (mono) <-> L (mono with legato, voices = 0)
        self.buttons = []
        bh = self._button_h()
        y = py0
        c.create_text(right, y, anchor="nw", text="Voices   (tap 1 again: L = legato)", fill=tx_off,
                      font=(fam, fs), tags="grain")
        y += fs * 2
        bw = (w - pad - right) / float(MAX_VOICES)
        for i in range(MAX_VOICES):
            x0 = right + i * bw
            if i == 0:
                text, value = ("L", 1) if voices == 0 else ("1", 0 if voices == 1 else 1)
            else:
                text, value = str(i + 1), i + 1
            self._button(x0, y, x0 + bw - 3, y + bh, text, (i + 1) <= max(voices, 1), True, "voices", value)
        # Glide: mono portamento, finger up/down; only with 1 voice (1 or L)
        y += bh + fs
        mono = voices <= 1
        gx0, gx1 = right, w - pad
        self.glide_box = (gx0, y, gx1, y + bh) if mono else None
        c.create_rectangle(gx0, y, gx1, y + bh, fill=BUTTON_OFF, outline=("#505050" if mono else "#303030"),
                           tags="grain")
        glide = int(round(self._zval("glide", 0)))
        frac = min(glide, GLIDE_MAX) / float(GLIDE_MAX)
        if mono and frac > 0:
            c.create_rectangle(gx0 + 1, y + 1, gx0 + 1 + frac * (gx1 - gx0 - 2), y + bh - 1, fill="#2d6c99", width=0,
                               tags="grain")
        c.create_text(gx0 + 6, y + bh / 2, anchor="w", tags="grain", font=(fam, fs),
                      fill=(tx if mono else "#505050"),
                      text=("Glide %d ms" % glide) if glide else "Glide off")

    def _button_h(self):
        """Button height from the font: text plus a small margin."""
        return int(self._header_h() * 1.3)

    def _button(self, x0, y0, x1, y1, text, on, enabled, sym, value, tag="grain"):
        c = self.widget_canvas
        fill = BUTTON_ON if on else BUTTON_OFF
        c.create_rectangle(x0, y0, x1, y1, fill=fill, outline=("#505050" if enabled else "#303030"), tags=tag)
        c.create_text((x0 + x1) / 2, (y0 + y1) / 2, text=text, tags=tag,
                      fill=("#000000" if on else (zynthian_gui_config.color_tx if enabled else "#505050")),
                      font=(zynthian_gui_config.font_family, self._font_size()))
        if enabled:
            self.buttons.append((x0, y0, x1, y1, sym, value))

    def _slider_press(self, event):
        """Glide (Grain tab), Bend range (Pitch tab), Mix (Filter tab), Root and Tune (Wave tab): finger right/left
        (easier with a stylus than up/down), double tap back to the default.
        Glide, Bend and Mix: one swipe across the bar covers the whole range."""
        wb = self.wave_boxes if self.tab == 0 else {}

        def across(box, hi):
            return hi / float(max(box[2] - box[0], 20)) if box else 1.0
        gb = self.glide_box if self.tab == 1 else None
        bb = self.bend_box if self.tab == 2 else None
        mb = self.mix_box if self.tab == 3 else None
        for box, sym, lo, hi, per_px, dflt in (
                (gb, "glide", 0, GLIDE_MAX, across(gb, GLIDE_MAX), 0),
                (mb, "filter_mix", 0, 100, across(mb, 100), 100),
                (bb, "bend_range", 0, BEND_MAX, across(bb, BEND_MAX), BEND_DEFAULT),
                (wb.get("root"), "root", 0, 127, ROOT_PER_PX, 60),
                (wb.get("tune"), "tune", -50, 50, TUNE_PER_PX, 0)):
            if not box or not (box[0] <= event.x <= box[2] and box[1] <= event.y <= box[3]):
                continue
            now = time.monotonic()
            if self.last_tap[0] == sym and now - self.last_tap[1] < DOUBLE_TAP:
                self._zset(sym, dflt)
                self.last_tap = (None, 0.0)
                self.drag = None
                self.pitch_view = None
                self.refresh_gui()
                return True
            self.last_tap = (sym, now)
            self.drag = (sym, int(round(self._zval(sym, dflt))), event.x, lo, hi, per_px, "x")
            return True
        return False

    def _wave_press(self, event):
        """Wave tab controls row: Root and Tune sliders, Auto takes the detected pitch."""
        if event is None:                              # a press without a position: the file selector
            return False
        if self._marker_press(event):                  # markers first: the end one sits next to the gain bar
            return True
        gb = self._gain_box()
        if gb[0] - 1 <= event.x <= gb[2] + 2 and gb[1] <= event.y <= gb[3]:
            now = time.monotonic()
            if self.last_tap[0] == "gain" and now - self.last_tap[1] < DOUBLE_TAP:
                norm = self._norm_gain()                  # double tap: normalize
                if norm is not None:
                    self._zset("gain", norm)
                self.last_tap = (None, 0.0)
                self.drag = None
                self.refresh_gui()
                return True
            self.last_tap = ("gain", now)
            self.drag = ("gain", self._gain(), event.y, -GAIN_MAX, GAIN_MAX,
                         2.0 * GAIN_MAX / float(max(gb[3] - gb[1], 20)))
            return True
        if self._slider_press(event):
            return True
        b = self.wave_boxes.get("auto")
        if b and b[0] <= event.x <= b[2] and b[1] <= event.y <= b[3]:
            det = self._mon("detected", -1.0)
            if det >= 0:
                root = int(round(det))
                self._zset("root", root)
                self._zset("tune", int(round((det - root) * 100)))
                self.refresh_gui()
            return True
        # the whole row is controls: a miss there does not open the file selector
        return event.y >= self._area()[1]

    def _button_press(self, event):
        for x0, y0, x1, y1, sym, value in self.buttons:
            if x0 <= event.x <= x1 and y0 <= event.y <= y1:
                self._zset(sym, value)
                self.grain_view = None
                self.pitch_view = None
                self.refresh_gui()
                return

    # ---------- Mod tab ----------
    def _mod_label_font(self, cw):
        """Largest small font (and names) whose column labels fit the cell width: no overlapping text."""
        fam, fs = zynthian_gui_config.font_family, self._font_size()
        dsts, short = self._grid_dsts()
        for names in ([d[1] for d in dsts], short):
            for size in range(max(6, fs - 4), 5, -1):
                try:
                    f = tkinter.font.Font(family=fam, size=size)
                    if max(f.measure(n) for n in names) <= cw - 3:
                        return size, names
                except Exception:
                    return max(6, fs - 4), names
        return 6, short

    def _mod_row_label_w(self):
        """Width of the source names at the grid's small font (measured once per font size)."""
        fs = max(6, self._font_size() - 4)
        if getattr(self, "row_label_w", (None,))[0] != fs:
            try:
                f = tkinter.font.Font(family=zynthian_gui_config.font_family, size=fs)
                self.row_label_w = (fs, max(f.measure(n) for _, n in MOD_SRCS) + 6)
            except Exception:
                self.row_label_w = (fs, fs * 4)
        return self.row_label_w[1]

    def _mod_layout(self):
        w, h = max(self.width, 80), max(self.height, 60)
        pad = 6
        top = self._header_h() + 4
        fs = self._font_size()
        gx1 = int(w * 0.66) if self.tab == 4 else w - pad   # Mod: grid on the left two thirds; X-Mod: all
        label_w = self._mod_row_label_w()                # as wide as the source names, no more
        head_h = fs * 2
        gx0 = pad + label_w
        gy0 = top + head_h
        gy1 = h - pad
        cw = (gx1 - gx0) / float(len(self._grid_dsts()[0]))
        ch = (gy1 - gy0) / float(len(MOD_SRCS))
        return w, h, pad, top, gx0, gy0, gx1, gy1, cw, ch

    def _grid_dsts(self):
        """(destinations, short names) of the grid on this tab: Mod or X-Mod"""
        return (XMOD_DSTS, XMOD_DSTS_SHORT) if self.tab == 5 else (MOD_DSTS, MOD_DSTS_SHORT)

    def _cell_sym(self, si, di):
        if self.tab == 5:
            return "x_%s_%s" % (MOD_SRCS[si][0], XMOD_DSTS[di][0])
        return "m_%s_%s" % (MOD_SRCS[si][0], MOD_DSTS[di][0])

    def _xmod_idle(self, di):
        """X-Mod: a Rate column does nothing while its LFO follows the tempo"""
        return self.tab == 5 and di < 2 and int(round(self._zval("lfo%d_sync" % (di + 1), 0))) == 2

    def _side_boxes(self):
        """Right column: (symbol, kind, x0, y0, x1, y1, label); kind: pol, shape, sync, drag. The polarity
        buttons (+- / +) sit in the top right corner of the shape boxes, so they come first (hit test)."""
        w, h, pad, top, gx0, gy0, gx1, gy1, cw, ch = self._mod_layout()
        x0, x1 = gx1 + pad * 2, w - pad
        rows = [("lfo1_shape", "shape", "LFO 1"), ("lfo1_sync", "sync", ""),
                ("lfo2_shape", "shape", "LFO 2"), ("lfo2_sync", "sync", ""),
                ("env1_shape", "drag", "Env 1"), ("env2_shape", "drag", "Env 2"), ("mod_depth", "drag", "Depth")]
        rh = (h - pad - top) / 5.0
        out = []
        xm = x0 + (x1 - x0) * 0.62
        pw = self._pol_w()                               # polarity button: a small square
        for k in range(2):
            y = top + k * rh
            out.append(("lfo%d_uni" % (k + 1), "pol", xm - 3 - pw, y, xm - 3, y + pw, ""))
        out.append((rows[0][0], "shape", x0, top, xm - 3, top + rh - 4, rows[0][2]))
        out.append((rows[1][0], "sync", xm, top, x1, top + rh - 4, ""))
        out.append((rows[2][0], "shape", x0, top + rh, xm - 3, top + 2 * rh - 4, rows[2][2]))
        out.append((rows[3][0], "sync", xm, top + rh, x1, top + 2 * rh - 4, ""))
        for k, r in enumerate(rows[4:]):
            y = top + (2 + k) * rh
            out.append((r[0], "drag", x0, y, x1, y + rh - 4, r[2]))
        return out

    def _draw_mod(self):
        c = self.widget_canvas
        w, h, pad, top, gx0, gy0, gx1, gy1, cw, ch = self._mod_layout()
        fam, fs = zynthian_gui_config.font_family, self._font_size()
        if self.mod_items is None:                        # static part: once per size / tab switch
            c.delete("mod")
            self.mod_items = {}
            self.mod_vals = {}
            self.mod_side = None
            fsmall, names = self._mod_label_font(cw)
            self.mod_fsmall = fsmall
            for di, name in enumerate(names):
                c.create_text(gx0 + (di + 0.5) * cw, gy0 - 3, anchor="s", text=name, fill=zynthian_gui_config.color_tx_off,
                              font=(fam, fsmall), tags="mod")
            self.mod_labels = []
            for si, (_, name) in enumerate(MOD_SRCS):
                self.mod_labels.append(c.create_text(gx0 - 4, gy0 + (si + 0.5) * ch, anchor="e", text=name,
                                                     font=(fam, fsmall), tags="mod", fill="#606060"))
                for di in range(len(self._grid_dsts()[0])):
                    x0, y0 = gx0 + di * cw + 1, gy0 + si * ch + 1
                    r = c.create_rectangle(x0, y0, x0 + cw - 2, y0 + ch - 2, fill=mix(MOD_POS, 0), width=0, tags="mod")
                    t = c.create_text(x0 + cw / 2 - 1, y0 + ch / 2 - 1, text="", font=(fam, self.mod_fsmall), tags="mod",
                                      fill="#000000")
                    self.mod_items[(si, di)] = (r, t)
        # cells: only the changed ones are touched
        used = [False] * len(MOD_SRCS)
        for (si, di), (r, t) in self.mod_items.items():
            v = int(round(self._zval(self._cell_sym(si, di), 0)))
            if v:
                used[si] = True
            idle = self._xmod_idle(di)
            if self.mod_vals.get((si, di)) == (v, idle):
                continue
            self.mod_vals[(si, di)] = (v, idle)
            f = min(abs(v), 100) / 100.0
            if idle:                                      # tempo LFO: its Rate cells are greyed out
                c.itemconfigure(r, fill=mix((90, 90, 90), 0.35 * f + 0.1))
            else:
                c.itemconfigure(r, fill=mix(MOD_POS if v >= 0 else MOD_NEG, 0.25 + 0.75 * f if v else 0))
            c.itemconfigure(t, text=("%+d" % v) if v else "", fill=("#000000" if f > 0.5 else "#e0e0e0"))
        for si, item in enumerate(self.mod_labels):
            want = zynthian_gui_config.color_tx if used[si] else "#606060"
            if c.itemcget(item, "fill") != want:
                c.itemconfigure(item, fill=want)
        if self.tab == 4:
            self._draw_mod_side()

    def _pol_w(self):
        """Polarity button: a square as tall as the LFO box allows, at least twice the font"""
        w, h, pad, top = self._mod_layout()[:4]
        rh = (h - pad - top) / 5.0
        return int(max(14, min(rh - 4, self._font_size() * 2.2)))

    def _draw_mod_side(self):
        vals = tuple(int(round(self._zval(b[0], 0))) for b in self._side_boxes())
        rates = (self._zval("lfo1_rate", 61), self._zval("lfo2_rate", 42))
        key = (vals, rates, self.width, self.height)
        if key == self.mod_side:
            return
        self.mod_side = key
        c = self.widget_canvas
        c.delete("modside")
        fam, fs = zynthian_gui_config.font_family, self._font_size()
        fsmall = max(7, fs - 2)
        tx, tx_off = zynthian_gui_config.color_tx, zynthian_gui_config.color_tx_off
        items = sorted(zip(self._side_boxes(), vals), key=lambda bv: bv[0][1] == "pol")   # pol on top
        for (sym, kind, x0, y0, x1, y1, label), v in items:
            if kind == "pol":                             # +- (both ways) or + (0..1): what the LFO puts out
                on = v == 1
                c.create_rectangle(x0, y0, x1, y1, outline="#707070", fill=(BUTTON_ON if on else BUTTON_OFF),
                                   tags="modside")
                c.create_text((x0 + x1) / 2, (y0 + y1) / 2, text=("+" if on else "\u00b1"), tags="modside",
                              fill=("#000000" if on else tx), font=(fam, max(fs + 2, int((y1 - y0) * 0.5)), "bold"))
                continue
            c.create_rectangle(x0, y0, x1, y1, outline="#505050", fill=BUTTON_OFF, tags="modside")
            if kind == "sync":
                on = v >= 1
                rate = rates[0 if sym == "lfo1_sync" else 1]
                if v == 2:
                    text = "\u2669 " + LFO_DIVS[rate_div(rate)]
                else:
                    text = "%s\n%s" % (LFO_SYNCS[min(max(v, 0), 1)], hz_text(rate_hz(rate)))
                c.create_rectangle(x0, y0, x1, y1, outline="#505050", fill=(BUTTON_ON if on else BUTTON_OFF),
                                   tags="modside")
                c.create_text((x0 + x1) / 2, (y0 + y1) / 2, text=text, tags="modside",
                              fill=("#000000" if on else tx), font=(fam, fsmall))
                continue
            # label and value on top, a picture of the shape below
            if kind == "shape":                           # the shape's name if it fits beside the +- button
                text = "%s %s" % (label, LFO_SHAPES[min(max(v, 0), len(LFO_SHAPES) - 1)])
                room = x1 - x0 - 8 - self._pol_w()
                try:
                    f = tkinter.font.Font(family=fam, size=fsmall)
                    for t in (text, label, label.replace("LFO ", "L")):
                        text = t
                        if f.measure(t) <= room:
                            break
                except Exception:
                    pass
            else:
                text = "%s %d %%" % (label, v)
            c.create_text(x0 + 3, y0 + 2, anchor="nw", text=text, fill=tx, font=(fam, fsmall), tags="modside")
            py0, py1 = y0 + fsmall * 2 + 2, y1 - 3
            if py1 - py0 < 4:
                continue
            n = 32
            pts = []
            for i in range(n + 1):
                x = i / float(n)
                if kind == "shape":
                    yv = (lfo_value(v, x) + 1) / 2.0
                elif sym == "mod_depth":
                    yv = v / 100.0
                else:
                    yv = env_value(v / 100.0, x)
                pts.extend((x0 + 4 + x * (x1 - x0 - 8), py1 - yv * (py1 - py0)))
            c.create_line(pts, fill=WAVE_COLOR, width=2, tags="modside")

    def _mod_press(self, event):
        w, h, pad, top, gx0, gy0, gx1, gy1, cw, ch = self._mod_layout()
        now = time.monotonic()
        sym, lo, hi = None, 0, 100
        if gx0 <= event.x < gx1 and gy0 <= event.y < gy1:
            di = min(int((event.x - gx0) / cw), len(self._grid_dsts()[0]) - 1)
            si = min(int((event.y - gy0) / ch), len(MOD_SRCS) - 1)
            sym, lo, hi = self._cell_sym(si, di), -100, 100
        elif self.tab == 4:
            for bsym, kind, x0, y0, x1, y1, _ in self._side_boxes():
                if x0 <= event.x <= x1 and y0 <= event.y <= y1:
                    if kind == "pol":
                        self._zset(bsym, 1 - int(round(self._zval(bsym, 0))))
                    elif kind == "shape":
                        self._zset(bsym, (int(round(self._zval(bsym, 0))) + 1) % len(LFO_SHAPES))
                    elif kind == "sync":
                        # a tap (no movement, see _on_release) cycles Free/Key/Tempo; the rate is on the knob
                        self.sync_press = (bsym, event.y)
                        return
                    else:
                        sym = bsym
                    break
            if sym is None:
                self.refresh_gui()
                return
        else:
            return
        if self.last_tap[0] == sym and now - self.last_tap[1] < DOUBLE_TAP:
            default = 100 if sym == "mod_depth" else (10 if sym == "env1_shape" else (50 if sym == "env2_shape" else 0))
            self._zset(sym, default)                      # double tap: back to the default
            self.last_tap = (None, 0.0)
            self.drag = None
            self.refresh_gui()
            return
        self.last_tap = (sym, now)
        self.drag = (sym, int(round(self._zval(sym, 0))), event.y, lo, hi, 1.0 / DRAG_PX)

    def _on_motion(self, event):
        if not self.drag:
            return
        sym, start, p0, lo, hi, per_px = self.drag[:6]
        d = event.x - p0 if self.drag[6:] == ("x",) else p0 - event.y    # right / up = more
        v = int(min(max(start + d * per_px, lo), hi))
        if abs(d) > 6:
            self.last_tap = (None, 0.0)                   # a drag is not a tap
        self._zset(sym, v)
        self.refresh_gui()

    def _on_release(self, event):
        self.drag = None
        sp = getattr(self, "sync_press", None)
        self.sync_press = None
        if sp and (event is None or abs(event.y - sp[1]) < 6):      # a tap on a sync button: next mode
            self._zset(sp[0], (int(round(self._zval(sp[0], 0))) + 1) % len(LFO_SYNCS))
            self.refresh_gui()

    # ---------- Filter tab ----------
    DB_TOP, DB_BOTTOM = 30.0, -42.0

    def _filter_layout(self):
        """Plot box; below it one line for the frequency labels and one for the info (real font metrics)."""
        w, h = max(self.width, 80), max(self.height, 60)
        pad = 8
        top = self._header_h() + 6
        line = self._header_h()
        return (pad + 30, top, w - pad, h - 2 * line - 2)

    def _fx(self, f, x0, x1):
        import math
        return x0 + (math.log10(f) - math.log10(20.0)) / 3.0 * (x1 - x0)

    def _dby(self, db, y0, y1):
        db = min(max(db, self.DB_BOTTOM), self.DB_TOP)
        return y0 + (self.DB_TOP - db) / (self.DB_TOP - self.DB_BOTTOM) * (y1 - y0)

    def _draw_filter(self):
        import math
        fc = self._zval("cutoff", 20000.0)
        res = min(max(self._zval("resonance", 0.0) / 100.0, 0.0), 1.0)
        ftype = min(max(int(round(self._zval("ftype", 0))), 0), len(FILTER_TYPES) - 1)
        kt = self._zval("keytrack", 0.0)
        mix = min(max(int(round(self._zval("filter_mix", 100))), 0), 100)
        view = (round(fc), round(res * 100), ftype, round(kt), mix, self.width, self.height)
        if view == self.filter_view:
            return
        self.filter_view = view
        c = self.widget_canvas
        c.delete("filter")
        x0, y0, x1, y1 = self._filter_layout()
        fam, fs = zynthian_gui_config.font_family, self._font_size()
        tx_off = zynthian_gui_config.color_tx_off
        c.create_rectangle(x0, y0, x1, y1, outline="#303030", tags="filter")
        for f in (100, 1000, 10000):                      # grid
            x = self._fx(f, x0, x1)
            c.create_line(x, y0, x, y1, fill="#262626", tags="filter")
            c.create_text(x, y1 + 2, anchor="n", text=("%dk" % (f // 1000)) if f >= 1000 else str(f),
                          fill=tx_off, font=(fam, fs), tags="filter")
        for db in (24, 12, 0, -12, -24, -36):
            y = self._dby(db, y0, y1)
            c.create_line(x0, y, x1, y, fill=("#404040" if db == 0 else "#262626"), tags="filter")
            c.create_text(x0 - 3, y, anchor="e", text="%+d" % db if db else "0", fill=tx_off,
                          font=(fam, max(7, fs - 2)), tags="filter")
        open_lp = ftype in (0, 3) and fc >= 20000 * 0.97 and res < 0.01
        open_hp = ftype in (1, 4) and fc <= 21.0 and res < 0.01
        pts = []
        n = max(64, int(x1 - x0) // 3)
        for i in range(n + 1):
            f = 20.0 * 10 ** (3.0 * i / n)
            g = 1.0 if (open_lp or open_hp or mix == 0) else filter_gain(f, fc, res, ftype, mix=mix / 100.0)
            pts.extend((self._fx(f, x0, x1), self._dby(20 * math.log10(max(g, 1e-9)), y0, y1)))
        c.create_line(pts, fill=WAVE_COLOR, width=2, tags="filter")
        fct = ("%.1f kHz" % (fc / 1000.0)) if fc >= 1000 else ("%d Hz" % round(fc))
        if ftype == 7:                                    # Vowel: the cutoff picks the vowel, not a frequency
            pos = vowel_pos(fc)
            a = int(min(pos, 3.999))
            t = pos - a
            fct = "AEIOU"[a + (t >= 0.5)] if abs(t - 0.5) > 0.2 else "%s-%s" % ("AEIOU"[a], "AEIOU"[a + 1])
        else:
            xc = self._fx(min(max(fc, 20.0), 20000.0), x0, x1)
            c.create_line(xc, y0, xc, y1, fill="#5c7a99", dash=(3, 3), tags="filter")
        info = "%s  %s  Res %d %%  Key %d %%%s" % (FILTER_TYPES[ftype], fct, round(res * 100), round(kt),
                                                 "  (off)" if (open_lp or open_hp or mix == 0) else "")
        c.create_text(x0, max(self.height, 60) - 1, anchor="sw", text=info, fill=tx_off,
                      font=(fam, fs), tags="filter")
        # Mix (dry/wet): a bar in the top right corner of the plot, finger left/right, double tap = 100 %
        mx0, my0, mx1, my1 = self.mix_box = self._mix_box()
        c.create_rectangle(mx0, my0, mx1, my1, fill=BUTTON_OFF, outline="#505050", tags="filter")
        if mix > 0:
            c.create_rectangle(mx0 + 1, my0 + 1, mx0 + 1 + mix / 100.0 * (mx1 - mx0 - 2), my1 - 1, fill="#2d6c99",
                               width=0, tags="filter")
        c.create_text(mx0 + 6, (my0 + my1) / 2, anchor="w", text=("Mix %d %%" % mix) if mix else "Mix 0 % (dry)",
                      fill=zynthian_gui_config.color_tx, font=(fam, fs), tags="filter")

    def _mix_box(self):
        """Filter Mix bar: top right inside the plot, about 40 % of its width, a button high."""
        x0, y0, x1, y1 = self._filter_layout()
        fs = self._font_size()
        try:
            tw = tkinter.font.Font(family=zynthian_gui_config.font_family, size=fs).measure("Mix 0 % (dry)") + 14
        except Exception:
            tw = fs * 9
        bw = min(max(int((x1 - x0) * 0.4), tw), int(x1 - x0) - 4)
        bh = min(self._button_h(), int((y1 - y0) * 0.3))
        return (x1 - 3 - bw, y0 + 3, x1 - 3, y0 + 3 + bh)

    # ---------- Pitch tab ----------
    PITCH_ROWS = 4    # below the keys: 3 rows of scale buttons, then Bend range and the info

    def _pitch_layout(self):
        """Keys on top; their height is what the button rows below leave (at most 38 %)."""
        w, h = max(self.width, 80), max(self.height, 60)
        pad = 4
        top = self._header_h() + 4
        rows_h = self.PITCH_ROWS * (self._button_h() + 4) + self._font_size() // 2
        kh = min(int((h - top) * 0.38), h - pad - top - rows_h)
        kb = (pad, top, w - pad, top + max(kh, 12))
        return w, h, pad, kb

    def _draw_pitch(self):
        scale = min(max(int(round(self._zval("scale", 0))), 0), len(SCALES) - 1)
        key = min(max(int(round(self._zval("key", 0))), 0), 11)
        quant = self._zval("quantize", 0.0)
        pitch = self._zval("pitch", 0.0)
        spray = self._zval("pitch_spray", 0.0)
        bend = int(round(self._zval("bend_range", BEND_DEFAULT)))
        view = (scale, key, round(quant), round(pitch, 1), round(spray, 1), bend, self.width, self.height)
        c = self.widget_canvas
        w, h, pad, (kx0, ky0, kx1, ky1) = self._pitch_layout()
        kw = (kx1 - kx0) / 12.0
        if view != self.pitch_view:
            self.pitch_view = view
            self.pitch_notes = None
            c.delete("pitch")
            self.buttons = []
            fam, fs = zynthian_gui_config.font_family, self._font_size()
            steps = SCALES[scale][1]
            for pc in range(12):                       # keys: in the scale lit, tonic filled; tap = tonic
                deg = (pc - key) % 12
                in_scale = deg in steps
                x0 = kx0 + pc * kw
                black = pc in BLACK_KEYS
                if deg == 0:
                    fill = BUTTON_ON
                elif in_scale:
                    fill = "#1f4a6b" if black else "#2d6c99"
                else:
                    fill = "#151515" if black else "#303030"
                c.create_rectangle(x0 + 1, ky0, x0 + kw - 2, ky1, fill=fill, outline="#505050", tags="pitch")
                c.create_text(x0 + kw / 2, ky1 - 3, anchor="s", text=NOTE_NAMES[pc], tags="pitch",
                              fill=("#000000" if deg == 0 else (zynthian_gui_config.color_tx if in_scale else "#606060")),
                              font=(fam, fs))
                self.buttons.append((x0 + 1, ky0, x0 + kw - 2, ky1, "key", pc))
            # scale buttons: 3 rows of 4
            bh = self._button_h()
            y = ky1 + fs // 2 + 4
            cols = 4
            bw = (w - 2 * pad) / float(cols)
            for i, (name, _) in enumerate(SCALES):
                bx = pad + (i % cols) * bw
                by = y + (i // cols) * (bh + 4)
                self._button(bx, by, bx + bw - 4, by + bh, name, i == scale, True, "scale", i, tag="pitch")
            # Pitch Bend range: finger up/down, double tap back to +-2
            rows = (len(SCALES) + cols - 1) // cols
            by = y + rows * (bh + 4)
            bx0, bx1 = pad, pad + 2 * bw - 4
            self.bend_box = (bx0, by, bx1, by + bh)
            c.create_rectangle(bx0, by, bx1, by + bh, fill=BUTTON_OFF, outline="#505050", tags="pitch")
            if bend > 0:
                c.create_rectangle(bx0 + 1, by + 1, bx0 + 1 + bend / float(BEND_MAX) * (bx1 - bx0 - 2), by + bh - 1,
                                   fill="#2d6c99", width=0, tags="pitch")
            c.create_text(bx0 + 6, by + bh / 2, anchor="w", tags="pitch", font=(fam, fs),
                          fill=zynthian_gui_config.color_tx,
                          text=("Bend \u00b1%d" % bend) if bend else "Bend off")
            info = "Quantize %d %%   Pitch %+.1f   Spray %.1f" % (round(quant), pitch, spray)
            try:                                       # narrow screen: the short form
                if tkinter.font.Font(family=fam, size=fs).measure(info) > w - pad - bx1 - 8:
                    info = "Q %d%%  P %+.1f  S %.1f" % (round(quant), pitch, spray)
            except Exception:
                pass
            c.create_text(bx1 + 8, by + bh / 2, anchor="w", text=info, fill=zynthian_gui_config.color_tx_off,
                          font=(fam, fs), tags="pitch")
        # sounding notes: dots on their keys, redrawn only when they change
        notes = set()
        for i in range(MAX_VOICES):
            v = self._mon("note%d" % (i + 1), -1.0)
            if v >= 0:
                notes.add(int(round(v)) % 12)
        notes = frozenset(notes)
        if notes == self.pitch_notes:
            return
        self.pitch_notes = notes
        c.delete("pnotes")
        r = max(3, int(min(kw, ky1 - ky0) * 0.12))
        for pc in notes:
            cx = kx0 + (pc + 0.5) * kw
            cy = ky0 + (ky1 - ky0) * 0.3
            c.create_oval(cx - r, cy - r, cx + r, cy + r, fill=NOTE_COLOR, outline="", tags="pnotes")

    def _draw_title(self):
        title = self._page_title()
        if not getattr(self, "title_shown", True):
            title = ""                                    # no room beside the tabs
        if title == self.title:
            return
        self.title = title
        c = self.widget_canvas
        c.delete("title")
        c.create_text(max(self.width, 20) - 6, 2, anchor="ne", text=title, tags="title",
                      fill=zynthian_gui_config.color_tx_off,
                      font=(zynthian_gui_config.font_family, self._font_size()))

    def _draw_info(self, path):
        status = int(self._mon("status", 0))
        name = os.path.splitext(os.path.basename(path))[0] if path else ""
        if not path:
            text = "No file, tap to load"
        elif status == ST_LOADING:
            text = "%s  loading..." % name
        elif status == ST_ERROR:
            text = "%s  can't load" % name
        elif status == ST_LOADED:
            secs = self._mon("seconds", 0.0)
            ma, mb = self._markers()
            if ma > 0.0 or mb < 1.0:                  # markers moved: the part in use
                text = "%s  %.2f-%.2f of %.1f s" % (name, ma * secs, mb * secs, secs)
            else:
                text = "%s  %.1f s" % (name, secs)
        else:
            text = name
        if self.peaks_path != path and path:
            text += "  (reading wave)"
        if path and self._gain():
            text += "  %+.1f dB" % (self._gain() / 10.0)
        room = max(self.width, 80) - 16
        if (text, room) == self.info:
            return
        self.info = (text, room)
        try:                                           # long names: cut, the controls stay readable
            f = tkinter.font.Font(family=zynthian_gui_config.font_family, size=self._font_size())
            while len(text) > 4 and f.measure(text) > room:
                text = text[:-2] + "\u2026" if not text.endswith("\u2026") else text[:-2] + "\u2026"
        except Exception:
            pass
        c = self.widget_canvas
        c.delete("info")
        fs = self._font_size()
        y = self._area()[0] + 2                        # over the top left of the waveform, on a dark plate
        t = c.create_text(6, y, anchor="nw", text=text, tags="info",
                          fill=("#ff6b6b" if status == ST_ERROR else zynthian_gui_config.color_tx),
                          font=(zynthian_gui_config.font_family, fs))
        b = c.bbox(t)
        if b:
            plate = c.create_rectangle(b[0] - 3, b[1] - 1, b[2] + 3, b[3] + 1, fill=zynthian_gui_config.color_bg,
                                       outline="", tags="info")
            c.tag_lower(plate, t)
        c.tag_raise("info")
