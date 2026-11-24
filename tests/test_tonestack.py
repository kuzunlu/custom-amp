import itertools
import unittest

import numpy as np

from ampsim.blocks import Tonestack
from ampsim.circuit import analog_response
from tests.common import SR


def yeh(f, t, m, l, C1, C2, C3, R1, R2, R3, R4):
    """The published closed form of this tone stack (Yeh & Smith 2006): ideal source, no load."""
    b1 = t*C1*R1 + m*C3*R3 + l*(C1*R2 + C2*R2) + (C1*R3 + C2*R3)
    b2 = (t*(C1*C2*R1*R4 + C1*C3*R1*R4) - m*m*(C1*C3*R3*R3 + C2*C3*R3*R3)
          + m*(C1*C3*R1*R3 + C1*C3*R3*R3 + C2*C3*R3*R3)
          + l*(C1*C2*R1*R2 + C1*C2*R2*R4 + C1*C3*R2*R4)
          + l*m*(C1*C3*R2*R3 + C2*C3*R2*R3)
          + (C1*C2*R1*R3 + C1*C2*R3*R4 + C1*C3*R3*R4))
    b3 = (l*m*(C1*C2*C3*R1*R2*R3 + C1*C2*C3*R2*R3*R4)
          - m*m*(C1*C2*C3*R1*R3*R3 + C1*C2*C3*R3*R3*R4)
          + m*(C1*C2*C3*R1*R3*R3 + C1*C2*C3*R3*R3*R4)
          + t*C1*C2*C3*R1*R3*R4 - t*m*C1*C2*C3*R1*R3*R4
          + t*l*C1*C2*C3*R1*R2*R4)
    a1 = (C1*R1 + C1*R3 + C2*R3 + C2*R4 + C3*R4) + m*C3*R3 + l*(C1*R2 + C2*R2)
    a2 = (m*(C1*C3*R1*R3 - C2*C3*R3*R4 + C1*C3*R3*R3 + C2*C3*R3*R3)
          + l*m*(C1*C3*R2*R3 + C2*C3*R2*R3)
          - m*m*(C1*C3*R3*R3 + C2*C3*R3*R3)
          + l*(C1*C2*R2*R4 + C1*C2*R1*R2 + C1*C3*R2*R4 + C2*C3*R2*R4)
          + (C1*C2*R1*R4 + C1*C3*R1*R4 + C1*C2*R3*R4 + C1*C2*R1*R3 + C1*C3*R3*R4 + C2*C3*R3*R4))
    a3 = (l*m*(C1*C2*C3*R1*R2*R3 + C1*C2*C3*R2*R3*R4)
          - m*m*(C1*C2*C3*R1*R3*R3 + C1*C2*C3*R3*R3*R4)
          + m*(C1*C2*C3*R3*R3*R4 + C1*C2*C3*R1*R3*R3 - C1*C2*C3*R1*R3*R4)
          + l*C1*C2*C3*R1*R2*R4 + C1*C2*C3*R1*R3*R4)
    s = 2j * np.pi * f
    return (b1*s + b2*s**2 + b3*s**3) / (1 + a1*s + a2*s**2 + a3*s**3)


class ToneStack(unittest.TestCase):
    def test_circuit_solver_matches_the_published_formula(self):
        freqs = np.geomspace(20, 20000, 200)
        for style, p in Tonestack.STYLES.items():
            for knobs in [(5, 5, 5), (0, 0, 0), (10, 10, 10), (10, 0, 3), (2, 8, 10), (7, 1, 0)]:
                with self.subTest(style=style, knobs=knobs):
                    ts = Tonestack({"type": "tonestack", "style": style, "treble": knobs[0], "mid": knobs[1], "bass": knobs[2],
                                    "treble_taper": "linear", "mid_taper": "linear", "bass_taper": "linear"}, SR)
                    t, m, l = (k / 10 for k in knobs)
                    R1, R2, R3 = p["R1"], p["R2"], p["R3"]
                    # the solver puts 1 ohm in a pot turned to an end; give the formula the same tiny value
                    t_eff = 1 - max(R1 * (1 - t), 1) / R1 if t > 0.5 else max(R1 * t, 1) / R1
                    mine = analog_response(ts.parts, freqs, source_ohms=1e-3, load_ohms=1e12)
                    theirs = yeh(freqs, t_eff, max(R3 * m, 1) / R3, max(R2 * l, 1) / R2, p["C1"], p["C2"], p["C3"], R1, R2, R3, p["R4"])
                    self.assertLess(np.max(np.abs(20 * np.log10(np.abs(mine) / np.abs(theirs)))), 0.001)  # measured 0.0003 dB

    def test_first_coefficient_is_above_zero(self):
        # needed inside a feedback loop: the block must rise with its input
        for style, rate, knobs in itertools.product(Tonestack.STYLES, (48000, 192000), itertools.product((0, 5, 10), repeat=3)):
            with self.subTest(style=style, rate=rate, knobs=knobs):
                ts = Tonestack({"type": "tonestack", "style": style, "treble": knobs[0], "mid": knobs[1], "bass": knobs[2]}, rate)
                self.assertGreater(ts.filter.b[0], 0)


if __name__ == "__main__":
    unittest.main()
