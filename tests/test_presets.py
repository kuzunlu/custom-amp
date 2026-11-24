import contextlib
import io
import json
import os
import unittest

import numpy as np

from ampsim.chain import load_amp
from ampsim.wavio import read_wav
from tests.common import ROOT, SR
from tools import check_panel, check_settings

PRESETS = sorted((ROOT / "presets").glob("*.json"))


def quietly(main):
    """Runs a tool's main() without its printout; returns its exit code."""
    with contextlib.redirect_stdout(io.StringIO()):
        return main()


class Presets(unittest.TestCase):
    def test_settings_json_matches_the_code(self):
        self.assertEqual(quietly(check_settings.main), 0, "run python tools/check_settings.py to see what differs")

    def test_panel_rules_and_every_preset_panel(self):
        self.assertEqual(quietly(check_panel.main), 0, "run python tools/check_panel.py to see what differs")

    def test_every_preset_has_a_name_and_a_panel(self):
        for path in PRESETS:
            with self.subTest(preset=path.name):
                preset = json.loads(path.read_text(encoding="utf-8"))
                self.assertTrue(preset.get("name"))
                self.assertTrue(preset.get("panel"))

    def test_presets_still_sound_the_same(self):
        """The start of every preset equals its out/*.wav file (made by run.py). If a change was MEANT to change the
        sound, make the files again: python run.py presets/<name>.json audio/test_di.wav out/<name>.wav"""
        di_file = ROOT / "audio" / "test_di.wav"
        if not di_file.exists():
            self.skipTest("no audio/test_di.wav: run python tools/make_test_audio.py")
        di = read_wav(str(di_file))[0][:, 0]
        cwd = os.getcwd()
        os.chdir(ROOT)  # the IR paths in the presets are relative to the project folder
        try:
            for path in PRESETS:
                out = ROOT / "out" / f"{path.stem}.wav"
                with self.subTest(preset=path.name):
                    if not out.exists():
                        self.skipTest(f"no {out.name}: make it with run.py")
                    # 0.5 s through the amp, compare the first 0.4 s (Python is slow: the whole 10 s takes minutes).
                    # The file is scaled to -1 dBFS over all 10 s, so find the scale first.
                    y = load_amp(str(path), SR).process(di[:SR // 2].copy())[:int(0.4 * SR)]
                    ref = read_wav(str(out))[0][:int(0.4 * SR), 0]
                    scale = np.dot(ref, y) / np.dot(y, y)
                    self.assertLess(np.max(np.abs(scale * y - ref)), 2 ** -23)  # 24-bit rounding: measured 6e-8
        finally:
            os.chdir(cwd)


if __name__ == "__main__":
    unittest.main()
