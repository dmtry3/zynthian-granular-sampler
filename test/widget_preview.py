#!/usr/bin/python3
# Runs the Granular Sampler widget outside Zynthian with stub modules (same approach as Generative Arp).
#   python3 test/widget_preview.py          scripted tests (PASS/FAIL)
#   python3 test/widget_preview.py --show   open a window: tap = "file selector" cycles through the samples
# Needs samples/test_pad.wav: python3 scripts/gen_samples.py
import importlib.util
import os
import sys
import time
import tkinter
import types

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
PAD = os.path.join(ROOT, "samples", "test_pad.wav")

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

    def get_monitors_dict(self):
        m = {"status": float(self.status), "seconds": float(self.seconds), "playhead": float(self.playhead)}
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
                                 "mode": NumCtrl("mode", 0), "voices": NumCtrl("voices", 8),
                                 "scale": NumCtrl("scale", 0), "key": NumCtrl("key", 0),
                                 "quantize": NumCtrl("quantize", 0.0), "pitch": NumCtrl("pitch", 0.0),
                                 "pitch_spray": NumCtrl("pitch_spray", 0.0)}

    def v(self, sym):
        return self.controllers_dict[sym].value


class Control:
    def __init__(self, root):
        self.main_frame = root
        self.layout = {"rows": 4}
        self.screen_title = "Voice"
        self.wide = True
        self.shown = True


def load_widget():
    path = os.path.join(ROOT, "ui", "zynthian_widget_granular_sampler.py")
    spec = importlib.util.spec_from_file_location("zynthian_widget_granular_sampler", path)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def main():
    show = "--show" in sys.argv
    if not os.path.exists(PAD):
        print("Missing %s: run python3 scripts/gen_samples.py" % PAD)
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
        items = c.find_withtag("info")
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
    check("status from the plugin: loading", "loading" in info() and "test_pad" in info())
    lo = min(p[0] for p in wdg.peaks)
    hi = max(p[1] for p in wdg.peaks)
    check("peaks look like the pad (about +-0.55)", -0.6 < lo < -0.3 and 0.3 < hi < 0.6)

    proc.engine.status, proc.engine.seconds = 2, 6.0
    pump(lambda: False, 0.05)
    check("status from the plugin: loaded, length", info() == "test_pad  6.0 s")

    # zone, playhead and grain marks
    proc.controllers_dict["position"].value = 25.0
    proc.controllers_dict["window"].value = 50.0
    proc.engine.playhead = 0.5
    proc.engine.grains = [0.4, 0.6]
    pump(lambda: False, 0.05)
    zones = [i for i in c.find_withtag("zone") if c.type(i) == "rectangle"]
    W = wdg.width
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

    # resize redraws it
    root.geometry("360x200")
    pump(lambda: wdg.width < 400, 2)
    check("waveform redrawn after resize", c.find_withtag("wave") != before and wave_drawn() and
          wdg.wave_img.width() == wdg.width)

    # plugin error, file the widget can't read
    sample.value = "/no/such/file.wav"
    proc.engine.status = 3
    pump(lambda: wdg.peaks_path == "/no/such/file.wav")
    check("missing file: error shown, no waveform", "can't load" in info() and not wave_drawn())

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
    class Tap:
        def __init__(self, x, y):
            self.x, self.y = x, y

    sample.value = PAD
    proc.engine.status = 2
    pump(lambda: wdg.peaks_path == PAD)
    img_before = wdg.wave_img
    check("three tabs: Wave, Grain, Pitch", len(wdg.tab_boxes) == 3 and wdg.tab == 0)
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
    check("voices buttons 1..8 in Poly", len([b for b in wdg.buttons if b[4] == "voices"]) == 8)
    b0 = wdg.buttons[0]
    check("buttons are low: about one text line (%d px)" % (b0[3] - b0[1]),
          b0[3] - b0[1] <= wdg._header_h() * 1.5)
    press_button("voices", 4)
    check("tap on voices 4 sets voices = 4", proc.v("voices") == 4)
    press_button("mode", 1)
    check("tap on Mono sets mode = 1", proc.v("mode") == 1)
    check("in Mono the voices buttons are disabled", not [b for b in wdg.buttons if b[4] == "voices"])
    writes = proc.controllers_dict["voices"].writes
    press_button("voices", 2)
    check("tap on a disabled voices button does nothing", proc.controllers_dict["voices"].writes == writes)
    press_button("mode", 0)
    check("back to Poly", proc.v("mode") == 0 and len([b for b in wdg.buttons if b[4] == "voices"]) == 8)
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

    x0, x1 = wdg.tab_boxes[0]
    wdg._on_press(Tap((x0 + x1) // 2, hh // 2))
    proc.engine.playhead = 0.3
    pump(lambda: False, 0.05)
    check("back to Wave: the same waveform image, not rebuilt; marks shown",
          wdg.tab == 0 and wdg.wave_img is img_before and wave_drawn() and
          c.itemcget(wdg.marks[0], "state") == "normal")
    wdg._on_press(Tap(wdg.width // 2, wdg.height // 2))
    check("tap on the waveform still opens the file selector", sample.nudges == nudges + 1)

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
