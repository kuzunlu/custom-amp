import unittest

import numpy as np

from ampsim.blocks import Power
from ampsim.curves import PENTODES, pentode_current
from tests.common import SR, distortion, harmonics, run, second_half, sine

BASE = dict(tube="EL34", plate_v=450, bias_v=-38, raa_ohms=3400)


def power(cfg, x):
    return run([dict(type="power", **cfg)], x)


class PowerTubes(unittest.TestCase):
    def test_fitted_tubes_hit_their_datasheet_points(self):
        sheet = {"EL34": (-13.5, 100, 11.0), "6L6GC": (-14.0, 72, 6.0), "EL84": (-7.3, 48, 11.3), "6V6GT": (-12.5, 45, 4.1)}
        for tube, (grid, ma, ma_per_v) in sheet.items():
            with self.subTest(tube=tube):
                i = pentode_current(np.array([grid - 1e-3, grid, grid + 1e-3]), 250, 250, **PENTODES[tube])
                self.assertAlmostEqual(1000 * i[1], ma, delta=0.005 * ma)
                self.assertAlmostEqual((i[2] - i[0]) / 2e-3 * 1000, ma_per_v, delta=0.01 * ma_per_v)

    def test_output_power_of_typical_amps(self):
        for cfg, watts in [(dict(tube="EL34", plate_v=450, bias_v=-38, raa_ohms=3400), 48),
                           (dict(tube="6L6GC", plate_v=460, bias_v=-50, raa_ohms=4200), 45),
                           (dict(tube="EL84", plate_v=320, bias_v=-10, raa_ohms=8000), 17),
                           (dict(tube="6V6GT", plate_v=420, bias_v=-35, raa_ohms=8000), 22)]:
            with self.subTest(tube=cfg["tube"]):
                self.assertAlmostEqual(Power(dict(type="power", **cfg), SR).tube_info["max_watts"], watts, delta=2)

    def test_push_pull_cancels_even_harmonics_unless_mismatched(self):
        second = {}
        for mismatch in (0.0, 0.1):
            h = harmonics(second_half(power(dict(BASE, mismatch=mismatch), sine(220, 30, 1.0))), 220, count=2)
            second[mismatch] = 100 * h[1] / h[0]
        self.assertLess(second[0.0], 0.01)
        self.assertAlmostEqual(second[0.1], 0.95, delta=0.25)

    def test_cold_bias_gives_crossover_distortion(self):
        x = sine(220, 10, 1.0)
        warm, cold = (distortion(second_half(power(dict(BASE, bias_v=b), x)), 220) for b in (-38, -45))
        self.assertLess(3 * warm, cold)  # measured 0.24% vs 1.61%

    def test_bias_shift_after_a_hard_hit(self):
        t = np.arange(int(0.6 * SR)) / SR
        loud_soft = np.where(t < 0.3, 80.0, 8.0) * np.sin(2 * np.pi * 220 * t)
        soft = np.where(t < 0.3, 0.0, 8.0) * np.sin(2 * np.pi * 220 * t)
        cfg = dict(BASE, bias_shift=0.5, shift_release_ms=100)
        after, alone = power(cfg, loud_soft), power(cfg, soft)
        window = lambda y, a, b: distortion(y[int(a * SR):int(b * SR)], 220)
        self.assertGreater(window(after, 0.30, 0.34), 1.3 * window(alone, 0.30, 0.34))  # measured 5.4% vs 3.1%

    def test_loop_and_normal_give_the_same(self):
        x = sine(220, 20)
        a = run([{"type": "feedback", "amount": 0, "chain": [dict(type="power", **BASE)]}], x)
        b = run([dict(type="power", oversample=4, **BASE)], x)
        self.assertLess(np.max(np.abs(a - b)[48:-48]), 0.005 * np.max(np.abs(b)))

    def test_mistakes_are_errors(self):
        for cfg in (dict(tube="KT88"), dict(bias_v=5), dict(mismatch=1.5)):
            with self.subTest(cfg=cfg), self.assertRaises(ValueError):
                Power(dict(type="power", **cfg), SR)


if __name__ == "__main__":
    unittest.main()
