import unittest

import numpy as np

from ampsim.blocks import CURVES
from ampsim.oversample import oversampled
from tests.common import SR


class Oversampling(unittest.TestCase):
    def fold_back_db(self, factor):
        """Hard clip of a 4.5 kHz tone: everything in 0-20 kHz that is not a real harmonic is fold-back noise."""
        n = SR
        y = oversampled(4 * np.sin(2 * np.pi * 4500 * np.arange(n) / SR), CURVES["hard"], factor)
        spec = np.abs(np.fft.rfft(y * np.hanning(n))) ** 2
        freqs = np.fft.rfftfreq(n, 1 / SR)
        harm = np.zeros_like(spec, bool)
        for k in range(1, 6, 2):
            harm |= np.abs(freqs - 4500 * k) < 30
        band = freqs < 20000
        return 10 * np.log10(spec[band & ~harm].sum() / spec[band & harm].sum())

    def test_8x_removes_most_fold_back_noise(self):
        at1, at8 = self.fold_back_db(1), self.fold_back_db(8)  # measured: -21 dB and -62 dB
        self.assertGreater(at1, -30)
        self.assertLess(at8, -55)

    def test_chunks_equal_one_piece(self):
        x = np.random.default_rng(0).standard_normal(100000)
        a = oversampled(x, np.tanh, 8, chunk=1 << 15)
        b = oversampled(x, np.tanh, 8, chunk=1 << 20)
        self.assertLess(np.max(np.abs(a - b)), 1e-12)


if __name__ == "__main__":
    unittest.main()
