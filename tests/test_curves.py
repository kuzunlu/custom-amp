import unittest

import numpy as np

from ampsim.blocks import Gain
from tests.common import SR, harmonics, run, second_half, sine


def second_and_third(cfg, amplitude):
    y = second_half(run([dict(type="gain", **cfg)], sine(220, amplitude, 1.0)))
    h = harmonics(y, 220, count=3)
    return 100 * h[1] / h[0], 100 * h[2] / h[0]


class Triode(unittest.TestCase):
    def test_12ax7_operating_point_matches_tube_data(self):
        info = Gain({"type": "gain", "curve": "triode"}, SR).tube_info  # 300 V, 100k, -1.5 V
        self.assertAlmostEqual(info["plate_v"], 205, delta=3)     # real tube data: about 190-205 V
        self.assertAlmostEqual(info["plate_ma"], 0.95, delta=0.03)
        self.assertAlmostEqual(info["gain"], 61, delta=1.5)       # real tube data: about 60

    def test_harmonics(self):
        second, third = second_and_third({"curve": "triode"}, 0.5)
        self.assertAlmostEqual(second, 2.6, delta=0.4)
        self.assertLess(third, 0.3)
        cold_second, _ = second_and_third({"curve": "triode", "bias_v": -2.5}, 2.0)
        self.assertAlmostEqual(cold_second, 24, delta=4)

    def test_bias_shift_starts_at_grid_current(self):
        lowest = {}
        for peak in (1.2, 1.8):
            g = Gain({"type": "gain", "curve": "triode", "bias_shift": 1.0}, SR)
            lowest[peak] = g.track_shift(sine(220, peak, 0.25)).min()
        self.assertGreater(lowest[1.2], -1e-6)
        self.assertLess(lowest[1.8], -0.01)


class Curves(unittest.TestCase):
    def test_custom_curve_has_its_own_top_and_bottom(self):
        g = Gain({"type": "gain", "curve": "custom", "top": 0.5, "top_softness": 0.2, "bottom": 2.0, "bottom_softness": 0.8}, SR)
        self.assertAlmostEqual(float(g.curve(np.array(0.001))) / 0.001, 1.0, delta=0.02)  # slope 1 in the middle
        self.assertAlmostEqual(float(g.curve(np.array(100.0))), 0.5, delta=0.01)          # flat at the top
        self.assertAlmostEqual(float(g.curve(np.array(-100.0))), -2.0, delta=0.02)        # and at the bottom

    def test_loop_and_normal_give_the_same(self):
        x = sine(220, 2.0)
        for cfg in ({"curve": "triode"}, {"curve": "custom", "top": 0.7, "bottom": 1.5}):
            with self.subTest(curve=cfg["curve"]):
                a = run([{"type": "feedback", "amount": 0, "chain": [dict(type="gain", **cfg)]}], x)
                b = run([dict(type="gain", oversample=4, **cfg)], x)
                self.assertLess(np.max(np.abs(a - b)[48:-48]), 0.005 * np.max(np.abs(b)))

    def test_mistakes_are_errors(self):
        for cfg in ({"top": 0.5}, {"curve": "custom", "bias_v": -2}, {"curve": "triode", "bias_v": 0.5},
                    {"curve": "triode", "tube": "EL34"}, {"curve": "custom", "top_softness": 2}, {"curve": "nope"}):
            with self.subTest(cfg=cfg), self.assertRaises(ValueError):
                Gain(dict(type="gain", **cfg), SR)


if __name__ == "__main__":
    unittest.main()
