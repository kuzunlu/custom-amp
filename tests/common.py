"""Small helpers shared by the tests: test tones and measuring them."""
from pathlib import Path

import numpy as np

from ampsim.chain import Amp

ROOT = Path(__file__).resolve().parent.parent
SR = 48000


def sine(freq, amplitude, seconds=0.5, sr=SR):
    return amplitude * np.sin(2 * np.pi * freq * np.arange(int(seconds * sr)) / sr)


def tone(y, freq, sr=SR):
    """Amplitude of one frequency in y (y should hold whole-ish cycles; the tests use long pieces)."""
    t = np.arange(len(y)) / sr
    return 2 * abs(np.mean(y * np.exp(-2j * np.pi * freq * t)))


def second_half(y):
    return y[len(y) // 2:]


def harmonics(y, freq, count=8, sr=SR):
    """Amplitudes of the note and its harmonics (1st = the note itself)."""
    return [tone(y, freq * k, sr) for k in range(1, count + 1)]


def distortion(y, freq, sr=SR):
    """All harmonics together, in % of the note."""
    h = harmonics(y, freq, sr=sr)
    return 100 * np.sqrt(sum(v * v for v in h[1:])) / h[0]


def run(chain, x, sr=SR):
    return Amp({"chain": chain}, sr).process(x)


def db(v):
    return 20 * np.log10(v + 1e-12)
