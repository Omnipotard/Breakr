#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""BREAKR's eight factory breaks, then 4-bit IMA ADPCM like the firmware reads it. Two kinds:
  - recorded: assets/breaks/*.wav, drums played and recorded by Omnipotard, given to the public domain (CC0,
    assets/breaks/README.md). Their hits are found here (spectral flux, one per sixteenth at most, each start
    found again to the sample) and classed from what rose (lows: kick, mids and noise: snare, highs: hat);
  - synthesised (sines, noise, filters, a small room), played in classic break rhythms with a human feel.

  tools/gen_breaks.py                 -> firmware/gen/brk_breaks.h
  tools/gen_breaks.py --wav DIR       also writes each break as a WAV (to listen)
  tools/gen_breaks.py --json FILE     also the slices and classes (tests)

A synthesised break's hits are known, so its slices are exact: one at each sixteenth that has a hit, its
class from what plays there (K kick, S snare, H hat, - other). Needs numpy.
"""
import argparse
import json
import math
import struct
import sys
import wave
from pathlib import Path

import numpy as np
try:
    from scipy.signal import lfilter
except ImportError:                              # (slower without scipy, same result)
    def lfilter(b, a, x):
        b = [v / a[0] for v in b]
        a = [v / a[0] for v in a]
        y = np.zeros(len(x))
        for i in range(len(x)):
            acc = 0.0
            for k, bk in enumerate(b):
                if bk and i - k >= 0:
                    acc += bk * x[i - k]
            for k in range(1, len(a)):
                if a[k] and i - k >= 0:
                    acc -= a[k] * y[i - k]
            y[i] = acc
        return y

SR = 44100
SRC = Path(__file__).resolve().parents[1]
OUT = SRC / "firmware" / "gen" / "brk_breaks.h"

# ---- IMA ADPCM, blocks of 256 bytes (505 samples), as firmware/src/brk_ima.c
STEP = [7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45, 50, 55, 60, 66, 73, 80, 88, 97,
        107, 118, 130, 143, 157, 173, 190, 209, 230, 253, 279, 307, 337, 371, 408, 449, 494, 544, 598, 658, 724, 796,
        876, 963, 1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066, 2272, 2499, 2749, 3024, 3327, 3660, 4026, 4428, 4871,
        5358, 5894, 6484, 7132, 7845, 8630, 9493, 10442, 11487, 12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623,
        27086, 29794, 32767]
IDX = [-1, -1, -1, -1, 2, 4, 6, 8]
SPB = 505


def _q(pred, idx, s):
    """the standard IMA nibble for sample s"""
    d = s - pred
    step = STEP[idx]
    nib = 0
    if d < 0:
        nib = 8
        d = -d
    if d >= step:
        nib |= 4; d -= step
    if d >= step >> 1:
        nib |= 2; d -= step >> 1
    if d >= step >> 2:
        nib |= 1
    return nib


def _ap(pred, idx, nib):
    step = STEP[idx]
    diff = step >> 3
    if nib & 4: diff += step
    if nib & 2: diff += step >> 1
    if nib & 1: diff += step >> 2
    pred += -diff if nib & 8 else diff
    return max(-32768, min(32767, pred)), max(0, min(88, idx + IDX[nib & 7]))


def ima_encode(pcm, look=2):
    """IMA blocks; each nibble chosen among the 16 for the least error over it and the next `look` samples
    (decoded by the standard decoder: only the encoder is smarter, +1.5 dB on drums)"""
    out = bytearray()
    idx = 0
    n = len(pcm)
    pcm = [int(v) for v in pcm]
    for at in range(0, n, SPB):
        pred = pcm[at]
        blk = bytearray(256)
        struct.pack_into("<hBB", blk, 0, pred, idx, 0)
        end = min(n, at + SPB)
        for i in range(1, SPB):
            if at + i >= n:
                nib = 0
            else:
                best, nib = None, 0
                for c in range(16):
                    p, ix = _ap(pred, idx, c)
                    e = (p - pcm[at + i]) ** 2
                    for j in range(1, look + 1):
                        if at + i + j >= end:
                            break
                        p, ix = _ap(p, ix, _q(p, ix, pcm[at + i + j]))
                        e += (p - pcm[at + i + j]) ** 2
                    if best is None or e < best:
                        best, nib = e, c
            pred, idx = _ap(pred, idx, nib)
            j = 4 + ((i - 1) >> 1)
            blk[j] |= nib if i & 1 else nib << 4
        out += blk
    return bytes(out)


def ima_decode(data, frames):
    out = np.zeros(frames, np.int32)
    for b in range(0, len(data) // 256):
        blk = data[b * 256:(b + 1) * 256]
        pred, idx = struct.unpack_from("<hB", blk, 0)
        base = b * SPB
        if base < frames:
            out[base] = pred
        for i in range(1, SPB):
            if base + i >= frames:
                break
            nib = blk[4 + ((i - 1) >> 1)]
            nib = nib & 15 if i & 1 else nib >> 4
            step = STEP[idx]
            diff = step >> 3
            if nib & 4: diff += step
            if nib & 2: diff += step >> 1
            if nib & 1: diff += step >> 2
            pred += -diff if nib & 8 else diff
            pred = max(-32768, min(32767, pred))
            idx = max(0, min(88, idx + IDX[nib & 7]))
            out[base + i] = pred
    return out


# ---- the drums (44.1 kHz, float)
def env_exp(n, tau):
    return np.exp(-np.arange(n) / (tau * SR))


def onepole_lp(x, fc):
    a = math.exp(-2 * math.pi * fc / SR)
    return lfilter([1 - a], [1, -a], x)


def biquad(x, kind, fc, q=0.707):
    w = 2 * math.pi * fc / SR
    cw, sw = math.cos(w), math.sin(w)
    al = sw / (2 * q)
    if kind == "lp":
        b = [(1 - cw) / 2, 1 - cw, (1 - cw) / 2]
    elif kind == "hp":
        b = [(1 + cw) / 2, -(1 + cw), (1 + cw) / 2]
    else:  # band-pass, 0 dB peak
        b = [al, 0, -al]
    a = [1 + al, -2 * cw, 1 - al]
    return lfilter(b, a, x)


class Kit:
    def __init__(self, seed, kick_f=(160, 48), kick_tau=0.22, snare_tune=190, snare_tau=0.16, hat_tau=0.045,
                 tone=1.0, dust=0.0):
        self.rng = np.random.default_rng(seed)
        self.kick_f, self.kick_tau, self.snare_tune, self.snare_tau, self.hat_tau = kick_f, kick_tau, snare_tune, snare_tau, hat_tau
        self.tone, self.dust = tone, dust
        self.cache = {}

    def noise(self, n):
        return self.rng.standard_normal(n)

    def kick(self, v):
        n = int(0.45 * SR)
        t = np.arange(n) / SR
        f0, f1 = self.kick_f
        f = f1 + (f0 - f1) * np.exp(-t / 0.035)
        ph = 2 * np.pi * np.cumsum(f) / SR
        body = np.sin(ph) * env_exp(n, self.kick_tau)
        click = biquad(self.noise(n) * env_exp(n, 0.004), "hp", 1500) * 0.5
        x = np.tanh(1.6 * (body + click)) * v
        return x

    def snare(self, v, ghost=False):
        n = int(0.35 * SR)
        t = np.arange(n) / SR
        tau = self.snare_tau * (0.55 if ghost else 1.0)
        f = self.snare_tune * (1 + 0.5 * np.exp(-t / 0.01))
        tone = (np.sin(2 * np.pi * np.cumsum(f) / SR) * 0.6 + np.sin(2 * np.pi * np.cumsum(f * 1.74) / SR) * 0.35)
        tone *= env_exp(n, 0.05)
        nz = biquad(self.noise(n), "bp", 3800, 0.6) * 1.4 + biquad(self.noise(n), "hp", 6000) * 0.5
        nz *= env_exp(n, tau)
        crack = self.noise(n) * env_exp(n, 0.003) * 0.6
        x = np.tanh(1.3 * (tone + nz + crack)) * v
        return x

    def hat(self, v, open_=False):
        n = int((0.5 if open_ else 0.12) * SR)
        t = np.arange(n) / SR
        met = np.zeros(n)
        for fr in (205.3, 304.4, 369.6, 522.7, 540.0, 800.0):
            met += np.sign(np.sin(2 * np.pi * fr * 8.1 * t + self.rng.uniform(0, 6.28)))
        x = biquad(met * 0.18 + self.noise(n) * 0.9, "hp", 7000)
        x = biquad(x, "bp", 10000, 0.9) * 1.5 + x * 0.4
        x *= env_exp(n, 0.32 if open_ else self.hat_tau)
        return x * v

    def ride(self, v):
        n = int(0.9 * SR)
        t = np.arange(n) / SR
        met = np.zeros(n)
        for fr in (310.0, 456.0, 587.0, 742.0, 1120.0):
            met += np.sin(2 * np.pi * fr * 4.3 * t + self.rng.uniform(0, 6.28))
        x = biquad(met * 0.25 + self.noise(n) * 0.35, "hp", 3500) * env_exp(n, 0.45)
        x += biquad(self.noise(n), "hp", 6000) * env_exp(n, 0.01) * 0.5
        return x * v

    def bongo(self, v, hi=True):
        n = int(0.25 * SR)
        t = np.arange(n) / SR
        f0 = 420 if hi else 290
        f = f0 * (1 + 0.25 * np.exp(-t / 0.015))
        x = np.sin(2 * np.pi * np.cumsum(f) / SR) * env_exp(n, 0.09 if hi else 0.12)
        x += biquad(self.noise(n), "bp", f0 * 3, 1.0) * env_exp(n, 0.008) * 0.6
        return np.tanh(1.4 * x) * v


def room(x, mix=0.12, size=1.0):
    """a small room: four combs and two all-passes (Schroeder), dark"""
    y = np.zeros_like(x)
    for d, g in ((1116, 0.78), (1188, 0.77), (1277, 0.76), (1356, 0.75)):
        d = int(d * size)
        a = np.zeros(d + 1)
        a[0], a[1] = 1.0, -0.4
        a[d] += -0.6 * g
        y += lfilter([1.0, -0.4], a, x)          # a comb, damped (a low-pass in its loop)
    for d, g in ((556, 0.5), (441, 0.5)):
        b = np.zeros(d + 1)
        a = np.zeros(d + 1)
        b[0], b[d] = -g, 1.0
        a[0], a[d] = 1.0, -g
        y = lfilter(b, a, y)                     # an all-pass
    return x + mix * y / 4


# ---- the breaks: per bar, 16 sixteenths; a letter a hit: K kick, k soft kick, S snare, g ghost snare,
# H closed hat, h soft hat, O open hat, R ride, B bongo high, b bongo low; '.' nothing.
# Each line is one instrument over the bars (one character a sixteenth).
BREAKS = [
    dict(name="OMNI 142 A", wav="omni142a.wav", bpm=142, bars=2),
    dict(name="OMNI 142 B", wav="omni142b.wav", bpm=142, bars=2),
    dict(name="OMNI 72 A", wav="omni72a.wav", bpm=72, bars=1),
    dict(name="OMNI 72 B", wav="omni72b.wav", bpm=72, bars=1),
    dict(name="JUNGLE 170", bpm=170, bars=2, seed=11, swing=0.0,
         kit=dict(kick_f=(170, 52), kick_tau=0.18, snare_tune=215, snare_tau=0.13, hat_tau=0.04),
         lines=["K.K.......KK....K.K.......K.....",
                "....S..g.g..S..g....S..g.g..S.gS",
                "R.R.R.R.R.R.R.R.R.R.R.R.R.R.R.R.",
                "..h...h...h...h...h...h...h...h."]),
    dict(name="FUNK 100", bpm=100, bars=1, seed=12, swing=0.08,
         kit=dict(kick_f=(150, 46), kick_tau=0.26, snare_tune=185, snare_tau=0.17),
         lines=["K..k..K...K.....",
                "....S..g.g..S..g",
                "HhHhHhHhHhHhHhO."]),
    dict(name="THINKER 112", bpm=112, bars=1, seed=13, swing=0.05,
         kit=dict(kick_f=(165, 50), kick_tau=0.2, snare_tune=200, snare_tau=0.15),
         lines=["K.........K.....",
                "....S.g.....S...",
                "H.HhH.HhH.HhH.O."]),
    dict(name="BONGO 118", bpm=118, bars=1, seed=17, swing=0.06,
         kit=dict(kick_f=(160, 48), kick_tau=0.22, snare_tune=195, snare_tau=0.15),
         lines=["K.......K.......",
                "....S.......S...",
                "B.bB.bB.B.bB.bBb",
                "..h...h...h...h."]),
]
# kept for later (the factory set holds 8): synthesised breaks out of the set
SPARE = [
    dict(name="BOOMBAP 90", bpm=90, bars=1, seed=14, swing=0.16,
         kit=dict(kick_f=(140, 44), kick_tau=0.3, snare_tune=175, snare_tau=0.2, tone=0.9),
         lines=["K......K..K.....",
                "....S.......S...",
                "H.H.H.H.H.H.H.H."]),
    dict(name="NUSKOOL 128", bpm=128, bars=2, seed=15, swing=0.0,
         kit=dict(kick_f=(175, 50), kick_tau=0.21, snare_tune=210, snare_tau=0.14, hat_tau=0.035),
         lines=["K.....K...K.....K.....K...K..K..",
                "....S.......S.......S.......S.gg",
                "..O...O...O...O...O...O...O...O.",
                "h.h.h.h.h.h.h.h.h.h.h.h.h.h.h.h."]),
    dict(name="HALFTIME 85", bpm=85, bars=1, seed=16, swing=0.1,
         kit=dict(kick_f=(130, 42), kick_tau=0.34, snare_tune=170, snare_tau=0.22),
         lines=["K.........KK....",
                "...g....S.....g.",
                "H.HhH.HhH.HhH.Hh"]),
    dict(name="DUSTY 80", bpm=80, bars=1, seed=18, swing=0.18, rate=16000, dust=1,
         kit=dict(kick_f=(130, 45), kick_tau=0.28, snare_tune=180, snare_tau=0.19, tone=0.8),
         lines=["K.....K..K......",
                "....S.......S..g",
                "H.H.H.H.H.H.H.H."]),
]

VEL = {"K": 0.95, "k": 0.55, "S": 0.9, "g": 0.3, "H": 0.42, "h": 0.22, "O": 0.4, "R": 0.38, "B": 0.6, "b": 0.6}
CLS = {"K": 1, "k": 1, "S": 2, "g": 2, "H": 3, "h": 3, "O": 3, "R": 3, "B": 0, "b": 0}
PRIO = {1: 3, 2: 2, 3: 1, 0: 0}


ASSETS = SRC / "assets" / "breaks"


def read_wav(path):
    with wave.open(str(path), "rb") as w:
        assert w.getsampwidth() == 2 and w.getnchannels() == 1 and w.getframerate() == SR, path
        return np.frombuffer(w.readframes(w.getnframes()), "<i2").astype(np.float64) / 32768.0


def stft_bands(y, n=1024, hop=128):
    """magnitudes in three bands per frame: lows (<150 Hz), mids and noise (150 Hz..6 kHz), highs (>6 kHz)"""
    win = np.hanning(n)
    pad = np.concatenate([np.zeros(n // 2), y, np.zeros(n)])
    frames = (len(y) + hop - 1) // hop
    f = np.fft.rfftfreq(n, 1 / SR)
    lo, mid, hi = f < 150, (f >= 150) & (f < 6000), f >= 6000
    out = np.zeros((frames, 3))
    for t in range(frames):
        m = np.abs(np.fft.rfft(pad[t * hop:t * hop + n] * win))
        out[t] = [m[lo].sum(), m[mid].sum(), m[hi].sum()]
    return out, hop


def onset_at(y, at):
    """the sample a hit starts at, near at: where the energy (32-sample blocks) jumps the most"""
    lo, hi = max(64, at - 1024), min(len(y) - 64, at + 768)
    e = np.convolve(y * y, np.ones(32), mode="same") + 1e-12
    n = np.arange(lo, hi)
    jump = np.log(e[n + 32]) - np.log(e[n - 32])
    return int(n[int(np.argmax(jump))])


def detect(y, bpm, bars):
    """the hits of a recorded loop: (sixteenth, class, start in samples, '?') as render() gives them. Each
    sixteenth is looked at near its place (the drummer's timing kept): a hit where a band jumps (x2.5) to a
    real level; its start found again to the sample (onset_at); its class from the bands: lows rising (kick), mids over
    highs (snare), highs (hat)"""
    b, hop = stft_bands(y)
    bmax = b.max(axis=0) + 1e-9
    lb = np.log1p(b / bmax * 100)
    nov = np.maximum(0, np.diff(lb, axis=0, prepend=lb[:1])).sum(axis=1)
    six = 60.0 / bpm / 4 * SR
    env = np.abs(y)
    hits = []
    for i in range(16 * bars):
        lo_t = max(0, int((i * six - 0.3 * six) / hop))
        hi_t = min(len(nov) - 1, int((i * six + 0.45 * six) / hop))
        t = lo_t + int(np.argmax(nov[lo_t:hi_t + 1])) if i else 0
        after = b[t:t + 10].mean(axis=0)
        before = b[max(0, t - 10):max(1, t - 2)].mean(axis=0) + 1e-9
        real = after > 0.08 * bmax
        jump = (after / before) * real
        if i and (not (jump >= 2.5).any() or int(round(t * hop / six)) != i):
            continue                            # (a hit nearer another sixteenth is that one's)
        start = 0 if i == 0 else onset_at(y, t * hop)
        tot = after.sum() + 1e-9
        if after[0] / tot > 0.3 and (i == 0 or jump[0] >= 3):
            cl = 1
        elif after[1] > 1.3 * after[2]:
            cl = 2
        else:
            cl = 3
        hits.append((i, cl, start, "?"))
    return hits


def render(spec):
    if "wav" in spec:
        y = read_wav(ASSETS / spec["wav"])
        y = y / (np.max(np.abs(y)) + 1e-9) * 0.89
        return y, detect(y, spec["bpm"], spec["bars"]), len(y)
    kit = Kit(spec["seed"], **spec["kit"])
    rng = np.random.default_rng(spec["seed"] + 100)
    bpm, bars = spec["bpm"], spec["bars"]
    six = 60.0 / bpm / 4
    n16 = 16 * bars
    loop = int(round(n16 * six * SR))
    total = loop * 2                             # two loops: the room's tail of the first wraps into the second
    mixb = np.zeros(total + SR)
    hits = []                                    # (sixteenth, class)
    for cycle in range(2):
        for line in spec["lines"]:
            for i, ch in enumerate(line[:n16]):
                if ch == ".":
                    continue
                late = spec["swing"] * six if i % 2 else 0.0
                jitter = rng.normal(0, 0.0025) if ch not in "KS" else rng.normal(0, 0.0012)
                at = int((cycle * n16 * six + i * six + late + jitter) * SR)
                at = max(0, at)
                v = VEL[ch] * rng.uniform(0.88, 1.06)
                x = {"K": lambda: kit.kick(v), "k": lambda: kit.kick(v), "S": lambda: kit.snare(v),
                     "g": lambda: kit.snare(v, ghost=True), "H": lambda: kit.hat(v), "h": lambda: kit.hat(v),
                     "O": lambda: kit.hat(v, open_=True), "R": lambda: kit.ride(v), "B": lambda: kit.bongo(v, True),
                     "b": lambda: kit.bongo(v, False)}[ch]()
                mixb[at:at + len(x)] += x[:len(mixb) - at]
                if cycle == 1:
                    hits.append((i, CLS[ch], at - loop, ch))
    y = room(mixb[:total], mix=0.16 if spec.get("dust") else 0.12)
    y = y[loop:loop + loop]                      # the second loop: seamless
    y = np.tanh(y * 1.2) / 1.2
    if spec.get("dust"):
        y = onepole_lp(y, 6500)
        crackle = (rng.random(len(y)) < 0.0007) * rng.normal(0, 0.25, len(y))
        y = y + onepole_lp(crackle, 3000) + rng.normal(0, 0.004, len(y))
    y = y / (np.max(np.abs(y)) + 1e-9) * 0.89
    return y, hits, loop


def resample(y, rate):
    if rate == SR:
        return y
    n = int(round(len(y) * rate / SR))
    # band-limit then linear interpolation (enough for drums at 22 kHz)
    yl = biquad(biquad(y, "lp", rate * 0.45), "lp", rate * 0.45)
    t = np.arange(n) * (SR / rate)
    return np.interp(t, np.arange(len(yl)), yl)


def slices(hits, n16, frames, rate, loop):
    """one slice at each sixteenth with a hit; its start at the earliest hit there, its class the strongest"""
    by = {}
    for i, cl, at, ch in hits:
        a = max(0, int(at * rate / SR) - int(0.0015 * rate))
        if i not in by:
            by[i] = [a, cl]
        else:
            by[i][0] = min(by[i][0], a)
            if PRIO[cl] > PRIO[by[i][1]]:
                by[i][1] = cl
    keys = sorted(by)
    sl = [0 if k == keys[0] else by[k][0] for k in keys]
    sl[0] = 0
    cls = [by[k][1] for k in keys]
    return sl[:32], cls[:32]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--wav", type=Path)
    ap.add_argument("--json", type=Path)
    a = ap.parse_args()
    out = ["/* SPDX-License-Identifier: GPL-3.0-only */",
           "/* BREAKR's factory breaks: made by tools/gen_breaks.py (4 recorded by Omnipotard, CC0; 4 synthesised). Do not edit. */",
           "typedef struct { const char *name; const uint8_t *data; uint32_t frames, rate, bpm10; uint8_t bars, nsl;",
           "                 uint32_t sl[32]; uint8_t cls[32]; } fact_t;"]
    table, meta, total = [], [], 0
    for k, spec in enumerate(BREAKS):
        y, hits, loop = render(spec)
        if "wav" in spec:
            print(f"{spec['name']:12s} hits " + " ".join(f"{i}{'-KSH'[c]}" for i, c, _, _ in hits), file=sys.stderr)
        rate = spec.get("rate", 22050)
        z = resample(y, rate)
        pcm = np.clip(np.round(z * 32767), -32768, 32767).astype(np.int32)
        data = ima_encode(pcm)
        frames = len(pcm)
        sl, cls = slices(hits, 16 * spec["bars"], frames, rate, loop)
        dec = ima_decode(data, frames)
        snr = 10 * math.log10(np.sum(pcm.astype(float) ** 2) / (np.sum((pcm - dec).astype(float) ** 2) + 1))
        total += len(data)
        print(f"{spec['name']:12s} {frames:6d} frames {rate} Hz {len(data):6d} B  {len(sl):2d} slices  IMA SNR {snr:.1f} dB",
              file=sys.stderr)
        if a.wav:
            a.wav.mkdir(parents=True, exist_ok=True)
            with wave.open(str(a.wav / f"{k + 1:02d}-{spec['name'].replace(' ', '_')}.wav"), "wb") as w:
                w.setnchannels(1); w.setsampwidth(2); w.setframerate(rate)
                w.writeframes(np.clip(dec, -32768, 32767).astype("<i2").tobytes())
        arr = ", ".join(str(b) for b in data)
        out.append(f"static const uint8_t FACT{k}[{len(data)}] = {{{arr}}};")
        slp = sl + [0] * (32 - len(sl))
        clp = cls + [0] * (32 - len(cls))
        table.append(f'    {{"{spec["name"]}", FACT{k}, {frames}u, {rate}u, {spec["bpm"] * 10}u, {spec["bars"]}, {len(sl)},\n'
                     f'     {{{", ".join(str(v) for v in slp)}}},\n     {{{", ".join(str(v) for v in clp)}}}}},')
        meta.append(dict(name=spec["name"], frames=frames, rate=rate, bpm=spec["bpm"], bars=spec["bars"], sl=sl, cls=cls))
    out.append(f"static const fact_t FACT[{len(BREAKS)}] = {{")
    out += table
    out.append("};")
    OUT.write_text("\n".join(out) + "\n")
    print(f"-> {OUT}  ({total} B of sound)", file=sys.stderr)
    if a.json:
        a.json.write_text(json.dumps(meta, indent=1))


if __name__ == "__main__":
    main()
