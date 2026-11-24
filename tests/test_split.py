import unittest

import numpy as np

from ampsim.chain import Amp
from tests.common import SR, run, sine, tone


class Split(unittest.TestCase):
    x = sine(440, 0.3)

    def split(self, paths, **settings):
        return run([{"type": "split", "paths": paths, **settings}], self.x)

    def test_paths_add_up(self):
        self.assertLess(np.max(np.abs(self.split([{}, {}]) - 2 * self.x)), 1e-12)
        self.assertLess(np.max(np.abs(self.split([{}, {"invert": True}]))), 1e-12)
        gain = np.max(np.abs(self.split([{"level_db": -6}, {"level_db": -60}]))) / 0.3
        self.assertAlmostEqual(gain, 10 ** (-6 / 20) + 10 ** (-60 / 20), delta=1e-6)

    def test_a_path_equals_the_same_done_by_hand(self):
        eq = {"type": "eq", "bands": [{"kind": "lowpass", "freq": 300}]}
        by_hand = run([eq], self.x) + self.x
        self.assertLess(np.max(np.abs(self.split([{"chain": [eq]}, {}]) - by_hand)), 1e-12)

    def test_nested_split_with_feedback_matches_theory(self):
        inner = [{"chain": [{"type": "split", "paths": [{}, {"chain": [
            {"type": "feedback", "amount": 0.5, "chain": [{"type": "eq", "level_db": 6}]}]}]}]}]
        g = tone(self.split(inner), 440) / 0.3
        two = 10 ** (6 / 20)
        self.assertAlmostEqual(g, 1 + two / (1 + 0.5 * two), delta=1e-3)  # measured 1.9988

    def test_sag_reaches_a_gain_inside_a_path(self):
        loud = sine(1000, 2.0)

        def peak(sag):
            chain = [{"type": "split", "paths": [{"chain": [{"type": "gain", "id": "a", "curve": "hard"}]}]}]
            if sag:
                chain.insert(0, {"type": "sag", "amount": 0.5, "targets": ["a"]})
            return np.max(np.abs(run(chain, loud)[SR // 4:]))

        self.assertLess(peak(True), 0.8 * peak(False))

    def test_mistakes_are_errors(self):
        for chain, words in [
            ([{"type": "split", "paths": [{"chain": [{"type": "gain", "id": "g"}]}, {"chain": [{"type": "gain", "id": "g"}]}]}], "two blocks"),
            ([{"type": "split", "paths": []}], "path"),
            ([{"type": "split", "paths": [{"volume": 1}]}], "unknown setting"),
            ([{"type": "split", "paths": [{"chain": [{"type": "nope"}]}]}], "nope"),
        ]:
            with self.subTest(words=words), self.assertRaisesRegex(ValueError, words):
                Amp({"chain": chain}, SR)


if __name__ == "__main__":
    unittest.main()
