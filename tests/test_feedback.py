import unittest

import numpy as np

from ampsim.chain import Amp
from ampsim.filters import Biquad
from tests.common import SR, distortion, run, second_half, sine, tone


def loop(amount, path=(), inner=None):
    inner = inner or [{"type": "eq", "level_db": 20}]  # a clean 10x gain
    return Amp({"chain": [{"type": "feedback", "amount": amount, "path": list(path), "chain": inner}]}, SR)


class Feedback(unittest.TestCase):
    def test_linear_loop_matches_theory(self):
        # gain 10, amount 0.1: 10 / (1 + 10 * 0.1) = 5
        for f in (200, 1000, 5000):
            with self.subTest(freq=f):
                g = tone(second_half(loop(0.1).process(sine(f, 0.01))), f) / 0.01
                self.assertAlmostEqual(g, 5.0, delta=0.005)

    def test_presence_shelf_in_the_path_matches_theory(self):
        path = [{"kind": "highshelf", "freq": 3000, "gain_db": -12}]
        bq = Biquad("highshelf", 3000, SR * 4, 0.707, -12)  # the loop runs at 4x
        for f in (200, 8000):
            with self.subTest(freq=f):
                z = np.exp(1j * 2 * np.pi * f / (SR * 4))
                h = (bq.b0 + bq.b1 / z + bq.b2 / z ** 2) / (1 + bq.a1 / z + bq.a2 / z ** 2)
                theory = abs(10 / (1 + 10 * 0.1 * h))  # 5.00 at 200 Hz, 8.05 at 8 kHz
                g = tone(second_half(loop(0.1, path).process(sine(f, 0.01))), f) / 0.01
                self.assertAlmostEqual(g, theory, delta=0.01 * theory)

    def test_amount_0_equals_a_plain_gain_stage(self):
        x = sine(440, 0.8, 1.0)
        cfg = {"type": "gain", "drive_db": 12, "curve": "tube", "bias": 0.1}
        a = run([{"type": "feedback", "amount": 0, "chain": [cfg]}], x)
        b = run([dict(cfg, oversample=4)], x)
        edge = SR // 1000  # only the first and last 1 ms differ (the resampling filters' start and end)
        self.assertLess(np.max(np.abs(a - b)[edge:-edge]), 1e-3 * np.max(np.abs(b)))

    def test_feedback_lowers_distortion_at_the_same_output_level(self):
        x = sine(220, 0.4, 1.0)
        stage = [{"type": "gain", "drive_db": 20, "curve": "soft"}]
        with_fb = loop(0.5, inner=stage).process(x)
        target = tone(second_half(with_fb), 220)
        lo, hi = 0.0, 1.0  # find the input level that gives the same output without feedback
        for _ in range(30):
            mid = (lo + hi) / 2
            without = loop(0, inner=stage).process(x * mid)
            lo, hi = (mid, hi) if tone(second_half(without), 220) < target else (lo, mid)
        d_with, d_without = distortion(second_half(with_fb), 220), distortion(second_half(without), 220)
        self.assertLess(d_with * 3, d_without)  # measured 0.83% vs 3.99%

    def test_strong_loop_is_stable(self):
        # loop gain 63 (amount 2, 30 dB): a 1-sample delay would oscillate; the solver must not
        y = loop(2.0, inner=[{"type": "gain", "drive_db": 30, "curve": "hard"}]).process(sine(220, 0.1, 1.0))
        theory = 0.1 * 10 ** (30 / 20) / (1 + 2 * 10 ** (30 / 20))
        self.assertTrue(np.all(np.isfinite(y)))
        self.assertAlmostEqual(tone(second_half(y), 220), theory, delta=0.01 * theory)

    def test_sag_outside_reaches_a_gain_inside(self):
        loud = sine(1000, 2.0)

        def peak(sag):
            chain = [{"type": "feedback", "amount": 0.2, "chain": [{"type": "gain", "id": "pwr", "curve": "hard"}]}]
            if sag:
                chain.insert(0, {"type": "sag", "amount": 0.5, "targets": ["pwr"]})
            return np.max(np.abs(run(chain, loud)[SR // 4:]))

        self.assertLess(peak(True), 0.8 * peak(False))

    def test_positive_feedback_matches_theory(self):
        # gain 2 inside, amount -0.25 / -0.4: 2 / (1 - 0.5) = 4, 2 / (1 - 0.8) = 10
        two = [{"type": "eq", "level_db": 20 * np.log10(2)}]
        for amount, theory in ((-0.25, 4.0), (-0.4, 10.0)):
            with self.subTest(amount=amount):
                g = tone(second_half(loop(amount, inner=two).process(sine(1000, 0.01))), 1000) / 0.01
                self.assertAlmostEqual(g, theory, delta=0.002 * theory)

    def test_positive_feedback_with_clipping_stays_stable(self):
        stage = [{"type": "gain", "drive_db": 6, "curve": "soft"}]  # steepest point: about 2
        y = loop(-0.4, inner=stage).process(sine(220, 1.0, 1.0))
        self.assertTrue(np.all(np.isfinite(y)))
        small = tone(second_half(loop(-0.4, inner=stage).process(sine(220, 0.001, 1.0))), 220) / 0.001
        two = 10 ** (6 / 20)
        theory = two / (1 - 0.4 * two)  # small signals: the linear theory (9.87); the stage's 10 Hz high-pass shifts
        self.assertAlmostEqual(small, theory, delta=0.05 * theory)  # the phase a little, measured 9.50

    def test_positive_feedback_with_loop_gain_1_or_more_is_an_error(self):
        two = [{"type": "eq", "level_db": 20 * np.log10(2)}]
        with self.assertRaisesRegex(ValueError, "needs a loop gain below 1, but this loop can reach 1.20"):
            loop(-0.6, inner=two)
        with self.assertRaisesRegex(ValueError, "loop gain below 1"):
            loop(-0.1, inner=[{"type": "gain", "drive_db": 30, "curve": "hard"}])

    def test_mistakes_are_errors(self):
        for chain, words in [
            ([{"type": "feedback", "chain": [{"type": "ir", "file": "x.wav"}]}], "ir"),
            ([{"type": "feedback", "chain": [{"type": "gain", "oversample": 8}]}], "oversample"),
            ([{"type": "feedback", "chain": [{"type": "gain", "id": "g"}]}, {"type": "sag", "targets": ["g"]}], "must come after"),
        ]:
            with self.subTest(words=words), self.assertRaisesRegex(ValueError, words):
                Amp({"chain": chain}, SR)


if __name__ == "__main__":
    unittest.main()
