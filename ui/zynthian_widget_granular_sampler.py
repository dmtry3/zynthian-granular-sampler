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
#   a second tap on 1 makes it L = mono with legato (voices 0).
# Pitch:
# - One octave: notes of the Quantize scale lit, the tonic filled, sounding notes (outputs note1..8) dotted;
#   a tap on a key sets the tonic (LV2 parameter key); scale buttons set the scale (LV2 parameter scale).
# Filter:
# - The response of the voice filter for the knobs (Cutoff, Resonance, Type, Key Track at C4): the TPT SVF of
#   grain_core.c is the bilinear transform of the analog prototype, so |H| is the analog formula at
#   w = tan(pi f / sr) / tan(pi fc / sr).
# Mod:
# - The 7 x 8 grid (LV2 parameters m_<source>_<destination>, -100..100): a finger up/down changes a cell,
#   a double tap resets it; colour = sign, brightness = depth; sources in use are lit.
# - LFO 1/2: a tap changes the shape, Free/Key sync; Env 1/2 shape and Mod Depth: finger up/down.
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
ZONE_COLOR = "#1d2a38"
PLAYHEAD_COLOR = "#ffffff"
GRAIN_COLOR = "#ffd43b"
NGRAIN_MARKS = 16
TABS = ["Wave", "Grain", "Pitch", "Filter", "Mod"]
MOD_SRCS = [("lfo1", "LFO1"), ("lfo2", "LFO2"), ("env1", "Env1"), ("env2", "Env2"), ("vel", "Vel"),
            ("wheel", "Wheel"), ("rnd", "Rnd")]
MOD_DSTS = [("position", "Pos"), ("scan", "Scan"), ("size", "Size"), ("density", "Dens"),
            ("pitch", "Pitch"), ("pan", "Pan"), ("level", "Lvl"), ("cutoff", "Cut")]
LFO_SHAPES = ["Sine", "Tri", "Saw up", "Saw dn", "Square", "S&H", "Smooth"]
TAB_GROUPS = ["g_position", "g_grain", "g_pitch", "g_filter", "g_mod"]   # knob page (port group) of each tab
MOD_DSTS_SHORT = ["Pos", "Scn", "Siz", "Dns", "Pit", "Pan", "Lvl", "Cut"]   # when the full names do not fit
MOD_POS = (77, 171, 247)        # positive cells: blue
MOD_NEG = (255, 146, 43)        # negative cells: orange
DOUBLE_TAP = 0.35               # s
DRAG_PX = 1.5                   # px of finger movement per step (1 %)
FILTER_TYPES = ["LP", "HP", "BP"]
SR = 48000.0
NOTE_NAMES = ["C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"]
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


def filter_gain(f, fc, res, ftype, sr=SR):
    """|H| of the voice filter at f Hz (same k as grain_core.c: 1/Q from Butterworth to Q 20)."""
    import math
    top = 0.45 * sr
    fc = min(max(fc, 20.0), top)
    k = 1.4142136 * (1.0 - res) + 0.05 * res
    w = math.tan(math.pi * min(f, 0.499 * sr) / sr) / math.tan(math.pi * fc / sr)
    den = math.sqrt((1.0 - w * w) ** 2 + (k * w) ** 2)
    if ftype == 1:
        return w * w / den
    if ftype == 2:
        return w / den
    return 1.0 / den


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
        self.wave_size = None        # canvas size the waveform was drawn for
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
        self.drag = None             # (symbol, start value, start y, lo, hi) while a finger is down
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
        """Top and bottom of the waveform area (zone band and grain strip are just above it)."""
        h = max(self.height, 20)
        return self._header_h() + ZONE_BAND + GRAIN_STRIP + 4, h - max(16, h // 6)

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
        if self.tab in (1, 2):
            self._button_press(event)
            return
        if self.tab == 4:
            self._mod_press(event)
            return
        if self.tab != 0:
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
        if self.tab == 4:
            self._draw_mod()
            return
        size = (self.width, self.height)
        if self.wave_size != size:
            self._build_wave()
            self.wave_size = size
            self.wave_shown = False
        if not self.wave_shown:
            self.widget_canvas.delete("wave")
            self.widget_canvas.create_image(0, self._area()[0], anchor="nw", image=self.wave_img, tags="wave")
            self.wave_shown = True
            self.widget_canvas.tag_raise("zone")
            self.widget_canvas.tag_raise("marks")
        self._draw_zone()
        self._draw_marks()
        self._draw_info(path)

    def _build_wave(self):
        """The waveform image, built once per file and size (kept across tab switches)."""
        self.wave_img = None
        w = max(int(self.width), 20)
        top, bottom = self._area()
        hgt = max(int(bottom - top), 2)
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
                    mx = max(s[1] for s in seg)
                    mn = min(s[0] for s in seg)
                    y0 = int(mid - max(min(mx, 1.0), 0.0) * half)
                    y1 = max(int(mid - min(max(mn, -1.0), 0.0) * half), y0) + 1
                if run and (x == w or (y0, y1) != (run[1], run[2])):
                    img.put(WAVE_COLOR, to=(run[0], run[1], x, min(run[2], hgt)))
                    run = None
                if x < w and run is None:
                    run = [x, y0, y1]
        self.wave_img = img

    def _draw_zone(self):
        """Where the playhead loops: from Position, Window wide; wraps past the end like the engine.
        A band above the waveform and a line at Position (on top of the image: drawn once per change)."""
        pos = min(max(self._zval("position", 0.0) / 100.0, 0.0), 1.0)
        win = min(max(self._zval("window", 100.0) / 100.0, 0.001), 1.0)
        key = (pos, win, self.width, self.height, bool(self.peaks))
        if key == self.zone:
            return
        self.zone = key
        c = self.widget_canvas
        c.delete("zone")
        if not self.peaks:
            return
        w = max(self.width, 20)
        top, bottom = self._area()
        b1 = self._strip()[0] - 1
        b0 = b1 - ZONE_BAND
        x0, x1 = pos * w, (pos + win) * w
        c.create_rectangle(x0, b0, min(x1, w), b1, fill=ZONE_COLOR, width=0, tags="zone")
        if x1 > w:
            c.create_rectangle(0, b0, x1 - w, b1, fill=ZONE_COLOR, width=0, tags="zone")
        c.create_line(x0, b0, x0, bottom, fill="#5c7a99", tags="zone")      # Position
        c.tag_raise("marks")

    def _draw_marks(self):
        c = self.widget_canvas
        if self.marks is None:
            self.marks = [c.create_line(0, 0, 0, 0, fill=PLAYHEAD_COLOR, width=2, state="hidden", tags="marks")]
            for _ in range(NGRAIN_MARKS):
                self.marks.append(c.create_line(0, 0, 0, 0, fill=GRAIN_COLOR, width=1, state="hidden", tags="marks"))
            self.mark_state = [None] * len(self.marks)
        w = max(self.width, 20)
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

    def _draw_tabs(self):
        key = (self.tab, self.width, self.height)
        if key == self.tabs_drawn:
            return
        self.tabs_drawn = key
        c = self.widget_canvas
        c.delete("tabs")
        fs = self._font_size()
        hh = self._header_h()
        font = (zynthian_gui_config.font_family, fs)
        try:
            f = tkinter.font.Font(family=zynthian_gui_config.font_family, size=fs)
            widths = [f.measure(t) + 2 * fs for t in TABS]
        except Exception:
            widths = [len(t) * fs + 2 * fs for t in TABS]
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
        key = (round(shape * 30), round(size, 1), round(dens, 2), voices, self.width, self.height)
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
        for names in ([d[1] for d in MOD_DSTS], MOD_DSTS_SHORT):
            for size in range(max(6, fs - 4), 5, -1):
                try:
                    f = tkinter.font.Font(family=fam, size=size)
                    if max(f.measure(n) for n in names) <= cw - 3:
                        return size, names
                except Exception:
                    return max(6, fs - 4), names
        return 6, MOD_DSTS_SHORT

    def _mod_layout(self):
        w, h = max(self.width, 80), max(self.height, 60)
        pad = 6
        top = self._header_h() + 4
        fs = self._font_size()
        gx1 = int(w * 0.66)                              # grid on the left two thirds
        label_w = fs * 4.5
        head_h = fs * 2
        gx0 = pad + label_w
        gy0 = top + head_h
        gy1 = h - pad
        cw = (gx1 - gx0) / float(len(MOD_DSTS))
        ch = (gy1 - gy0) / float(len(MOD_SRCS))
        return w, h, pad, top, gx0, gy0, gx1, gy1, cw, ch

    def _cell_sym(self, si, di):
        return "m_%s_%s" % (MOD_SRCS[si][0], MOD_DSTS[di][0])

    def _side_boxes(self):
        """Right column: (symbol, kind, x0, y0, x1, y1, label); kind: shape, sync, drag"""
        w, h, pad, top, gx0, gy0, gx1, gy1, cw, ch = self._mod_layout()
        x0, x1 = gx1 + pad * 2, w - pad
        rows = [("lfo1_shape", "shape", "LFO 1"), ("lfo1_sync", "sync", ""),
                ("lfo2_shape", "shape", "LFO 2"), ("lfo2_sync", "sync", ""),
                ("env1_shape", "drag", "Env 1"), ("env2_shape", "drag", "Env 2"), ("mod_depth", "drag", "Depth")]
        rh = (h - pad - top) / 5.0
        out = []
        xm = x0 + (x1 - x0) * 0.62
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
                for di in range(len(MOD_DSTS)):
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
            if self.mod_vals.get((si, di)) == v:
                continue
            self.mod_vals[(si, di)] = v
            f = min(abs(v), 100) / 100.0
            c.itemconfigure(r, fill=mix(MOD_POS if v >= 0 else MOD_NEG, 0.25 + 0.75 * f if v else 0))
            c.itemconfigure(t, text=("%+d" % v) if v else "", fill=("#000000" if f > 0.5 else "#e0e0e0"))
        for si, item in enumerate(self.mod_labels):
            want = zynthian_gui_config.color_tx if used[si] else "#606060"
            if c.itemcget(item, "fill") != want:
                c.itemconfigure(item, fill=want)
        self._draw_mod_side()

    def _draw_mod_side(self):
        vals = tuple(int(round(self._zval(b[0], 0))) for b in self._side_boxes())
        key = (vals, self.width, self.height)
        if key == self.mod_side:
            return
        self.mod_side = key
        c = self.widget_canvas
        c.delete("modside")
        fam, fs = zynthian_gui_config.font_family, self._font_size()
        fsmall = max(7, fs - 2)
        tx, tx_off = zynthian_gui_config.color_tx, zynthian_gui_config.color_tx_off
        for (sym, kind, x0, y0, x1, y1, label), v in zip(self._side_boxes(), vals):
            c.create_rectangle(x0, y0, x1, y1, outline="#505050", fill=BUTTON_OFF, tags="modside")
            if kind == "sync":
                on = v == 1
                c.create_rectangle(x0, y0, x1, y1, outline="#505050", fill=(BUTTON_ON if on else BUTTON_OFF),
                                   tags="modside")
                c.create_text((x0 + x1) / 2, (y0 + y1) / 2, text=("Key" if on else "Free"), tags="modside",
                              fill=("#000000" if on else tx), font=(fam, fsmall))
                continue
            # label and value on top, a picture of the shape below
            if kind == "shape":
                text = "%s %s" % (label, LFO_SHAPES[min(max(v, 0), len(LFO_SHAPES) - 1)])
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
            di = min(int((event.x - gx0) / cw), len(MOD_DSTS) - 1)
            si = min(int((event.y - gy0) / ch), len(MOD_SRCS) - 1)
            sym, lo, hi = self._cell_sym(si, di), -100, 100
        else:
            for bsym, kind, x0, y0, x1, y1, _ in self._side_boxes():
                if x0 <= event.x <= x1 and y0 <= event.y <= y1:
                    if kind == "shape":
                        self._zset(bsym, (int(round(self._zval(bsym, 0))) + 1) % len(LFO_SHAPES))
                    elif kind == "sync":
                        self._zset(bsym, 1 - int(round(self._zval(bsym, 0))))
                    else:
                        sym = bsym
                    break
            if sym is None:
                self.refresh_gui()
                return
        if self.last_tap[0] == sym and now - self.last_tap[1] < DOUBLE_TAP:
            default = 100 if sym == "mod_depth" else (10 if sym == "env1_shape" else (50 if sym == "env2_shape" else 0))
            self._zset(sym, default)                      # double tap: back to the default
            self.last_tap = (None, 0.0)
            self.drag = None
            self.refresh_gui()
            return
        self.last_tap = (sym, now)
        self.drag = (sym, int(round(self._zval(sym, 0))), event.y, lo, hi)

    def _on_motion(self, event):
        if not self.drag or self.tab != 4:
            return
        sym, start, y0, lo, hi = self.drag
        v = int(min(max(start + (y0 - event.y) / DRAG_PX, lo), hi))
        if abs(event.y - y0) > 6:
            self.last_tap = (None, 0.0)                   # a drag is not a tap
        self._zset(sym, v)
        self.refresh_gui()

    def _on_release(self, event):
        self.drag = None

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
        ftype = min(max(int(round(self._zval("ftype", 0))), 0), 2)
        kt = self._zval("keytrack", 0.0)
        view = (round(fc), round(res * 100), ftype, round(kt), self.width, self.height)
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
        open_lp = ftype == 0 and fc >= 20000 * 0.97 and res < 0.01
        open_hp = ftype == 1 and fc <= 21.0 and res < 0.01
        pts = []
        n = max(64, int(x1 - x0) // 3)
        for i in range(n + 1):
            f = 20.0 * 10 ** (3.0 * i / n)
            g = 1.0 if (open_lp or open_hp) else filter_gain(f, fc, res, ftype)
            pts.extend((self._fx(f, x0, x1), self._dby(20 * math.log10(max(g, 1e-9)), y0, y1)))
        c.create_line(pts, fill=WAVE_COLOR, width=2, tags="filter")
        xc = self._fx(min(max(fc, 20.0), 20000.0), x0, x1)
        c.create_line(xc, y0, xc, y1, fill="#5c7a99", dash=(3, 3), tags="filter")
        fct = ("%.1f kHz" % (fc / 1000.0)) if fc >= 1000 else ("%d Hz" % round(fc))
        info = "%s  %s  Res %d %%  Key %d %%%s" % (FILTER_TYPES[ftype], fct, round(res * 100), round(kt),
                                                 "  (off)" if (open_lp or open_hp) else "")
        c.create_text(x0, max(self.height, 60) - 1, anchor="sw", text=info, fill=tx_off,
                      font=(fam, fs), tags="filter")

    # ---------- Pitch tab ----------
    def _pitch_layout(self):
        w, h = max(self.width, 80), max(self.height, 60)
        pad = 8
        top = self._header_h() + 6
        kb = (pad, top, w - pad, top + int((h - top) * 0.38))
        return w, h, pad, kb

    def _draw_pitch(self):
        scale = min(max(int(round(self._zval("scale", 0))), 0), len(SCALES) - 1)
        key = min(max(int(round(self._zval("key", 0))), 0), 11)
        quant = self._zval("quantize", 0.0)
        pitch = self._zval("pitch", 0.0)
        spray = self._zval("pitch_spray", 0.0)
        view = (scale, key, round(quant), round(pitch, 1), round(spray, 1), self.width, self.height)
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
            y = ky1 + fs
            cols = 4
            bw = (w - 2 * pad) / float(cols)
            for i, (name, _) in enumerate(SCALES):
                bx = pad + (i % cols) * bw
                by = y + (i // cols) * (bh + 4)
                self._button(bx, by, bx + bw - 4, by + bh, name, i == scale, True, "scale", i, tag="pitch")
            info = "%s %s   Quantize %d %%   Pitch %+.1f   Spray %.1f" % (
                NOTE_NAMES[key], SCALES[scale][0], round(quant), pitch, spray)
            c.create_text(pad, h - 3, anchor="sw", text=info, fill=zynthian_gui_config.color_tx_off,
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
            text = "No sample - tap to load"
        elif status == ST_LOADING:
            text = "%s  loading..." % name
        elif status == ST_ERROR:
            text = "%s  can't load" % name
        elif status == ST_LOADED:
            text = "%s  %.1f s" % (name, self._mon("seconds", 0.0))
        else:
            text = name
        if self.peaks_path != path and path:
            text += "  (reading wave)"
        if text == self.info:
            return
        self.info = text
        c = self.widget_canvas
        c.delete("info")
        h = max(self.height, 20)
        fs = max(8, min(h // 12, 16))
        c.create_text(6, h - 3, anchor="sw", text=text, tags="info",
                      fill=("#ff6b6b" if status == ST_ERROR else zynthian_gui_config.color_tx),
                      font=(zynthian_gui_config.font_family, fs))
