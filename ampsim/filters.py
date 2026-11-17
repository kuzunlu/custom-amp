"""Basic filters: biquads (the building block of every EQ), FFT convolution and resampling."""
import math

import numpy as np

KINDS = ("highpass", "lowpass", "lowshelf", "highshelf", "peak")
BAND_SETTINGS = {"kind", "freq", "q", "gain_db"}


def check_keys(cfg, allowed, where):
    """Stop with a clear message when a preset has a typo in a setting name."""
    unknown = set(cfg) - set(allowed)
    if unknown:
        raise ValueError(
            f"{where}: unknown setting(s) {', '.join(sorted(unknown))}. "
            f"Allowed: {', '.join(sorted(allowed))}"
        )


class Biquad:
    """One 2nd-order filter. Formulas from the RBJ "Audio EQ Cookbook".

    It keeps its state between calls, so audio can be fed in pieces.
    """

    def __init__(self, kind, freq, sr, q=0.707, gain_db=0.0):
        if kind not in KINDS:
            raise ValueError(f"unknown filter kind '{kind}'. Use one of: {', '.join(KINDS)}")
        if not 0 < freq < sr / 2:
            raise ValueError(f"filter freq {freq} Hz must be between 0 and {sr / 2:g} Hz")

        w0 = 2 * math.pi * freq / sr
        cos_w, sin_w = math.cos(w0), math.sin(w0)
        alpha = sin_w / (2 * q)
        a = 10 ** (gain_db / 40)
        sq = 2 * math.sqrt(a) * alpha

        if kind == "lowpass":
            b = ((1 - cos_w) / 2, 1 - cos_w, (1 - cos_w) / 2)
            den = (1 + alpha, -2 * cos_w, 1 - alpha)
        elif kind == "highpass":
            b = ((1 + cos_w) / 2, -(1 + cos_w), (1 + cos_w) / 2)
            den = (1 + alpha, -2 * cos_w, 1 - alpha)
        elif kind == "peak":
            b = (1 + alpha * a, -2 * cos_w, 1 - alpha * a)
            den = (1 + alpha / a, -2 * cos_w, 1 - alpha / a)
        elif kind == "lowshelf":
            b = (
                a * ((a + 1) - (a - 1) * cos_w + sq),
                2 * a * ((a - 1) - (a + 1) * cos_w),
                a * ((a + 1) - (a - 1) * cos_w - sq),
            )
            den = ((a + 1) + (a - 1) * cos_w + sq, -2 * ((a - 1) + (a + 1) * cos_w), (a + 1) + (a - 1) * cos_w - sq)
        else:  # highshelf
            b = (
                a * ((a + 1) + (a - 1) * cos_w + sq),
                -2 * a * ((a - 1) + (a + 1) * cos_w),
                a * ((a + 1) + (a - 1) * cos_w - sq),
            )
            den = ((a + 1) - (a - 1) * cos_w + sq, 2 * ((a - 1) - (a + 1) * cos_w), (a + 1) - (a - 1) * cos_w - sq)

        a0 = den[0]
        self.b0, self.b1, self.b2 = b[0] / a0, b[1] / a0, b[2] / a0
        self.a1, self.a2 = den[1] / a0, den[2] / a0
        self.z1 = self.z2 = 0.0

    def process(self, x):
        b0, b1, b2, a1, a2 = self.b0, self.b1, self.b2, self.a1, self.a2
        z1, z2 = self.z1, self.z2
        out = [0.0] * len(x)
        for i, v in enumerate(x.tolist()):
            y = b0 * v + z1
            z1 = b1 * v - a1 * y + z2
            z2 = b2 * v - a2 * y
            out[i] = y
        self.z1, self.z2 = z1, z2
        return np.array(out)

    def tick(self, v, commit=True):
        """One sample at a time, for use inside a feedback loop.

        commit=False only tries the value: the filter does not move on to the next sample.
        """
        y = self.b0 * v + self.z1
        if commit:
            self.z1 = self.b1 * v - self.a1 * y + self.z2
            self.z2 = self.b2 * v - self.a2 * y
        return y


class IIR:
    """A filter of any order from its coefficients (b, a with a[0] = 1), like Biquad but general."""

    def __init__(self, b, a):
        self.b = [float(v) for v in b]
        self.a = [float(v) for v in a]
        self.z = [0.0] * (len(self.a) - 1)

    def process(self, x):
        out = [0.0] * len(x)
        z = list(self.z)
        for i, v in enumerate(x.tolist()):
            out[i] = self.tick_with(z, v)
        self.z = z
        return np.array(out)

    def tick_with(self, z, v):
        """One sample, moving the state list z on."""
        b, a = self.b, self.a
        y = b[0] * v + z[0]
        for k in range(len(z) - 1):
            z[k] = b[k + 1] * v - a[k + 1] * y + z[k + 1]
        z[-1] = b[-1] * v - a[-1] * y
        return y

    def tick(self, v, commit=True):
        if commit:
            return self.tick_with(self.z, v)
        return self.b[0] * v + self.z[0]


def make_filters(bands, sr):
    """Build a list of Biquads from preset bands like {"kind": "peak", "freq": 700, "gain_db": -6, "q": 0.7}."""
    filters = []
    for band in bands:
        check_keys(band, BAND_SETTINGS, f"filter band {band}")
        filters.append(Biquad(band["kind"], band["freq"], sr, band.get("q", 0.707), band.get("gain_db", 0.0)))
    return filters


def run_filters(filters, x):
    for f in filters:
        x = f.process(x)
    return x


def fft_convolve(x, h):
    """Full convolution of x and h, done with the FFT (fast for long signals)."""
    n = len(x) + len(h) - 1
    nfft = 1 << (n - 1).bit_length()
    return np.fft.irfft(np.fft.rfft(x, nfft) * np.fft.rfft(h, nfft), nfft)[:n]


def resample_fft(x, sr_from, sr_to):
    """Change the sample rate of a short signal (like an IR) by cutting or padding its spectrum."""
    n_new = round(len(x) * sr_to / sr_from)
    spectrum = np.fft.rfft(x)
    new = np.zeros(n_new // 2 + 1, dtype=complex)
    m = min(len(spectrum), len(new))
    new[:m] = spectrum[:m]
    return np.fft.irfft(new, n_new) * n_new / len(x)
