import unittest

import numpy as np

from ampsim.blocks import Gate
from ampsim.chain import Amp
from tests.common import SR, db, run


def rms(y, start, end):
    return np.sqrt(np.mean(y[int(start * SR):int(end * SR)] ** 2))


class GateTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        rng = np.random.default_rng(0)
        t = np.arange(SR) / SR
        hiss = 10 ** (-70 / 20) * rng.standard_normal(SR)
        cls.x = hiss + np.where((t >= 0.2) & (t < 0.6), 0.1, 0.0) * np.sin(2 * np.pi * 196 * t)  # -70 dB hiss, -23 dB note
        # a note fading through the threshold, with -70 dB hiss (the exact count of changes depends on the hiss: fixed seed)
        cls.fade = (10 ** (np.linspace(-40, -70, SR) / 20) * np.sin(2 * np.pi * 196 * t)
                    + 10 ** (-70 / 20) * np.random.default_rng(0).standard_normal(SR))

    def test_hiss_goes_the_note_stays(self):
        gate = {"type": "gate", "threshold_db": -50, "hold_ms": 50, "release_ms": 20}
        y = run([gate], self.x)
        self.assertLess(db(rms(y, 0, 0.2)), -120)  # measured -150 dB
        self.assertAlmostEqual(db(rms(y, 0.3, 0.5)), db(rms(self.x, 0.3, 0.5)), delta=0.1)
        opened = np.argmax(Gate(gate, SR).gains(self.x) > 0.5) / SR
        self.assertLess(opened - 0.2, 0.001)  # measured 0.3 ms

    def test_hysteresis_stops_chatter(self):
        changes = {}
        for hysteresis in (0, 3, 6):
            g = Gate({"type": "gate", "threshold_db": -55, "hysteresis_db": hysteresis, "hold_ms": 0, "release_ms": 5}, SR).gains(self.fade)
            changes[hysteresis] = np.count_nonzero(np.diff((g > 0.5).astype(int)))
        self.assertGreaterEqual(changes[0], 4)  # chatter (measured 6)
        self.assertEqual(changes[3], 2)         # opens once, closes once
        self.assertLessEqual(changes[6], 1)     # closes too late: the closing point is under the hiss peaks

    def test_gate_at_the_input_works_on_a_high_gain_amp(self):
        amp = [{"type": "gain", "id": "v1", "drive_db": 40, "curve": "tube"}, {"type": "gain", "id": "v2", "drive_db": 20, "curve": "soft"}]
        after = run(amp + [{"type": "gate", "threshold_db": -50, "release_ms": 20}], self.x)
        at_input = run([{"type": "gate", "threshold_db": -50, "release_ms": 20, "apply_after": "v2"}] + amp, self.x)
        self.assertGreater(db(rms(after, 0.8, 1.0)), -30)    # placed after the amp it never closes (-11.5 dB)
        self.assertLess(db(rms(at_input, 0.8, 1.0)), -60)    # at the input, applied after v2: -79 dB

    def test_mistakes_are_errors(self):
        for chain, words in [
            ([{"type": "gate", "apply_after": "x"}], "no block has the id"),
            ([{"type": "gain", "id": "g"}, {"type": "gate", "apply_after": "g"}], "must come after"),
            ([{"type": "gate", "apply_after": "p"}, {"type": "feedback", "chain": [{"type": "gain", "id": "p"}]}], "inside a feedback loop"),
        ]:
            with self.subTest(words=words), self.assertRaisesRegex(ValueError, words):
                Amp({"chain": chain}, SR)


if __name__ == "__main__":
    unittest.main()
