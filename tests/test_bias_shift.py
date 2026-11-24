import unittest

import numpy as np

from ampsim.blocks import Gain
from tests.common import SR, harmonics, run


class BiasShift(unittest.TestCase):
    CFG = {"type": "gain", "curve": "tube", "bias_shift": 1.0, "shift_attack_ms": 1, "shift_release_ms": 50}

    @classmethod
    def setUpClass(cls):
        t = np.arange(int(0.6 * SR)) / SR
        cls.loud_then_soft = np.where(t < 0.3, 3.0, 0.8) * np.sin(2 * np.pi * 220 * t)
        cls.soft_alone = np.where(t < 0.3, 0.0, 0.8) * np.sin(2 * np.pi * 220 * t)
        cls.after = Gain(cls.CFG, SR).process(cls.loud_then_soft)
        cls.alone = Gain(cls.CFG, SR).process(cls.soft_alone)

    def window(self, y, start, end):
        h = harmonics(y[int(start * SR):int(end * SR)], 220, count=2)
        return h[0], 100 * h[1] / h[0]

    def test_a_soft_note_right_after_a_loud_part_is_quieter_and_dirtier(self):
        level_after, second_after = self.window(self.after, 0.30, 0.34)  # measured 0.47, 15.6%
        level_alone, second_alone = self.window(self.alone, 0.30, 0.34)  # measured 0.72, 3.1%
        self.assertLess(level_after, 0.8 * level_alone)
        self.assertGreater(second_after, 3 * second_alone)

    def test_it_is_back_to_normal_later(self):
        level_after, _ = self.window(self.after, 0.55, 0.59)
        level_alone, _ = self.window(self.alone, 0.55, 0.59)
        self.assertAlmostEqual(level_after, level_alone, delta=0.03 * level_alone)

    def test_loop_and_normal_give_the_same(self):
        a = run([{"type": "feedback", "amount": 0, "chain": [dict(self.CFG)]}], self.loud_then_soft)
        b = run([dict(self.CFG, oversample=4)], self.loud_then_soft)
        self.assertLess(np.max(np.abs(a - b)[48:-48]), 0.005 * np.max(np.abs(b)))  # measured 0.08%

    def test_out_of_range_is_an_error(self):
        with self.assertRaises(ValueError):
            Gain({"type": "gain", "bias_shift": 2}, SR)


if __name__ == "__main__":
    unittest.main()
