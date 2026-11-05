#!/usr/bin/python3
# Runs the Granular Sampler widget outside Zynthian with stub modules (same approach as Generative Arp).
#   python3 test/widget_preview.py          scripted tests (PASS/FAIL)
#   python3 test/widget_preview.py --show   open a window: tap = "file selector" cycles through the samples
import importlib.util
import os
import sys
import time
import tkinter
import types

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
PAD = os.path.join(ROOT, "samples", "cello_d2.wav")

# ---------- stub zyngui modules ----------
cfg = types.ModuleType("zyngui.zynthian_gui_config")
cfg.font_family = "DejaVu Sans"
cfg.color_bg = "#000000"
cfg.color_tx = "#ffffff"
cfg.color_tx_off = "#707070"
cfg.color_hl = "#08a2c0"
cfg.zyngui = None


class _WidgetBase(tkinter.Frame):          # same contract as zynthian_widget_base
    def __init__(self, parent):
        super().__init__(parent.main_frame, bg=cfg.color_bg)
        self.zyngui = cfg.zyngui
        self.zyngui_control = parent
        self.width = 1
        self.height = 1
        self.wide = parent.wide
        self.rows = parent.layout['rows']
        self.shown = False
        self.processor = None
        self.widget_canvas = None
        self.monitors = None
        self.bind('<Configure>', self.on_size)

    def on_size(self, event):
        if event.width == self.width and event.height == self.height:
            return False
        self.width, self.height = event.width, event.height
        try:
            self.widget_canvas.configure(width=self.width, height=self.height)
        except Exception:
            pass
        return True

    def update(self):
        self.monitors = self.processor.engine.get_monitors_dict()
        self.refresh_gui()

    def set_processor(self, processor):
        self.processor = processor


base = types.ModuleType("zyngui.zynthian_widget_base")
base.zynthian_widget_base = _WidgetBase
pkg = types.ModuleType("zyngui")
pkg.zynthian_gui_config, pkg.zynthian_widget_base = cfg, base
sys.modules.update({"zyngui": pkg, "zyngui.zynthian_gui_config": cfg, "zyngui.zynthian_widget_base": base})


class PathCtrl:
    """A path controller: nudge() opens the file selector in Zynthian, here it calls on_select."""
    def __init__(self, sym):
        self.symbol, self.value, self.is_path = sym, "", True
        self.nudges = 0
        self.on_select = None

    def nudge(self, val, send=True, fine=False):
        self.nudges += 1
        if self.on_select:
            self.on_select(self)
        return True

    def set_value(self, v, send=True):
        self.value = str(v)


class NumCtrl:
    def __init__(self, sym, value):
        self.symbol, self.value = sym, value
        self.writes = 0

    def set_value(self, v, send=True):
        self.value = v
        self.writes += 1


class Engine:
    def __init__(self):
        self.status = 0
        self.seconds = 0.0
        self.playhead = -1.0
        self.grains = []
        self.notes = []
        self.detected = -1.0
        self.peak = -100.0

    def get_monitors_dict(self):
        m = {"status": float(self.status), "seconds": float(self.seconds), "playhead": float(self.playhead),
             "detected": float(self.detected), "peak": float(self.peak)}
        for i in range(16):
            m["grain%d" % (i + 1)] = float(self.grains[i]) if i < len(self.grains) else -1.0
        for i in range(8):
            m["note%d" % (i + 1)] = float(self.notes[i]) if i < len(self.notes) else -1.0
        return m


class Processor:
    def __init__(self):
        self.engine = Engine()
        self.id = 1
        self.controllers_dict = {"sample": PathCtrl("sample"),
                                 "position": NumCtrl("position", 0.0), "window": NumCtrl("window", 100.0),
                                 "shape": NumCtrl("shape", 67.0), "size": NumCtrl("size", 100.0),
                                 "density": NumCtrl("density", 20.0),
                                 "voices": NumCtrl("voices", 8), "glide": NumCtrl("glide", 0),
                                 "bend_range": NumCtrl("bend_range", 2),
                                 "root": NumCtrl("root", 60), "tune": NumCtrl("tune", 0),
                                 "gain": NumCtrl("gain", 0),
                                 "mark_start": NumCtrl("mark_start", 0), "mark_end": NumCtrl("mark_end", 10000),
                                 "scale": NumCtrl("scale", 0), "key": NumCtrl("key", 0),
                                 "quantize": NumCtrl("quantize", 0.0), "pitch": NumCtrl("pitch", 0.0),
                                 "pitch_spray": NumCtrl("pitch_spray", 0.0),
                                 "cutoff": NumCtrl("cutoff", 20000.0), "resonance": NumCtrl("resonance", 0.0),
                                 "ftype": NumCtrl("ftype", 0), "keytrack": NumCtrl("keytrack", 0.0)}
        for sym, v in (("lfo1_shape", 0), ("lfo2_shape", 1), ("lfo1_sync", 0), ("lfo2_sync", 0),
                       ("lfo1_rate", 61), ("lfo2_rate", 42), ("lfo1_uni", 0), ("lfo2_uni", 0),
                       ("env1_shape", 10), ("env2_shape", 50), ("mod_depth", 100), ("filter_mix", 100), ("vowel_lows", 25), ("vowel_highs", 50),
                       ("filter_slope", 0)):
            self.controllers_dict[sym] = NumCtrl(sym, v)
        for a in ("lfo1", "lfo2", "env1", "env2", "vel", "wheel", "rnd"):
            for b in ("position", "scan", "spray", "size", "density", "pitch", "pan", "level", "cutoff"):
                self.controllers_dict["m_%s_%s" % (a, b)] = NumCtrl("m_%s_%s" % (a, b), 0)
            for b in ("lfo1_rate", "lfo2_rate", "lfo1_amt", "lfo2_amt", "env1_time", "env2_time"):
                self.controllers_dict["x_%s_%s" % (a, b)] = NumCtrl("x_%s_%s" % (a, b), 0)

    def v(self, sym):
        return self.controllers_dict[sym].value


class Control:
    """Zynthian's control screen: list_data rows (group symbol, index, title, processor, page)."""
    def __init__(self, root):
        self.main_frame = root
        self.layout = {"rows": 4}
        self.screen_title = "Voice"
        self.list_data = []
        self.screen_info = None
        self.selected = []
        self.wide = True
        self.shown = True

    def add_pages(self, proc):
        self.list_data = [(None, None, "> Granular Sampler", proc)]
        for j, (g, t) in enumerate((("g_position", "Position"), ("g_grain", "Grain"), ("g_pitch", "Pitch"),
                                    ("g_voice", "Voice"), ("g_filter", "Filter"), ("g_mod", "Mod"))):
            self.list_data.append((g, j + 1, t, proc, j))

    def select(self, index=None, set_zctrl=True):
        self.selected.append(index)
        self.screen_info = self.list_data[index]
        self.screen_title = self.screen_info[2]


def load_widget():
    path = os.path.join(ROOT, "ui", "zynthian_widget_granular_sampler.py")
    spec = importlib.util.spec_from_file_location("zynthian_widget_granular_sampler", path)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def main():
    show = "--show" in sys.argv
    if not os.path.exists(PAD):
        print("Missing %s" % PAD)
        return 2
    mod = load_widget()
    throttle = mod.UPDATE_PERIOD
    if not show:
        mod.UPDATE_PERIOD = 0.0          # scripted checks see every update; the throttle has its own check
    root = tkinter.Tk()
    root.geometry("480x300")
    proc = Processor()
    wdg = mod.zynthian_widget_granular_sampler(Control(root))
    wdg.set_processor(proc)
    wdg.pack(fill="both", expand=True)
    root.update()
    wdg.update()
    root.update()
    c = wdg.widget_canvas
    sample = proc.controllers_dict["sample"]

    def pump(cond, secs=10.0):
        """Run the GUI loop like Zynthian does (update() every few ms) until cond() or timeout."""
        t = time.monotonic()
        while time.monotonic() - t < secs:
            wdg.update()
            root.update()
            if cond():
                return True
            time.sleep(0.01)
        return False

    def wave_drawn():
        """Waveform pixels in the image: the wave colour somewhere just above the middle line."""
        imgs = [i for i in c.find_withtag("wave") if c.type(i) == "image"]
        if len(imgs) != 1 or wdg.wave_img is None:
            return False
        img = wdg.wave_img
        wc = tuple(int(mod.WAVE_COLOR[k:k + 2], 16) for k in (1, 3, 5))
        y = img.height() // 2 - 2
        return any(tuple(img.get(x, y)) == wc for x in range(0, img.width(), 7))

    def info():
        items = [i for i in c.find_withtag("info") if c.type(i) == "text"]
        return c.itemcget(items[0], "text") if items else ""

    if show:
        files = [PAD, "", "/no/such/file.wav"]

        def select(z):                    # each tap: next file, the "plugin" loads it after 0.5 s
            z.value = files[(files.index(z.value) + 1) % len(files)] if z.value in files else files[0]
            proc.engine.status = 1 if z.value else 0

            def done():
                ok = os.path.exists(z.value)
                proc.engine.status = (2 if ok else 3) if z.value else 0
                proc.engine.seconds = 6.0 if ok else 0.0
            root.after(500, done)
        sample.on_select = select

        import random

        def tick():                       # a fake voice: playhead runs, grains around it
            if proc.engine.status == 2:
                pos = proc.controllers_dict["position"].value / 100.0
                win = proc.controllers_dict["window"].value / 100.0
                ph = proc.engine.playhead if proc.engine.playhead >= 0 else pos
                ph = pos + ((ph - pos + 0.005) % max(win, 0.001))
                proc.engine.playhead = ph % 1.0
                proc.engine.grains = [(ph + random.uniform(-0.03, 0.03)) % 1.0 for _ in range(10)]
            wdg.update()
            root.after(50, tick)
        proc.controllers_dict["position"].value = 30.0
        proc.controllers_dict["window"].value = 50.0
        tick()
        root.mainloop()
        return 0

    fails = 0

    def check(name, cond):
        nonlocal fails
        print("%s: %s" % ("PASS" if cond else "FAIL", name))
        fails += 0 if cond else 1

    check("widget sized to the window", wdg.width >= 400 and wdg.height >= 250)

    def title():
        items = c.find_withtag("title")
        return c.itemcget(items[0], "text") if items else ""
    check("knob page name at the top", title() == "Voice")
    wdg.zyngui_control.screen_title = "Position"
    pump(lambda: False, 0.05)
    check("page name follows the knob page", title() == "Position")
    title_items = c.find_withtag("title")
    pump(lambda: False, 0.1)
    check("page name not redrawn when unchanged", c.find_withtag("title") == title_items)
    x = c.coords(c.find_withtag("title")[0])[0]
    check("page name at the right edge", x > wdg.width - 20 and c.itemcget(c.find_withtag("title")[0], "anchor") == "ne")
    check("empty: hint to tap", "tap to load" in info())
    check("empty: no waveform drawn", not wave_drawn())

    wdg._on_press(None)
    check("tap opens the file selector (nudge on the path controller)", sample.nudges == 1)

    # a file is chosen; the plugin is still loading it
    sample.value = PAD
    proc.engine.status = 1
    got = pump(lambda: wdg.peaks_path == PAD)
    check("waveform read in the background", got and mod.NPEAKS - 8 <= len(wdg.peaks) <= mod.NPEAKS)
    check("waveform drawn into one image", wave_drawn())
    polys = [i for i in c.find_withtag("wave") if c.type(i) == "image"]
    check("status from the plugin: loading", "loading" in info() and "cello_d2" in info())
    lo = min(p[0] for p in wdg.peaks)
    hi = max(p[1] for p in wdg.peaks)
    check("peaks look like the cello (peak 0.23)", -0.26 < lo < -0.1 and 0.1 < hi < 0.26)

    proc.engine.status, proc.engine.seconds = 2, 4.91
    pump(lambda: False, 0.05)
    check("status from the plugin: loaded, length", info() == "cello_d2  4.9 s")

    # zone, playhead and grain marks
    proc.controllers_dict["position"].value = 25.0
    proc.controllers_dict["window"].value = 50.0
    proc.engine.playhead = 0.5
    proc.engine.grains = [0.4, 0.6]
    pump(lambda: False, 0.05)
    zones = [i for i in c.find_withtag("zone") if c.type(i) == "rectangle"]
    W = wdg._wave_w()                                # the gain slider takes the right edge
    check("window zone from Position 25 % to 75 %", len(zones) == 1 and
          abs(c.coords(zones[0])[0] - 0.25 * W) < 1 and abs(c.coords(zones[0])[2] - 0.75 * W) < 1)
    marks = wdg.marks
    shown = [m for m in marks if c.itemcget(m, "state") != "hidden"]
    check("playhead and 2 grain marks shown", len(shown) == 3 and abs(c.coords(marks[0])[0] - 0.5 * W) < 1)
    check("marks above the waveform", c.find_all().index(marks[0]) > c.find_all().index(polys[0]) if
          polys[0] in c.find_all() else False)
    s0, s1 = wdg._strip()
    check("grain marks are ticks in the strip, playhead full height",
          c.coords(marks[1])[1] == s0 and c.coords(marks[1])[3] == s1 and c.coords(marks[0])[3] > s1 + 50)
    # unchanged marks: no canvas calls at all (checked by counting coords() calls)
    calls = []
    orig = c.coords
    c.coords = lambda *a: calls.append(a) or orig(*a)
    pump(lambda: False, 0.05)
    c.coords = orig
    check("unchanged marks are not touched", calls == [])
    proc.controllers_dict["position"].value = 80.0                     # 80 % + 50 % wraps past the end
    proc.engine.grains = []
    pump(lambda: False, 0.05)
    zones = [i for i in c.find_withtag("zone") if c.type(i) == "rectangle"]
    check("zone past the end wraps to the start", len(zones) == 2)
    check("marks reused, not recreated", wdg.marks is marks and
          len([m for m in marks if c.itemcget(m, "state") != "hidden"]) == 1)
    proc.controllers_dict["position"].value = 0.0
    proc.controllers_dict["window"].value = 100.0
    proc.engine.playhead = -1.0
    pump(lambda: False, 0.05)

    # redraws do not rebuild the waveform (it is drawn once per file and size)
    before = c.find_withtag("wave")
    pump(lambda: False, 0.2)
    check("waveform not redrawn on every update", c.find_withtag("wave") == before)

    class Tap:
        def __init__(self, x, y):
            self.x, self.y = x, y

    # markers: drag left/right near a marker line; what is cut away is shaded; Position lives between them
    W = wdg._wave_w()
    top, bottom = wdg._area()
    my = (top + bottom) // 2
    nudges0 = sample.nudges
    proc.controllers_dict["position"].value = 0.0
    proc.controllers_dict["window"].value = 100.0
    wdg._on_press(Tap(2, my)); wdg._on_motion(Tap(2 + W // 4, my)); wdg._on_release(None)
    check("drag the start marker right a quarter: 25 %", abs(proc.v("mark_start") - 2500) <= 20 and
          sample.nudges == nudges0)
    time.sleep(mod.DOUBLE_TAP + 0.02)
    wdg._on_press(Tap(W - 2, my)); wdg._on_motion(Tap(W / 2.0 - 2, my)); wdg._on_release(None)
    check("drag the end marker left to the middle: 50 %", abs(proc.v("mark_end") - 5000) <= 20)
    pump(lambda: False, 0.1)
    shades = [i for i in c.find_withtag("markers") if c.type(i) == "rectangle"]
    check("both cut-away parts shaded", len(shades) == 2)
    zones = [i for i in c.find_withtag("zone") if c.type(i) == "rectangle"]
    check("Position/Window zone between the markers", len(zones) == 1 and
          abs(c.coords(zones[0])[0] - 0.25 * W) < 2 and abs(c.coords(zones[0])[2] - 0.5 * W) < 2)
    check("info shows the part in use", "-" in info() and "of" in info())
    time.sleep(mod.DOUBLE_TAP + 0.02)
    x = int(0.25 * W)
    wdg._on_press(Tap(x, my)); wdg._on_motion(Tap(W, my)); wdg._on_release(None)
    check("the start marker stops before the end marker", proc.v("mark_start") < proc.v("mark_end"))
    time.sleep(mod.DOUBLE_TAP + 0.02)
    wdg._on_press(Tap(int(0.1 * W), my))
    check("a tap away from the markers opens the file selector", sample.nudges == nudges0 + 1)
    wdg._on_release(None)
    for sym, xm in (("mark_start", proc.v("mark_start") / 10000.0), ("mark_end", 0.5)):
        time.sleep(mod.DOUBLE_TAP + 0.02)
        a_, b_ = wdg._markers()
        xx = int((a_ if sym == "mark_start" else b_) * W)
        for _ in range(2):
            wdg._on_press(Tap(xx, my)); wdg._on_release(None)
    check("double tap on each marker: back to the whole file", proc.v("mark_start") == 0 and
          proc.v("mark_end") == 10000)
    pump(lambda: False, 0.1)

    # sample gain: slider at the right of the waveform; double tap = normalize; the waveform follows
    gb = wdg._gain_box()
    check("gain slider right of the waveform", gb[0] >= wdg._wave_w() and gb[2] <= wdg.width and
          gb[3] <= wdg._area()[1])
    nudges0 = sample.nudges
    img0 = wdg.wave_img
    gx, gy = (gb[0] + gb[2]) // 2, (gb[1] + gb[3]) // 2
    wdg._on_press(Tap(gx, gy)); wdg._on_motion(Tap(gx, gb[1])); wdg._on_release(None)
    pump(lambda: False, 0.3)
    check("drag to the top of the gain slider: +24 dB (about)", proc.v("gain") >= 230 and sample.nudges == nudges0)
    check("waveform redrawn with the gain", wdg.wave_img is not img0)
    clip = tuple(int(mod.CLIP_COLOR[k:k + 2], 16) for k in (1, 3, 5))
    img = wdg.wave_img
    check("+24 dB: clipping shown in red", any(tuple(img.get(x, 1)) == clip for x in range(0, img.width(), 3)))
    check("info shows the gain", "+2" in info() and "dB" in info())
    time.sleep(mod.DOUBLE_TAP + 0.02)
    wdg._on_press(Tap(gx, gy)); wdg._on_release(None)
    wdg._on_press(Tap(gx, gy)); wdg._on_release(None)
    check("double tap without a known peak: unchanged", proc.v("gain") >= 230)
    proc.engine.peak = -12.6                          # cello_d2
    pump(lambda: False, 0.1)
    norm_ticks = [i for i in c.find_withtag("gain") if c.type(i) == "line" and c.itemcget(i, "fill") == mod.NORM_COLOR]
    check("the normalize point is marked", len(norm_ticks) == 1)
    time.sleep(mod.DOUBLE_TAP + 0.02)
    wdg._on_press(Tap(gx, gy)); wdg._on_release(None)
    wdg._on_press(Tap(gx, gy)); wdg._on_release(None)
    check("double tap: normalize, the peak at -1 dB (gain +11.6)", proc.v("gain") == 116)
    proc.controllers_dict["gain"].value = 0
    proc.engine.peak = -100.0
    pump(lambda: False, 0.3)

    # resize redraws it
    root.geometry("360x200")
    pump(lambda: wdg.width < 400, 2)
    check("waveform redrawn after resize", c.find_withtag("wave") != before and wave_drawn() and
          wdg.wave_img.width() == wdg._wave_w())
    check("360x200: all six tabs fit (%d px)" % wdg.tab_boxes[-1][1], wdg.tab_boxes[-1][1] <= wdg.width)

    # plugin error, file the widget can't read
    sample.value = "/no/such/file.wav"
    proc.engine.status = 3
    pump(lambda: wdg.peaks_path == "/no/such/file.wav")
    check("missing file: error shown, no waveform", "can't load" in info() and not wave_drawn())

    # Wave tab bottom row: Root, Tune, Auto
    check("Wave tab: Root, Tune and Auto in the bottom row", set(wdg.wave_boxes) == {"root", "tune", "auto"} and
          all(b[1] >= wdg._area()[1] and b[3] <= wdg.height and b[2] <= wdg.width for b in wdg.wave_boxes.values()))
    nudges0 = sample.nudges
    rb = wdg.wave_boxes["root"]
    wdg._on_press(Tap((rb[0] + rb[2]) // 2, (rb[1] + rb[3]) // 2))
    wdg._on_motion(Tap((rb[0] + rb[2]) // 2 - 16, (rb[1] + rb[3]) // 2))
    wdg._on_release(None)
    check("drag left on Root: C4 -> A#3", proc.v("root") == 58 and sample.nudges == nudges0)
    wdg.last_tap = (None, 0.0)
    for _ in range(2):
        wdg._on_press(Tap((rb[0] + rb[2]) // 2, (rb[1] + rb[3]) // 2))
        wdg._on_release(None)
    check("double tap on Root: back to C4", proc.v("root") == 60)
    ab = wdg.wave_boxes["auto"]
    wdg._on_press(Tap((ab[0] + ab[2]) // 2, (ab[1] + ab[3]) // 2))
    check("Auto without a detected pitch: nothing changes", proc.v("root") == 60 and sample.nudges == nudges0)
    proc.engine.detected = 37.93                       # cello D2, 7 cents flat
    pump(lambda: False, 0.1)
    texts = [c.itemcget(i, "text") for i in c.find_withtag("wctl") if c.type(i) == "text"]
    check("Auto shows the detected note", "Auto D2" in texts)
    wdg._on_press(Tap((ab[0] + ab[2]) // 2, (ab[1] + ab[3]) // 2))
    pump(lambda: False, 0.1)
    texts = [c.itemcget(i, "text") for i in c.find_withtag("wctl") if c.type(i) == "text"]
    check("tap on Auto: Root D2, Tune -7", proc.v("root") == 38 and proc.v("tune") == -7 and
          "Root D2" in texts and "-7c" in texts)
    wdg._on_press(Tap(5, (ab[1] + ab[3]) // 2))
    check("a tap left in the bottom row does not open the file selector", sample.nudges == nudges0)
    proc.controllers_dict["root"].value = 60
    proc.controllers_dict["tune"].value = 0
    proc.engine.detected = -1.0

    # clear: Zynthian sends "_"
    sample.value = "_"
    proc.engine.status = 0
    pump(lambda: False, 0.1)
    check("cleared ('_'): empty again", "tap to load" in info())

    # quick change while reading: the widget ends up with the last file
    sample.value = PAD
    pump(lambda: False, 0.0)
    sample.value = "/no/such/file.wav"
    pump(lambda: wdg.peaks_path == "/no/such/file.wav")
    check("quick change: the last file wins", wdg.peaks == [] and wdg.loading_path is None)

    # ---------- tabs ----------
    sample.value = PAD
    proc.engine.status = 2
    pump(lambda: wdg.peaks_path == PAD)
    img_before = wdg.wave_img
    check("six tabs: Wave, Grain, Pitch, Filter, Mod, X-Mod", len(wdg.tab_boxes) == 6 and wdg.tab == 0)
    nudges = sample.nudges
    hh = wdg._header_h()
    x0, x1 = wdg.tab_boxes[1]
    wdg._on_press(Tap((x0 + x1) // 2, hh // 2))
    pump(lambda: False, 0.05)
    check("tap on the Grain tab switches to it, not the file selector",
          wdg.tab == 1 and sample.nudges == nudges)
    check("Grain tab: no waveform and no marks", not c.find_withtag("wave") and not c.find_withtag("marks"))
    lines = [i for i in c.find_withtag("grain") if c.type(i) == "line"]
    check("Grain tab: window shape curve", len(lines) == 1 and len(c.coords(lines[0])) == 2 * 65)

    def curve_mid_y():
        ln = [i for i in c.find_withtag("grain") if c.type(i) == "line"][0]
        co = c.coords(ln)
        return co[65], co[1]            # y at x = 0.5, y at x = 0
    bell_mid, _ = curve_mid_y()
    proc.controllers_dict["shape"].value = 0.0           # square
    pump(lambda: False, 0.05)
    sq_mid, sq_start = curve_mid_y()
    proc.controllers_dict["shape"].value = 100.0 / 3     # decay: low in the middle
    pump(lambda: False, 0.05)
    dc_mid, _ = curve_mid_y()
    check("shape curve follows the Shape knob (square high, decay low in the middle)",
          abs(sq_mid - bell_mid) < 2 and dc_mid > bell_mid + 20 and sq_start > sq_mid)

    def press_button(sym, value):
        for bx0, by0, bx1, by1, s_, v_ in wdg.buttons:
            if s_ == sym and v_ == value:
                wdg._on_press(Tap((bx0 + bx1) / 2, (by0 + by1) / 2))
                return True
        return False
    check("voices buttons 1..8, no mode buttons", len([b for b in wdg.buttons if b[4] == "voices"]) == 8 and
          not [b for b in wdg.buttons if b[4] == "mode"])
    b0 = wdg.buttons[0]
    check("buttons are low: about one text line (%d px)" % (b0[3] - b0[1]),
          b0[3] - b0[1] <= wdg._header_h() * 1.5)
    press_button("voices", 4)
    check("tap on voices 4 sets voices = 4", proc.v("voices") == 4)
    def tap_first_voice():
        bx0, by0, bx1, by1 = [b for b in wdg.buttons if b[4] == "voices"][0][:4]
        wdg._on_press(Tap((bx0 + bx1) / 2, (by0 + by1) / 2))

    def first_label():
        texts = [i for i in c.find_withtag("grain") if c.type(i) == "text"]
        bx0, by0, bx1, by1 = [b for b in wdg.buttons if b[4] == "voices"][0][:4]
        for t in texts:
            x, y = c.coords(t)
            if bx0 <= x <= bx1 and by0 <= y <= by1:
                return c.itemcget(t, "text")
        return None
    tap_first_voice()
    check("tap on 1 (from 4) sets one voice", proc.v("voices") == 1 and first_label() == "1")
    tap_first_voice()
    check("tap on 1 again: L = legato (voices 0)", proc.v("voices") == 0 and first_label() == "L")
    tap_first_voice()
    check("tap on L: back to 1", proc.v("voices") == 1 and first_label() == "1")
    # Glide: only in mono, finger up/down, double tap = off
    check("with 1 voice (mono) Glide is active", wdg.glide_box is not None)
    gb = wdg.glide_box
    gx, gy = (gb[0] + gb[2]) / 2, (gb[1] + gb[3]) / 2
    gw = gb[2] - gb[0]
    wdg._on_press(Tap(gx, gy)); wdg._on_motion(Tap(gx + gw / 4.0, gy - 30)); wdg._on_release(None)
    check("Glide: finger right a quarter of the bar = 500 ms (up/down ignored)", abs(proc.v("glide") - 500) <= 10)
    time.sleep(mod.DOUBLE_TAP + 0.02)
    wdg._on_press(Tap(gx, gy)); wdg._on_motion(Tap(gx - gw / 8.0, gy)); wdg._on_release(None)
    check("Glide: finger left = shorter", abs(proc.v("glide") - 250) <= 10)
    time.sleep(mod.DOUBLE_TAP + 0.02)
    wdg._on_press(Tap(gx, gy)); wdg._on_motion(Tap(gx + 5000, gy)); wdg._on_release(None)
    check("Glide clamped at 2000 ms", proc.v("glide") == 2000)
    time.sleep(mod.DOUBLE_TAP + 0.02)
    wdg._on_press(Tap(gx, gy)); wdg._on_release(None)
    wdg._on_press(Tap(gx, gy)); wdg._on_release(None)
    check("Glide: double tap = off", proc.v("glide") == 0)
    time.sleep(mod.DOUBLE_TAP + 0.02)
    tap_first_voice()
    press_button("voices", 8)
    check("from L to 8 voices", proc.v("voices") == 8 and first_label() == "1")
    check("with 8 voices Glide is inactive", wdg.glide_box is None)
    writes = proc.controllers_dict["glide"].writes
    wdg._on_press(Tap(gx, gy)); wdg._on_motion(Tap(gx, gy - 30)); wdg._on_release(None)
    check("a drag on the inactive Glide does nothing", proc.controllers_dict["glide"].writes == writes)
    items = c.find_withtag("grain")
    pump(lambda: False, 0.1)
    check("Grain tab not redrawn when nothing changed", c.find_withtag("grain") == items)

    # Pitch tab
    x0, x1 = wdg.tab_boxes[2]
    wdg._on_press(Tap((x0 + x1) // 2, hh // 2))
    pump(lambda: False, 0.05)
    check("Pitch tab: 12 keys and 12 scale buttons", wdg.tab == 2 and
          len([b for b in wdg.buttons if b[4] == "key"]) == 12 and
          len([b for b in wdg.buttons if b[4] == "scale"]) == 12)

    def lit_keys():
        """keys drawn as in the scale (any fill but the dark 'off' ones)"""
        rects = [i for i in c.find_withtag("pitch") if c.type(i) == "rectangle"][:12]
        return [k for k, r in enumerate(rects) if c.itemcget(r, "fill") not in ("#151515", "#303030")]
    check("chromatic: all 12 keys lit", len(lit_keys()) == 12)
    press_button("scale", 2)
    check("tap on Minor sets scale = 2", proc.v("scale") == 2)
    press_button("key", 2)
    check("tap on the D key sets the tonic = 2", proc.v("key") == 2)
    check("D minor lit: D E F G A A# C", lit_keys() == [0, 2, 4, 5, 7, 9, 10])
    rects = [i for i in c.find_withtag("pitch") if c.type(i) == "rectangle"][:12]
    check("the tonic key is filled", c.itemcget(rects[2], "fill") == mod.BUTTON_ON)
    proc.engine.notes = [62, 69, 74]                 # D, A, D an octave up: two pitch classes
    pump(lambda: False, 0.05)
    dots = c.find_withtag("pnotes")
    check("sounding notes: dots on D and A", len(dots) == 2)
    pump(lambda: False, 0.1)
    check("dots not redrawn when the notes stay", c.find_withtag("pnotes") == dots)
    statics = c.find_withtag("pitch")
    proc.engine.notes = [64]
    pump(lambda: False, 0.05)
    check("a note change redraws only the dots", c.find_withtag("pitch") == statics and
          len(c.find_withtag("pnotes")) == 1)
    proc.engine.notes = []
    # Bend range: below the scale buttons, finger up = wider, double tap = back to 2
    bb = wdg.bend_box
    check("Pitch tab: Bend range box below the scale buttons", bb is not None and
          bb[1] > max(b[3] for b in wdg.buttons if b[4] == "scale") and bb[3] < wdg.height)
    wdg._on_press(Tap((bb[0] + bb[2]) // 2, (bb[1] + bb[3]) // 2))
    wdg._on_motion(Tap((bb[0] + bb[2]) // 2 + (bb[2] - bb[0]) * 10 / 24.0 + 1, (bb[1] + bb[3]) // 2))
    wdg._on_release(None)
    check("drag right on Bend: range 12", proc.v("bend_range") == 12)
    pump(lambda: False, 0.05)
    check("Bend label shows the range", any(c.itemcget(i, "text") == "Bend \u00b112"
                                           for i in c.find_withtag("pitch") if c.type(i) == "text"))
    wdg.last_tap = (None, 0.0)
    for _ in range(2):
        wdg._on_press(Tap((bb[0] + bb[2]) // 2, (bb[1] + bb[3]) // 2))
        wdg._on_release(None)
    check("double tap on Bend: back to 2", proc.v("bend_range") == 2)

    # Filter tab
    import math
    x0, x1 = wdg.tab_boxes[3]
    wdg._on_press(Tap((x0 + x1) // 2, hh // 2))
    pump(lambda: False, 0.05)

    def curve():
        ln = [i for i in c.find_withtag("filter") if c.type(i) == "line" and c.itemcget(i, "width") == "2.0"]
        return c.coords(ln[0]) if ln else []

    def db_at(f):
        """dB of the drawn curve at f Hz (nearest point)"""
        fx0, fy0, fx1, fy1 = wdg._filter_layout()
        co = curve()
        xs = co[0::2]
        x = wdg._fx(f, fx0, fx1)
        k = min(range(len(xs)), key=lambda i: abs(xs[i] - x))
        y = co[2 * k + 1]
        return wdg.DB_TOP - (y - fy0) / (fy1 - fy0) * (wdg.DB_TOP - wdg.DB_BOTTOM)
    check("Filter tab: open low-pass is a flat line at 0 dB", wdg.tab == 3 and abs(db_at(100)) < 0.5 and
          abs(db_at(10000)) < 0.5)
    proc.controllers_dict["cutoff"].value = 1000.0
    pump(lambda: False, 0.05)
    check("LP 1 kHz: 0 dB at 100 Hz, -3 dB at 1 kHz, about -36 at 8 kHz",
          abs(db_at(100)) < 0.5 and abs(db_at(1000) + 3) < 1.0 and -40 < db_at(8000) < -33)
    proc.controllers_dict["ftype"].value = 1
    pump(lambda: False, 0.05)
    check("HP 1 kHz mirrors it", db_at(100) < -35 and abs(db_at(10000)) < 0.5)
    proc.controllers_dict["ftype"].value = 0
    proc.controllers_dict["resonance"].value = 100.0
    pump(lambda: False, 0.05)
    check("resonance 100 %: +26 dB peak at the cutoff", abs(db_at(1000) - 26) < 1.5)
    items = c.find_withtag("filter")
    pump(lambda: False, 0.1)
    check("Filter tab not redrawn when the knobs stay", c.find_withtag("filter") == items)
    check("python curve = engine formula (LP 1k, BP 50 %: +2.7 dB at the centre)",
          abs(20 * math.log10(mod.filter_gain(1000, 1000, 0.5, 2)) - 2.7) < 0.1)

    def gdb(f, fc, res, t):
        return 20 * math.log10(max(mod.filter_gain(f, fc, res, t), 1e-9))
    # the same points test_grain measures on the engine (printed there as "filters: ...")
    check("python curve = engine: LP24 1k is -24.2 dB at 2 kHz", abs(gdb(2000, 1000, 0, 3) + 24.2) < 0.3)
    check("python curve = engine: HP24 1k is -3 dB at 1 kHz", abs(gdb(1000, 1000, 0, 4) + 3.0) < 0.3)
    check("python curve = engine: Notch 1k is a deep hole", gdb(1000, 1000, 0, 5) < -60 and abs(gdb(150, 1000, 0, 5)) < 1)
    check("python curve = engine: Comb 500 Hz res 100 %: +18.2 / -18.2 dB",
          abs(gdb(500, 500, 1, 6) - 18.2) < 0.5 and abs(gdb(750, 500, 1, 6) + 18.2) < 0.5)
    knob_hz = lambda x: 20 + 19980 * (10 ** x - 1) / 9.0      # Zynthian's log knob
    vow_i = knob_hz(0.5)
    check("python curve = engine: Vowel A +9.5 dB at 650 Hz, I +9.5 dB at 290 Hz",
          abs(gdb(650, 20, 0.5, 7) - 9.5) < 0.5 and abs(gdb(290, vow_i, 0.5, 7) - 9.5) < 0.5)
    check("python curve = engine: Vowel keeps the top (-6 dB at 10 kHz)", -8 < gdb(10000, 20, 0.5, 7) < -4)
    check("python curve = engine: Vowel keeps the bottom, quieter (-12 dB at 60 Hz)", -14 < gdb(60, 20, 0.5, 7) < -10)
    texts = lambda: [c.itemcget(i, "text") for i in c.find_withtag("filter") if c.type(i) == "text"]
    for t, fc, want in ((5, 20.0, "Vowel  A"), (5, vow_i, "Vowel  I"), (5, knob_hz(0.625), "Vowel  I-O"),
                        (4, 220.0, "Comb  220 Hz"), (0, 1000.0, "LP 12  1.0 kHz"), (2, 1000.0, "BP 12  1.0 kHz")):
        proc.controllers_dict["ftype"].value = t
        proc.controllers_dict["cutoff"].value = fc
        pump(lambda: False, 0.05)
        check("Filter info: %s" % want, any(x.startswith(want + " ") for x in texts()))
    # LP / HP / BP: 12 | 24 dB switch top left; none on Notch, Comb, Vowel
    proc.controllers_dict["ftype"].value = 0
    proc.controllers_dict["cutoff"].value = 1000.0
    proc.controllers_dict["resonance"].value = 0.0
    pump(lambda: False, 0.05)
    fx0, fy0, fx1, fy1 = wdg._filter_layout()
    sb = wdg.slope_boxes
    check("LP: the 12 | 24 dB switch in the top left, 12 lit", len(sb) == 2 and sb[0][0][0] < fx0 + 10 and
          "12 dB" in texts() and "24 dB" in texts() and proc.v("filter_slope") == 0)
    lp12 = db_at(2000)
    b24 = sb[1][0]
    wdg._on_press(Tap((b24[0] + b24[2]) / 2, (b24[1] + b24[3]) / 2)); wdg._on_release(None)
    pump(lambda: False, 0.05)
    check("tap on 24 dB: slope 1, LP 24 in the info, the curve twice as steep", proc.v("filter_slope") == 1 and
          any(x.startswith("LP 24 ") for x in texts()) and abs(db_at(2000) - 2 * lp12) < 2.5)
    proc.controllers_dict["ftype"].value = 2
    pump(lambda: False, 0.05)
    check("BP 24: same peak as BP 12, steeper sides", any(x.startswith("BP 24 ") for x in texts()) and
          abs(db_at(1000) + 3.0) < 1.0 and db_at(4000) < -18)   # BP 12: -3 at the centre, -12 at 4k
    check("python curve = engine: BP 24 peak = BP 12 peak",
          abs(gdb(1000, 1000, 0.5, 8) - gdb(1000, 1000, 0.5, 2)) < 0.3)
    for t, name in ((3, "Notch"), (4, "Comb"), (5, "Vowel")):
        proc.controllers_dict["ftype"].value = t
        pump(lambda: False, 0.05)
        check("%s: no slope switch" % name, not wdg.slope_boxes and "24 dB" not in texts())
    proc.controllers_dict["ftype"].value = 0
    pump(lambda: False, 0.05)
    b12 = wdg.slope_boxes[0][0]
    wdg._on_press(Tap((b12[0] + b12[2]) / 2, (b12[1] + b12[3]) / 2)); wdg._on_release(None)
    pump(lambda: False, 0.05)
    check("tap on 12 dB: back to 12", proc.v("filter_slope") == 0)
    # Mix (dry/wet): bar in the top right of the plot, finger left/right, double tap = 100 %
    mb = wdg.mix_box
    fx0, fy0, fx1, fy1 = wdg._filter_layout()
    check("Mix bar in the top right corner of the plot", mb and mb[2] <= fx1 and mb[2] > fx1 - 10 and
          mb[1] >= fy0 and mb[1] < fy0 + 10 and mb[0] > (fx0 + fx1) / 2 - 40)
    check("Mix label: 100 %", "Mix 100 %" in texts())
    mx, my = (mb[0] + mb[2]) / 2, (mb[1] + mb[3]) / 2
    mw = mb[2] - mb[0]
    time.sleep(mod.DOUBLE_TAP + 0.02)
    wdg._on_press(Tap(mx, my)); wdg._on_motion(Tap(mx - mw / 2.0, my + 20)); wdg._on_release(None)
    pump(lambda: False, 0.05)
    check("Mix: finger left half the bar = 50 %", abs(proc.v("filter_mix") - 50) <= 1 and "Mix 50 %" in texts())
    proc.controllers_dict["ftype"].value = 3                      # Notch
    proc.controllers_dict["cutoff"].value = 1000.0
    proc.controllers_dict["resonance"].value = 0.0               # a wide hole: the nearest curve point is in it
    pump(lambda: False, 0.05)
    check("Mix 50 %: the notch hole is -6 dB on the curve", abs(db_at(1000) + 6.0) < 0.6)
    check("python curve = engine: Mix 50 % on the notch -6.0 dB",
          abs(20 * math.log10(mod.filter_gain(1000, 1000, 0, 5, mix=0.5)) + 6.02) < 0.1)
    time.sleep(mod.DOUBLE_TAP + 0.02)
    wdg._on_press(Tap(mx, my)); wdg._on_motion(Tap(mx - 5000, my)); wdg._on_release(None)
    pump(lambda: False, 0.05)
    check("Mix 0: dry, flat curve, (off) in the info", proc.v("filter_mix") == 0 and abs(db_at(1000)) < 0.5 and
          "Mix 0 % (dry)" in texts() and any(x.endswith("(off)") for x in texts()))
    time.sleep(mod.DOUBLE_TAP + 0.02)
    for _ in range(2):
        wdg._on_press(Tap(mx, my)); wdg._on_release(None)
    pump(lambda: False, 0.05)
    check("Mix: double tap = 100 %", proc.v("filter_mix") == 100)
    # Vowel Lows / Highs: only with Vowel; Lows top left, Highs under Mix
    check("no Lows/Highs bars on a Notch", wdg.lows_box is None and wdg.highs_box is None and
          not any(x.startswith("Lows") or x.startswith("Highs") for x in texts()))
    proc.controllers_dict["ftype"].value = 5                      # Vowel
    proc.controllers_dict["cutoff"].value = 20.0
    pump(lambda: False, 0.05)
    lb, hb, mb = wdg.lows_box, wdg.highs_box, wdg.mix_box
    check("Vowel: Lows top left, Highs under Mix", lb and hb and lb[0] < fx0 + 10 and lb[1] == mb[1] and
          hb[0] == mb[0] and hb[1] > mb[3] and lb[2] < mb[0])
    check("Vowel: Lows -12 dB, Highs -6 dB", "Lows -12 dB" in texts() and "Highs -6 dB" in texts())
    hx, hy = (hb[0] + hb[2]) / 2, (hb[1] + hb[3]) / 2
    time.sleep(mod.DOUBLE_TAP + 0.02)
    wdg._on_press(Tap(hx, hy)); wdg._on_motion(Tap(hx + 5000, hy)); wdg._on_release(None)
    pump(lambda: False, 0.05)
    check("Highs: finger right to 100 % = 0 dB, the curve's top follows", proc.v("vowel_highs") == 100 and
          "Highs 0 dB" in texts() and abs(db_at(12000)) < 1.5)
    lx, ly = (lb[0] + lb[2]) / 2, (lb[1] + lb[3]) / 2
    time.sleep(mod.DOUBLE_TAP + 0.02)
    wdg._on_press(Tap(lx, ly)); wdg._on_motion(Tap(lx - 5000, ly)); wdg._on_release(None)
    pump(lambda: False, 0.05)
    check("Lows: finger left = off, the curve's bottom drops", proc.v("vowel_lows") == 0 and "Lows off" in texts() and
          db_at(40) < -40)
    for (x, y, sym, d) in ((hx, hy, "vowel_highs", 50), (lx, ly, "vowel_lows", 25)):
        time.sleep(mod.DOUBLE_TAP + 0.02)
        for _ in range(2):
            wdg._on_press(Tap(x, y)); wdg._on_release(None)
        pump(lambda: False, 0.05)
        check("%s: double tap = default %d" % (sym, d), proc.v(sym) == d)
    proc.controllers_dict["ftype"].value = 0
    proc.controllers_dict["ftype"].value = 0
    nudges0 = sample.nudges
    wdg._on_press(Tap(wdg.width // 2, wdg.height // 2))
    check("a tap on the Filter tab does not open the file selector", sample.nudges == nudges0)
    proc.controllers_dict["cutoff"].value = 20000.0
    proc.controllers_dict["resonance"].value = 0.0

    # Mod tab
    x0, x1 = wdg.tab_boxes[4]
    wdg._on_press(Tap((x0 + x1) // 2, hh // 2))
    pump(lambda: False, 0.05)
    check("Mod tab: 63 cells (7 x 9)", wdg.tab == 4 and len(wdg.mod_items) == 63)
    W_, H_, pad_, top_, gx0, gy0, gx1, gy1, cw, ch = wdg._mod_layout()

    def cell_xy(si, di):
        return gx0 + (di + 0.5) * cw, gy0 + (si + 0.5) * ch

    def drag(x, y, dy):
        wdg._on_press(Tap(x, y))
        wdg._on_motion(Tap(x, y - dy))
        wdg._on_release(Tap(x, y - dy))
        time.sleep(mod.DOUBLE_TAP + 0.02)

    x, y = cell_xy(0, 5)                                  # LFO1 -> Pitch
    drag(x, y, 30)
    check("finger up 30 px: +20 on LFO1>Pitch", proc.v("m_lfo1_pitch") == 20)
    r, t = wdg.mod_items[(0, 5)]
    check("positive cell is blue with its value", c.itemcget(t, "text") == "+20" and
          c.itemcget(r, "fill") == mod.mix(mod.MOD_POS, 0.25 + 0.75 * 0.2))
    check("a source in use is lit", c.itemcget(wdg.mod_labels[0], "fill") == cfg.color_tx and
          c.itemcget(wdg.mod_labels[1], "fill") == "#606060")
    x, y = cell_xy(3, 8)                                  # Env2 -> Cutoff
    drag(x, y, -60)
    check("finger down: negative, orange", proc.v("m_env2_cutoff") == -40 and
          c.itemcget(wdg.mod_items[(3, 8)][0], "fill") == mod.mix(mod.MOD_NEG, 0.25 + 0.75 * 0.4))
    drag(x, y, -1000)
    check("clamped at -100", proc.v("m_env2_cutoff") == -100)
    wdg._on_press(Tap(x, y)); wdg._on_release(Tap(x, y))
    wdg._on_press(Tap(x, y)); wdg._on_release(Tap(x, y))
    check("double tap resets the cell", proc.v("m_env2_cutoff") == 0 and
          c.itemcget(wdg.mod_items[(3, 8)][1], "text") == "")
    time.sleep(mod.DOUBLE_TAP + 0.02)
    boxes = {b[0]: b for b in wdg._side_boxes()}

    def tap_box(sym):
        b = boxes[sym]
        wdg._on_press(Tap((b[2] + b[4]) / 2, (b[3] + b[5]) / 2))
        wdg._on_release(None)
        time.sleep(mod.DOUBLE_TAP + 0.02)
    tap_box("lfo1_shape"); tap_box("lfo1_shape")
    check("taps on LFO 1 change its shape (sine -> tri -> saw up)", proc.v("lfo1_shape") == 2)
    for _ in range(5):
        tap_box("lfo1_shape")
    check("shape wraps around after Smooth", proc.v("lfo1_shape") == 0)

    def sync_label(sym):
        b = boxes[sym]
        for t in c.find_withtag("modside"):
            if c.type(t) == "text":
                x, y = c.coords(t)
                if b[2] <= x <= b[4] and b[3] <= y <= b[5]:
                    return c.itemcget(t, "text")
    check("Free shows the Rate knob in Hz (42 = 0.24 Hz)", sync_label("lfo2_sync") == "Free\n0.24 Hz")
    tap_box("lfo2_sync")
    check("tap on LFO 2 sync: Key, still Hz", proc.v("lfo2_sync") == 1 and sync_label("lfo2_sync") == "Key\n0.24 Hz")
    tap_box("lfo2_sync")
    check("next tap: Tempo, the same knob as a cycle length (42 = 1/4)",
          proc.v("lfo2_sync") == 2 and sync_label("lfo2_sync").endswith("1/4"))
    proc.controllers_dict["lfo2_rate"].value = 61                # the knob turned
    wdg.refresh_gui()
    check("Rate 61 in Tempo: 1/8", sync_label("lfo2_sync").endswith("1/8"))
    for u, want in ((0, "8 bars"), (100, "1/32"), (27, "1 bar"), (9, "4 bars")):
        proc.controllers_dict["lfo2_rate"].value = u
        wdg.refresh_gui()
        check("Rate %d in Tempo: %s" % (u, want), sync_label("lfo2_sync").endswith(" " + want))
    check("helpers match gc_rate_hz at the ends and the defaults",
          abs(mod.rate_hz(0) - 0.01) < 1e-9 and abs(mod.rate_hz(100) - 20) < 1e-9 and
          mod.hz_text(mod.rate_hz(61)) == "1.0 Hz" and mod.hz_text(mod.rate_hz(100)) == "20 Hz")
    proc.controllers_dict["lfo2_rate"].value = 61
    time.sleep(mod.DOUBLE_TAP + 0.02)
    tap_box("lfo2_sync")
    check("tap in Tempo: back to Free, in Hz", proc.v("lfo2_sync") == 0 and sync_label("lfo2_sync") == "Free\n1.0 Hz")
    b = boxes["lfo2_sync"]
    bx, by = (b[2] + b[4]) / 2, (b[3] + b[5]) / 2
    writes = proc.controllers_dict["lfo2_rate"].writes
    wdg._on_press(Tap(bx, by)); wdg._on_motion(Tap(bx, by - 40)); wdg._on_release(Tap(bx, by - 40))
    check("a drag on the sync button changes nothing", proc.controllers_dict["lfo2_rate"].writes == writes and
          proc.v("lfo2_sync") == 0)
    time.sleep(mod.DOUBLE_TAP + 0.02)
    tap_box("lfo2_sync")
    b = boxes["env1_shape"]
    drag((b[2] + b[4]) / 2, (b[3] + b[5]) / 2, 45)
    check("Env 1 shape: finger up 45 px = +30", proc.v("env1_shape") == 40)
    b = boxes["mod_depth"]
    drag((b[2] + b[4]) / 2, (b[3] + b[5]) / 2, 600)
    check("Mod Depth clamped at 100", proc.v("mod_depth") == 100)
    drag((b[2] + b[4]) / 2, (b[3] + b[5]) / 2, -60)
    check("Mod Depth down to 60", proc.v("mod_depth") == 60)
    cells = {k: v for k, v in wdg.mod_items.items()}
    side = c.find_withtag("modside")
    configured = []
    orig_cfg = c.itemconfigure
    c.itemconfigure = lambda *a, **k: configured.append(a) or orig_cfg(*a, **k)
    pump(lambda: False, 0.1)
    c.itemconfigure = orig_cfg
    check("nothing redrawn when the values stay", configured == [] and c.find_withtag("modside") == side and
          wdg.mod_items == cells)

    # LFO polarity: the small button in the corner of the shape box
    pb = boxes["lfo1_uni"]
    sb = boxes["lfo1_shape"]
    check("polarity button inside the LFO 1 box, top right", sb[2] < pb[2] and pb[4] <= sb[4] and pb[3] == sb[3])
    shape0 = proc.v("lfo1_shape")

    def side_text_in(b):
        for t in c.find_withtag("modside"):
            if c.type(t) == "text":
                x, y = c.coords(t)
                if b[2] <= x <= b[4] and b[3] <= y <= b[5]:
                    return c.itemcget(t, "text")
    check("polarity shows +- (both ways)", side_text_in(pb) == "\u00b1")
    glyph = [t for t in c.find_withtag("modside") if c.type(t) == "text" and c.itemcget(t, "text") == "\u00b1"]
    gb = c.bbox(glyph[0]) if glyph else None
    check("polarity button big (%d px, at least twice the font) and its sign fills it, inside" % (pb[4] - pb[2]),
          pb[4] - pb[2] >= min(2 * wdg._font_size(), pb[5] - pb[3]) and gb and gb[3] - gb[1] >= 0.5 * (pb[5] - pb[3]) and
          gb[0] >= pb[2] - 1 and gb[2] <= pb[4] + 1)
    wdg._on_press(Tap((pb[2] + pb[4]) / 2, (pb[3] + pb[5]) / 2)); wdg._on_release(None)
    pump(lambda: False, 0.05)
    check("tap: + (0..1), the shape stays", proc.v("lfo1_uni") == 1 and proc.v("lfo1_shape") == shape0 and
          side_text_in(pb) == "+")
    time.sleep(mod.DOUBLE_TAP + 0.02)
    wdg._on_press(Tap((pb[2] + pb[4]) / 2, (pb[3] + pb[5]) / 2)); wdg._on_release(None)
    check("tap again: back to +-", proc.v("lfo1_uni") == 0)
    time.sleep(mod.DOUBLE_TAP + 0.02)
    lab = [t for t in c.find_withtag("modside") if c.type(t) == "text" and c.itemcget(t, "text").split(" ")[0:2] in (["LFO", "1"], ["L1"])]
    pbt = [t for t in c.find_withtag("modside") if c.type(t) == "rectangle" and
           [round(v) for v in c.coords(t)] == [round(v) for v in pb[2:6]]]
    bl, br_ = c.bbox(lab[0]) if lab else None, c.bbox(pbt[-1]) if pbt else None
    check("LFO 1 label clear of the polarity button", bl and br_ and bl[2] <= br_[0])

    # X-Mod tab: 7 sources x 6 modulator targets
    x0, x1 = wdg.tab_boxes[5]
    wdg._on_press(Tap((x0 + x1) // 2, hh // 2))
    pump(lambda: False, 0.05)
    check("X-Mod tab: 42 cells (7 x 6), no side column", wdg.tab == 5 and len(wdg.mod_items) == 42 and
          not c.find_withtag("modside"))
    W_, H_, pad_, top_, gx0, gy0, gx1, gy1, cw, ch = wdg._mod_layout()
    x, y = cell_xy(2, 2)                                  # Env1 -> LFO1 Amount
    drag(x, y, 45)
    check("finger up 45 px: +30 on Env1>L1 Amt", proc.v("x_env1_lfo1_amt") == 30 and proc.v("m_env1_spray") == 0)
    wdg._on_press(Tap(x, y)); wdg._on_release(None); wdg._on_press(Tap(x, y)); wdg._on_release(None)
    check("double tap resets it", proc.v("x_env1_lfo1_amt") == 0)
    time.sleep(mod.DOUBLE_TAP + 0.02)
    x, y = cell_xy(4, 4)                                  # Vel -> Env1 Time
    drag(x, y, -60)
    pump(lambda: False, 0.05)
    check("finger down: Vel>E1 Time -40, the Vel row lit", proc.v("x_vel_env1_time") == -40 and
          c.itemcget(wdg.mod_labels[4], "fill") == cfg.color_tx)
    x, y = cell_xy(1, 0)                                  # LFO2 -> LFO1 Rate
    drag(x, y, 30)
    pump(lambda: False, 0.05)
    lit = c.itemcget(wdg.mod_items[(1, 0)][0], "fill")
    proc.controllers_dict["lfo1_sync"].value = 2          # LFO 1 on the tempo: its Rate column does nothing
    pump(lambda: False, 0.05)
    grey = c.itemcget(wdg.mod_items[(1, 0)][0], "fill")
    check("tempo LFO 1: its Rate cells greyed out, the others not",
          grey != lit and c.itemcget(wdg.mod_items[(1, 1)][0], "fill") == c.itemcget(wdg.mod_items[(0, 3)][0], "fill"))
    proc.controllers_dict["lfo1_sync"].value = 0
    pump(lambda: False, 0.05)
    check("Free again: the colour back", c.itemcget(wdg.mod_items[(1, 0)][0], "fill") == lit)
    for k in ("x_vel_env1_time", "x_lfo2_lfo1_rate"):
        proc.controllers_dict[k].value = 0

    x0, x1 = wdg.tab_boxes[0]
    wdg._on_press(Tap((x0 + x1) // 2, hh // 2))
    proc.engine.playhead = 0.3
    pump(lambda: False, 0.05)
    check("back to Wave: the same waveform image, not rebuilt; marks shown",
          wdg.tab == 0 and wdg.wave_img is img_before and wave_drawn() and
          c.itemcget(wdg.marks[0], "state") == "normal")
    wdg._on_press(Tap(wdg.width // 2, wdg.height // 2))
    check("tap on the waveform still opens the file selector", sample.nudges == nudges + 1)

    # ---------- tabs <-> knob pages ----------
    ctl = wdg.zyngui_control
    ctl.add_pages(proc)
    ctl.select(4)                                         # Voice page: no tab of its own
    pump(lambda: False, 0.05)
    check("a page without a tab keeps the tab", wdg.tab == 0)
    x0, x1 = wdg.tab_boxes[2]
    wdg._on_press(Tap((x0 + x1) // 2, hh // 2))
    pump(lambda: False, 0.05)
    check("tap on the Pitch tab selects the Pitch knob page", ctl.selected[-1] == 3 and ctl.screen_title == "Pitch"
          and wdg.tab == 2)
    x0, x1 = wdg.tab_boxes[0]
    wdg._on_press(Tap((x0 + x1) // 2, hh // 2))
    pump(lambda: False, 0.05)
    check("tap on Wave selects the Position page", ctl.screen_title == "Position" and wdg.tab == 0)
    ctl.select(6)                                         # a knob turns to the Mod page
    pump(lambda: False, 0.05)
    check("turning to the Mod page shows the Mod tab", wdg.tab == 4)
    ctl.select(5)
    pump(lambda: False, 0.05)
    check("turning to the Filter page shows the Filter tab", wdg.tab == 3)
    n = len(ctl.selected)
    pump(lambda: False, 0.1)
    check("no page selected by the widget on its own", len(ctl.selected) == n)
    ctl.select(1)
    pump(lambda: False, 0.05)

    # ---------- no overlapping text at a Zynthian-like size ----------
    def overlap(b1, b2):
        return b1 and b2 and not (b1[2] <= b2[0] or b2[2] <= b1[0] or b1[3] <= b2[1] or b2[3] <= b1[1])
    for geo in ("800x380", "480x272"):
        root.geometry(geo)
        pump(lambda: False, 0.3)
        x0, x1 = wdg.tab_boxes[3]
        wdg._on_press(Tap((x0 + x1) // 2, hh // 2))
        pump(lambda: False, 0.05)
        texts = [i for i in c.find_withtag("filter") if c.type(i) == "text"]
        info = [t for t in texts if c.itemcget(t, "text").split(" ")[0] in mod.FILTER_TYPES][0]
        mixt = [t for t in texts if c.itemcget(t, "text").startswith("Mix")][0]
        mb = wdg.mix_box
        tb = c.bbox(mixt)
        check("%s Filter: Mix label inside its bar, the bar below the tabs" % geo,
              tb[0] >= mb[0] and tb[2] <= mb[2] and mb[1] >= wdg._header_h())
        check("%s Filter: Mix bar clear of the dB labels" % geo,
              not any(overlap(c.bbox(t), mb) for t in texts if t != mixt))
        axis = [t for t in texts if c.itemcget(t, "text") in ("100", "1k", "10k")]
        check("%s Filter: frequency labels clear of the info line" % geo,
              len(axis) == 3 and not any(overlap(c.bbox(t), c.bbox(info)) for t in axis))
        proc.controllers_dict["ftype"].value = 5                  # Vowel
        pump(lambda: False, 0.05)
        texts = [i for i in c.find_withtag("filter") if c.type(i) == "text"]
        bars = {"Lows": wdg.lows_box, "Highs": wdg.highs_box, "Mix": wdg.mix_box}
        ok = all(any(c.itemcget(t, "text").startswith(k) and c.bbox(t)[0] >= b[0] and c.bbox(t)[2] <= b[2]
                     for t in texts) for k, b in bars.items())
        check("%s Vowel: Lows, Highs and Mix labels inside their bars" % geo, ok)
        check("%s Vowel: the three bars do not overlap" % geo, not overlap(bars["Lows"], bars["Mix"]) and
              not overlap(bars["Lows"], bars["Highs"]) and not overlap(bars["Mix"], bars["Highs"]))
        proc.controllers_dict["ftype"].value = 0
        pump(lambda: False, 0.05)
        texts = [i for i in c.find_withtag("filter") if c.type(i) == "text"]
        ok = all(any(c.itemcget(t, "text") == "%d dB" % d and c.bbox(t)[0] >= b_[0] and c.bbox(t)[2] <= b_[2]
                     for t in texts) for (b_, i), d in zip(wdg.slope_boxes, (12, 24)))
        check("%s LP: 12 / 24 dB inside their buttons, clear of Mix" % geo,
              ok and not overlap(wdg.slope_boxes[1][0], wdg.mix_box))
        x0, x1 = wdg.tab_boxes[4]
        wdg._on_press(Tap((x0 + x1) // 2, hh // 2))
        pump(lambda: False, 0.05)
        heads = [i for i in c.find_withtag("mod") if c.type(i) == "text" and c.itemcget(i, "text") in
                 [d[1] for d in mod.MOD_DSTS] + mod.MOD_DSTS_SHORT]
        heads.sort(key=lambda i: c.coords(i)[0])
        check("%s Mod: column labels do not overlap" % geo,
              len(heads) == 9 and not any(overlap(c.bbox(a_), c.bbox(b_)) for a_, b_ in zip(heads, heads[1:])))
        rows = wdg.mod_labels
        first_cell = wdg.mod_items[(0, 0)][0]
        check("%s Mod: row labels clear of the grid" % geo,
              all(c.bbox(r)[2] <= c.bbox(first_cell)[0] + 1 for r in rows))
        lab = [t for t in c.find_withtag("modside") if c.type(t) == "text" and c.itemcget(t, "text").startswith("LFO ")]
        pols = [b_ for b_ in wdg._side_boxes() if b_[1] == "pol"]
        check("%s Mod: LFO labels clear of the polarity buttons" % geo,
              len(lab) == 2 and all(c.bbox(t)[2] <= pb_[2] for t, pb_ in zip(sorted(lab, key=lambda t: c.coords(t)[1]), pols)))
        x0, x1 = wdg.tab_boxes[5]
        wdg._on_press(Tap((x0 + x1) // 2, hh // 2))
        pump(lambda: False, 0.05)
        heads = [i for i in c.find_withtag("mod") if c.type(i) == "text" and c.itemcget(i, "text") in
                 [d[1] for d in mod.XMOD_DSTS] + mod.XMOD_DSTS_SHORT]
        heads.sort(key=lambda i: c.coords(i)[0])
        check("%s X-Mod: 6 column labels, no overlap, inside the screen" % geo,
              len(heads) == 6 and not any(overlap(c.bbox(a_), c.bbox(b_)) for a_, b_ in zip(heads, heads[1:])) and
              c.bbox(heads[-1])[2] <= wdg.width)
        check("%s X-Mod: the tab bar fits" % geo, wdg.tab_boxes[-1][1] <= wdg.width)
        tt = [i for i in c.find_withtag("title") if c.type(i) == "text" and c.itemcget(i, "text")]
        check("%s: the page title (if shown) clear of the tabs" % geo,
              all(c.bbox(i)[0] >= wdg.tab_boxes[-1][1] for i in tt))
        inside = True
        for sb in wdg._side_boxes():
            if sb[1] != "sync":
                continue
            for t in c.find_withtag("modside"):
                if c.type(t) == "text":
                    x, y = c.coords(t)
                    if sb[2] <= x <= sb[4] and sb[3] <= y <= sb[5]:
                        tb = c.bbox(t)
                        inside &= tb[0] >= sb[2] - 1 and tb[2] <= sb[4] + 1 and tb[1] >= sb[3] - 1 and tb[3] <= sb[5] + 1
        check("%s Mod: sync buttons hold their two lines (mode, Hz)" % geo, inside)
        x0, x1 = wdg.tab_boxes[0]
        wdg._on_press(Tap((x0 + x1) // 2, hh // 2))
        pump(lambda: False, 0.1)
        boxes = list(wdg.wave_boxes.values())
        check("%s Wave: Root, Tune, Auto inside the widget, under the waveform" % geo,
              len(boxes) == 3 and all(b[0] >= 0 and b[2] <= wdg.width and b[3] <= wdg.height and
                                      b[1] >= wdg._area()[1] for b in boxes))
        gb = wdg._gain_box()
        check("%s Wave: gain slider inside the widget, clear of the bottom row" % geo,
              gb[2] <= wdg.width and gb[3] <= wdg._area()[1] and gb[3] - gb[1] > 40)
        tinfo = [i for i in c.find_withtag("info") if c.type(i) == "text"]
        check("%s Wave: file info inside the widget" % geo, tinfo and c.bbox(tinfo[0])[2] <= wdg.width)
        x0, x1 = wdg.tab_boxes[2]
        wdg._on_press(Tap((x0 + x1) // 2, hh // 2))
        pump(lambda: False, 0.05)
        items = c.find_withtag("pitch")
        bottom = max(c.bbox(i)[3] for i in items)
        right = max(c.bbox(i)[2] for i in items)
        check("%s Pitch: everything inside the widget (bottom %d of %d, right %d of %d)" %
              (geo, bottom, wdg.height, right, wdg.width), bottom <= wdg.height and right <= wdg.width)
        texts = [i for i in items if c.type(i) == "text"]
        check("%s Pitch: no overlapping text" % geo,
              not any(overlap(c.bbox(a_), c.bbox(b_)) for k, a_ in enumerate(texts) for b_ in texts[k + 1:]))
    root.geometry("480x300")
    pump(lambda: False, 0.2)

    # Zynthian calls update() every 32 ms: the widget asks for monitors only every UPDATE_PERIOD
    mod.UPDATE_PERIOD = throttle
    polls = []
    get = proc.engine.get_monitors_dict
    proc.engine.get_monitors_dict = lambda: polls.append(1) or get()
    t = time.monotonic()
    while time.monotonic() - t < 1.0:
        wdg.update()
        root.update()
        time.sleep(0.032)
    proc.engine.get_monitors_dict = get
    check("monitors polled about 12 times a second, not 30 (%d)" % len(polls), 9 <= len(polls) <= 14)

    root.destroy()
    print("All widget tests passed" if not fails else "%d FAILED" % fails)
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
