#!/usr/bin/python3
# -*- coding: utf-8 -*-
# Granular Sampler widget. Tabs (top left): Wave, Grain; Pitch, Filter and Mod come with their stages.
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
#   Size, Density and the mean overlap; Poly/Mono and the number of voices (LV2 parameters mode, voices).
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
TABS = ["Wave", "Grain"]
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
        self.buttons = []            # Grain tab: (x0, y0, x1, y1, symbol, value)
        self.widget_canvas = tkinter.Canvas(
            self, highlightthickness=0, relief='flat',
            bg=zynthian_gui_config.color_bg)
        self.widget_canvas.grid(sticky='news')
        self.widget_canvas.bind("<ButtonPress-1>", self._on_press)

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
                    self._clear()
                    self.refresh_gui()
            return
        if self.tab == 1:
            self._grain_press(event)
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
        self._draw_tabs()
        self._draw_title()
        if self.tab == 1:
            self._draw_grain()
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
        mode = int(round(self._zval("mode", 0)))
        voices = int(round(self._zval("voices", MAX_VOICES)))
        key = (round(shape * 30), round(size, 1), round(dens, 2), mode, voices, self.width, self.height)
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
        # Poly / Mono and voices
        self.buttons = []
        bh = max(fs * 2 + 6, (py1 - py0) // 5)
        y = py0
        c.create_text(right, y, anchor="nw", text="Mode", fill=tx_off, font=(fam, fs), tags="grain")
        y += fs * 2
        bw = (w - pad - right) / 2.0
        for i, name in enumerate(("Poly", "Mono")):
            x0 = right + i * bw
            self._button(x0, y, x0 + bw - 4, y + bh, name, mode == i, True, "mode", i)
        y += bh + fs
        c.create_text(right, y, anchor="nw", text="Voices", fill=(tx_off if mode else tx_off),
                      font=(fam, fs), tags="grain")
        y += fs * 2
        bw = (w - pad - right) / float(MAX_VOICES)
        for i in range(MAX_VOICES):
            x0 = right + i * bw
            self._button(x0, y, x0 + bw - 3, y + bh, str(i + 1), (i + 1) <= voices and not mode, not mode,
                         "voices", i + 1)

    def _button(self, x0, y0, x1, y1, text, on, enabled, sym, value):
        c = self.widget_canvas
        fill = BUTTON_ON if on else BUTTON_OFF
        c.create_rectangle(x0, y0, x1, y1, fill=fill, outline=("#505050" if enabled else "#303030"), tags="grain")
        c.create_text((x0 + x1) / 2, (y0 + y1) / 2, text=text, tags="grain",
                      fill=("#000000" if on else (zynthian_gui_config.color_tx if enabled else "#505050")),
                      font=(zynthian_gui_config.font_family, self._font_size()))
        if enabled:
            self.buttons.append((x0, y0, x1, y1, sym, value))

    def _grain_press(self, event):
        for x0, y0, x1, y1, sym, value in self.buttons:
            if x0 <= event.x <= x1 and y0 <= event.y <= y1:
                self._zset(sym, value)
                self.grain_view = None
                self.refresh_gui()
                return

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
