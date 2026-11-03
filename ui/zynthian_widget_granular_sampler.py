#!/usr/bin/python3
# -*- coding: utf-8 -*-
# Granular Sampler widget. Stage 1c: the Wave view.
# - The sample path is the "sample" LV2 parameter (atom:Path, notOnGUI): a tap opens Zynthian's file selector.
# - The waveform is read from the file here, in a background thread, and drawn once as one polygon.
# - The plugin's own state comes from its output ports: status (0 empty, 1 loading, 2 loaded, 3 error), seconds.
# - At the top, in a small font: the name of the current knob page (screen_title of Zynthian's control screen).
import logging
import os
import threading
import tkinter
import tkinter.font

from zyngui import zynthian_gui_config
from zyngui import zynthian_widget_base

MULTI_INSTANCE = True

NPEAKS = 1024                   # min/max pairs over the whole file
ST_EMPTY, ST_LOADING, ST_LOADED, ST_ERROR = range(4)
WAVE_COLOR = "#4dabf7"


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
        self.widget_canvas = tkinter.Canvas(
            self, highlightthickness=0, relief='flat',
            bg=zynthian_gui_config.color_bg)
        self.widget_canvas.grid(sticky='news')
        self.widget_canvas.bind("<ButtonPress-1>", self._on_press)

    def on_size(self, event):
        if not super().on_size(event):
            return
        self.widget_canvas.grid(row=0, column=0, sticky='news')
        self.wave_size = None
        self.info = None
        self.title = None
        self.refresh_gui()

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
        try:
            f = tkinter.font.Font(family=zynthian_gui_config.font_family, size=self._font_size())
            return f.metrics("linespace") + 4
        except Exception:
            return 2 * self._font_size()

    def _font_size(self):
        return max(8, min(max(self.height, 20) // 14, 13))

    def _mon(self, key, default=0.0):
        try:
            return float((self.monitors or {}).get(key, default))
        except Exception:
            return default

    # ---------- touch ----------
    def _on_press(self, event):
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
        size = (self.width, self.height)
        if self.wave_size != size:
            self._draw_wave()
            self.wave_size = size
        self._draw_info(path)
        self._draw_title()

    def _draw_wave(self):
        c = self.widget_canvas
        c.delete("wave")
        w, h = max(self.width, 20), max(self.height, 20)
        top, bottom = self._header_h() + 2, h - max(16, h // 6)
        mid, half = (top + bottom) / 2.0, (bottom - top) / 2.0
        c.create_line(0, mid, w, mid, fill="#303030", tags="wave")
        p = self.peaks
        if not p:
            return
        # one value pair per screen column, then the outline as one polygon
        cols = max(2, int(w))
        hi, lo = [], []
        n = len(p)
        for x in range(cols):
            i0 = x * n // cols
            i1 = max(i0 + 1, (x + 1) * n // cols)
            seg = p[i0:i1]
            mn = min(s[0] for s in seg)
            mx = max(s[1] for s in seg)
            hi.extend((x, mid - max(mx, 0.0) * half))
            lo.extend((x, mid - min(mn, 0.0) * half))
        pts = hi
        for k in range(len(lo) - 2, -1, -2):
            pts.extend((lo[k], lo[k + 1]))
        c.create_polygon(pts, fill=WAVE_COLOR, outline=WAVE_COLOR, tags="wave")

    def _draw_title(self):
        title = self._page_title()
        if title == self.title:
            return
        self.title = title
        c = self.widget_canvas
        c.delete("title")
        c.create_text(6, 2, anchor="nw", text=title, tags="title",
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
