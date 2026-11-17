"""Make test audio until real files are available:

    audio/test_di.wav    a 10 s fake guitar riff (dry, no amp), made with the Karplus-Strong plucked-string method
    audio/fake_4x12.wav  a fake cabinet IR, made from a few filters

    python tools/make_test_audio.py
"""
import sys
from pathlib import Path

import numpy as np

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT))

from ampsim.filters import make_filters, run_filters  # noqa: E402
from ampsim.wavio import write_wav  # noqa: E402

SR = 48000

# (decay per string round trip, brightness 0..1, time between strings in a strum)
STYLES = {
    "ring": (0.998, 0.6, 0.012),
    "mute": (0.93, 0.3, 0.004),  # palm mute: dies fast and sounds dark
    "lead": (0.997, 0.7, 0.0),
}


def pluck(midi, hold, rng, decay, bright):
    """One plucked string. A noise burst runs around a delay loop that slowly smooths it out."""
    freq = 440 * 2 ** ((midi - 69) / 12)
    period = max(2, round(SR / freq - 0.5))
    n = int((hold + 0.03) * SR)
    y = np.zeros(n + 1)  # y[0] stays 0 so the loop below never reads before the start

    burst = rng.uniform(-1, 1, period)
    smooth = max(1, round((1 - bright) * 8))
    burst = np.convolve(burst, np.ones(smooth) / smooth, mode="same")
    y[1 : period + 1] = burst - burst.mean()

    # Each sample depends only on samples one period back, so a whole period is computed at once.
    for start in range(period + 1, n + 1, period):
        stop = min(start + period, n + 1)
        y[start:stop] = decay * 0.5 * (y[start - period : stop - period] + y[start - period - 1 : stop - period - 1])

    y = y[1:]
    fade = int(0.03 * SR)  # the hand stops the string
    y[-fade:] *= np.linspace(1, 0, fade)
    return y


def make_di(rng):
    e_major = [40, 47, 52, 56, 59, 64]
    e5, e5_mute = [40, 47, 52], [40, 47]
    g5, a5, d5, c5 = [43, 50, 55], [45, 52, 57], [50, 57, 62], [48, 55, 60]

    events = [(0.0, e_major, 2.0, "ring")]
    events += [(2.0 + i * 0.25, e5_mute, 0.25, "mute") for i in range(4)]
    events += [(3.0, g5, 0.5, "ring"), (3.5, a5, 0.5, "ring")]
    events += [(4.0 + i * 0.25, e5_mute, 0.25, "mute") for i in range(4)]
    events += [(5.0, d5, 0.5, "ring"), (5.5, c5, 0.5, "ring")]
    events += [(6.0 + i * 0.25, [m], 0.25, "lead") for i, m in enumerate([64, 67, 69, 71, 74, 76])]
    events += [(7.5, e5, 2.0, "ring")]

    out = np.zeros(10 * SR)
    for t0, notes, dur, style in events:
        decay, bright, strum = STYLES[style]
        for k, midi in enumerate(notes):
            offset = k * strum
            note = pluck(midi, dur - offset, rng, decay, bright) * rng.uniform(0.8, 1.0)
            i = int((t0 + offset) * SR)
            note = note[: len(out) - i]
            out[i : i + len(note)] += note
    return out * (0.5 / np.max(np.abs(out)))  # peak at about -6 dBFS, like a real DI track


def make_fake_ir():
    """Rough 4x12 shape: low thump, mid dip, presence bump, steep top cut."""
    bands = [
        {"kind": "highpass", "freq": 70, "q": 0.9},
        {"kind": "peak", "freq": 110, "gain_db": 4, "q": 1.2},
        {"kind": "peak", "freq": 400, "gain_db": -2, "q": 1.0},
        {"kind": "peak", "freq": 1200, "gain_db": -3, "q": 2.0},
        {"kind": "peak", "freq": 2500, "gain_db": 5, "q": 1.5},
        {"kind": "lowpass", "freq": 5000, "q": 0.7},
        {"kind": "lowpass", "freq": 6000, "q": 0.7},
    ]
    impulse = np.zeros(2048)
    impulse[0] = 1.0
    ir = run_filters(make_filters(bands, SR), impulse)
    fade = len(ir) // 4
    ir[-fade:] *= 0.5 * (1 + np.cos(np.linspace(0, np.pi, fade)))
    return ir * (0.9 / np.max(np.abs(ir)))


def main():
    audio = ROOT / "audio"
    audio.mkdir(exist_ok=True)
    rng = np.random.default_rng(1)
    write_wav(audio / "test_di.wav", make_di(rng), SR)
    write_wav(audio / "fake_4x12.wav", make_fake_ir(), SR)
    print(f"Wrote {audio / 'test_di.wav'} and {audio / 'fake_4x12.wav'}")


if __name__ == "__main__":
    main()
