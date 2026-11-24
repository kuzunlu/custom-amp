import unittest

import numpy as np

from ampsim.chain import Amp
from tests.common import SR


class Sag(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        t = np.arange(int(1.6 * SR)) / SR
        cls.x = np.where((t >= 0.1) & (t < 0.6), 2.0, 0.2) * np.sin(2 * np.pi * 1000 * t)  # loud burst, then quiet

    def build(self, sag):
        chain = [{"type": "gain", "id": "p", "curve": "hard"}]
        if sag:
            chain.insert(0, {"type": "sag", "amount": 0.5, "attack_ms": 5, "release_ms": 150, "targets": ["p"]})
        amp = Amp({"chain": chain}, SR)
        return amp, amp.process(self.x)

    def test_headroom_drops_fast_and_comes_back_slowly(self):
        amp, _ = self.build(True)
        h = amp.blocks[1].headroom
        at = lambda seconds: h[int(seconds * SR)]
        self.assertAlmostEqual(at(0.099), 0.91, delta=0.03)  # quiet: a little sag only
        self.assertLess(at(0.12), 0.6)                      # 20 ms into the burst: measured 0.54
        self.assertGreater(at(0.6 + 0.6), 0.85)             # 600 ms after: almost back

    def test_quiet_sound_is_not_changed(self):
        _, y0 = self.build(False)
        _, y1 = self.build(True)
        self.assertLess(np.max(np.abs(y0[int(1.2 * SR):] - y1[int(1.2 * SR):])), 1e-9)

    def test_mistakes_are_errors(self):
        for chain, words in [
            ([{"type": "gain", "id": "p"}, {"type": "sag", "targets": ["p"]}], "must come after"),
            ([{"type": "sag", "targets": ["nope"]}], "no block has the id"),
            ([{"type": "sag", "targets": ["e"]}, {"type": "eq", "id": "e"}], "not a gain or power block"),
            ([{"type": "sag", "amount": 1.0}], "amount"),
        ]:
            with self.subTest(words=words), self.assertRaisesRegex(ValueError, words):
                Amp({"chain": chain}, SR)


if __name__ == "__main__":
    unittest.main()
