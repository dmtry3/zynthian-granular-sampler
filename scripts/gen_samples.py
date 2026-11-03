#!/usr/bin/python3
# Writes the generated test samples into samples/ (only the stdlib: runs on Zynthian as is).
# test_pad.wav:   6 s stereo pad, a slowly moving chord of detuned saws, good to hear grains on.
# test_drums.wav: 2 bars at 120 BPM of synthesized kick, snare and hi-hat: rhythm, transients.
# test_pluck.wav: a plucked arpeggio (Karplus-Strong): clear pitch, attacks and decays.
import math
import os
import random
import struct
import wave

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
RATE = 48000


def saw(phase):
    return 2.0 * (phase - math.floor(phase + 0.5))


def test_pad(path, secs=6.0):
    rnd = random.Random(1)
    notes = [48, 55, 60, 63, 67, 70]                      # C minor 7 spread
    voices = []
    for n in notes:
        f = 440.0 * 2 ** ((n - 69) / 12.0)
        for det in (-0.07, 0.0, 0.07):                     # semitones
            voices.append([f * 2 ** (det / 12.0), rnd.random(), rnd.uniform(-1, 1)])
    frames = int(secs * RATE)
    out = bytearray()
    lp_l = lp_r = 0.0
    for i in range(frames):
        t = i / RATE
        l = r = 0.0
        for v in voices:
            v[1] += v[0] / RATE
            s = saw(v[1] % 1.0)
            pan = 0.5 + 0.4 * v[2]
            l += s * (1 - pan)
            r += s * pan
        # a lowpass that opens and closes, and a fade in/out
        cut = 0.02 + 0.10 * (0.5 - 0.5 * math.cos(2 * math.pi * t / secs))
        lp_l += cut * (l - lp_l)
        lp_r += cut * (r - lp_r)
        env = min(1.0, t / 0.5, (secs - t) / 0.5)
        g = 0.12 * env
        out += struct.pack("<hh", int(max(-1, min(1, lp_l * g)) * 32767), int(max(-1, min(1, lp_r * g)) * 32767))
    with wave.open(path, "wb") as w:
        w.setnchannels(2)
        w.setsampwidth(2)
        w.setframerate(RATE)
        w.writeframes(bytes(out))


def write_stereo(path, left, right):
    out = bytearray()
    for l, r in zip(left, right):
        out += struct.pack("<hh", int(max(-1, min(1, l)) * 32767), int(max(-1, min(1, r)) * 32767))
    with wave.open(path, "wb") as w:
        w.setnchannels(2)
        w.setsampwidth(2)
        w.setframerate(RATE)
        w.writeframes(bytes(out))


def test_drums(path):
    rnd = random.Random(2)
    beat = RATE // 2                                      # 120 BPM
    n = 8 * beat                                          # 2 bars of 4/4
    left = [0.0] * n
    right = [0.0] * n

    def add(start, sig, pan):
        for i, v in enumerate(sig):
            if start + i < n:
                left[start + i] += v * (1 - pan)
                right[start + i] += v * pan

    kick = [0.9 * math.sin(2 * math.pi * (45 * t + 90 * (1 - math.exp(-t * 30)) / 30)) * math.exp(-t * 7)
            for t in (i / RATE for i in range(int(0.45 * RATE)))]
    snare = [(0.35 * math.sin(2 * math.pi * 190 * t) + 0.45 * (rnd.random() * 2 - 1)) * math.exp(-t * 18)
             for t in (i / RATE for i in range(int(0.3 * RATE)))]
    hat = []
    prev = 0.0
    for i in range(int(0.06 * RATE)):
        x = rnd.random() * 2 - 1
        hat.append(0.25 * (x - prev) * math.exp(-i / RATE * 60))   # differentiated noise: bright
        prev = x
    for b in range(8):
        if b % 4 in (0, 2) or b == 7:
            add(b * beat + (beat // 2 if b == 7 else 0), kick, 0.5)
        if b % 2 == 1:
            add(b * beat, snare, 0.45)
        for e in range(2):
            add(b * beat + e * beat // 2, hat, 0.65 if e else 0.35)
    write_stereo(path, left, right)


def test_pluck(path):
    rnd = random.Random(3)
    notes = [57, 60, 64, 67, 69, 72, 76, 72, 69, 67, 64, 60]          # A minor arpeggio
    step = int(0.35 * RATE)
    n = step * len(notes) + RATE
    left = [0.0] * n
    right = [0.0] * n
    for k, note in enumerate(notes):
        f = 440.0 * 2 ** ((note - 69) / 12.0)
        period = int(RATE / f)
        buf = [rnd.random() * 2 - 1 for _ in range(period)]
        pan = 0.3 + 0.4 * (k % 2)
        start = k * step
        length = int(1.3 * RATE)
        for i in range(length):
            j = i % period
            v = buf[j]
            buf[j] = 0.996 * 0.5 * (buf[j] + buf[(j + 1) % period])     # Karplus-Strong
            if start + i < n:
                left[start + i] += 0.8 * v * (1 - pan)
                right[start + i] += 0.8 * v * pan
    write_stereo(path, left, right)


SAMPLES = [("test_pad.wav", test_pad), ("test_drums.wav", test_drums), ("test_pluck.wav", test_pluck)]

if __name__ == "__main__":
    d = os.path.join(ROOT, "samples")
    os.makedirs(d, exist_ok=True)
    for name, make in SAMPLES:
        p = os.path.join(d, name)
        if not os.path.exists(p):
            make(p)
            print("Generated:", p)
