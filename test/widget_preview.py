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


class Engine:
    def __init__(self):
        self.status = 0
        self.seconds = 0.0
        self.playhead = -1.0
        self.grains = []

    def get_monitors_dict(self):
        m = {"status": float(self.status), "seconds": float(self.seconds), "playhead": float(self.playhead)}
        for i in range(16):
            m["grain%d" % (i + 1)] = float(self.grains[i]) if i < len(self.grains) else -1.0
        return m


class Processor:
    def __init__(self):
        self.engine = Engine()
        self.id = 1
        self.controllers_dict = {"sample": PathCtrl("sample"),
                                 "position": NumCtrl("position", 0.0), "window": NumCtrl("window", 100.0)}


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
    check("empty: no waveform polygon", not [i for i in c.find_withtag("wave") if c.type(i) == "polygon"])

    wdg._on_press(None)
    check("tap opens the file selector (nudge on the path controller)", sample.nudges == 1)

    # a file is chosen; the plugin is still loading it
    sample.value = PAD
    proc.engine.status = 1
    got = pump(lambda: wdg.peaks_path == PAD)
    check("waveform read in the background", got and mod.NPEAKS - 8 <= len(wdg.peaks) <= mod.NPEAKS)
    polys = [i for i in c.find_withtag("wave") if c.type(i) == "polygon"]
    check("waveform drawn as one polygon", len(polys) == 1)
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
    check("waveform redrawn after resize", c.find_withtag("wave") != before and
          len([i for i in c.find_withtag("wave") if c.type(i) == "polygon"]) == 1)

    # plugin error, file the widget can't read
    sample.value = "/no/such/file.wav"
    proc.engine.status = 3
    pump(lambda: wdg.peaks_path == "/no/such/file.wav")
    check("missing file: error shown, no waveform", "can't load" in info() and
          not [i for i in c.find_withtag("wave") if c.type(i) == "polygon"])

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

    root.destroy()
    print("All widget tests passed" if not fails else "%d FAILED" % fails)
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
