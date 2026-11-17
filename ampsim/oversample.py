"""Run a clipper at a higher sample rate.

Clipping creates many high harmonics. At the normal sample rate, the ones above
half the sample rate fold back down as harsh, out-of-tune noise (aliasing).
So we raise the rate, clip, filter away everything above the normal range, and lower the rate again.
"""
import numpy as np

from .filters import fft_convolve

TAPS_PER_PHASE = 32


def _lowpass_fir(factor):
    """Windowed-sinc low-pass. Passes up to 90% of the base-rate top frequency."""
    n = TAPS_PER_PHASE * factor + 1
    m = np.arange(n) - (n - 1) / 2
    cutoff = 0.45 / factor  # in cycles per sample at the high rate
    h = 2 * cutoff * np.sinc(2 * cutoff * m) * np.kaiser(n, 8.0)
    return h / h.sum()


def upsample(x, factor):
    """Raise the sample rate. Used by the feedback loop, which must run sample by sample."""
    if factor == 1:
        return x
    h = _lowpass_fir(factor)
    delay = (len(h) - 1) // 2
    up = np.zeros(len(x) * factor)
    up[::factor] = x * factor
    return np.convolve(up, h)[delay : delay + len(up)]


def downsample(y, factor):
    if factor == 1:
        return y
    h = _lowpass_fir(factor)
    delay = (len(h) - 1) // 2
    return np.convolve(y, h)[delay : delay + len(y)][::factor]


def oversampled(x, func, factor=8, chunk=1 << 15, ctrl=None):
    """Return func(x) computed at `factor` times the sample rate.

    func must work sample by sample with no memory (a clipping curve).
    If ctrl is given (one control value per sample, like headroom from sag), func is called as func(x, ctrl).
    The audio is done in chunks with some overlap, so long files do not use too much memory.
    """
    if factor == 1:
        return func(x) if ctrl is None else func(x, ctrl)

    h = _lowpass_fir(factor)
    delay = (len(h) - 1) // 2
    margin = len(h) // factor + 2  # base-rate samples each filter reaches on each side
    padded = np.concatenate([np.zeros(margin), x, np.zeros(margin)])
    if ctrl is not None:
        padded_ctrl = np.concatenate([np.full(margin, ctrl[0]), ctrl, np.full(margin, ctrl[-1])])
    out = np.empty(len(x))

    for start in range(0, len(x), chunk):
        stop = min(start + chunk, len(x))
        seg = padded[start : stop + 2 * margin]
        n_up = len(seg) * factor

        up = np.zeros(n_up)
        up[::factor] = seg * factor
        up = fft_convolve(up, h)[delay : delay + n_up]
        if ctrl is None:
            clipped = func(up)
        else:
            # Control values change slowly, so simply repeating each one is enough.
            clipped = func(up, np.repeat(padded_ctrl[start : stop + 2 * margin], factor))
        y = fft_convolve(clipped, h)[delay : delay + n_up][::factor]

        out[start:stop] = y[margin : margin + stop - start]
    return out
