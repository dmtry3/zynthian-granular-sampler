#!/usr/bin/python3
# Writes the generated test samples into samples/ (only the stdlib: runs on Zynthian as is).
# test_pad.wav: 6 s stereo pad, a slowly moving chord of detuned saws, good to hear grains on.
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


if __name__ == "__main__":
    d = os.path.join(ROOT, "samples")
    os.makedirs(d, exist_ok=True)
    p = os.path.join(d, "test_pad.wav")
    if not os.path.exists(p):
        test_pad(p)
        print("Generated:", p)
