"""Check that shared/settings.json matches the code: same blocks, setting names, choices and defaults.

    python tools/check_settings.py
"""
import json
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT))

from ampsim.blocks import BLOCK_TYPES, CURVES  # noqa: E402
from ampsim.filters import BAND_SETTINGS, KINDS, make_filters  # noqa: E402

SR = 48000
# Settings a block can not be built without, and attributes that can not be compared simply.
REQUIRED = {"ir": {"file": str(ROOT / "audio" / "fake_4x12.wav")}}
SKIP = {"blocks", "path", "pre", "post", "filters", "targets", "target_ids", "ir", "curve", "scalar_curve"}


def main():
    doc = json.loads((ROOT / "shared" / "settings.json").read_text(encoding="utf-8"))
    problems = []

    if set(doc["blocks"]) != set(BLOCK_TYPES):
        problems.append(f"block types: code {sorted(BLOCK_TYPES)}, doc {sorted(doc['blocks'])}")
    if set(doc["common"]["settings"]["type"]["choices"]) != set(BLOCK_TYPES):
        problems.append("common.type choices do not list every block type")

    for kind, cls in BLOCK_TYPES.items():
        if kind not in doc["blocks"]:
            continue
        settings = doc["blocks"][kind]["settings"]
        if set(settings) != cls.SETTINGS:
            problems.append(f"{kind}: only in code {sorted(cls.SETTINGS - set(settings))}, "
                            f"only in doc {sorted(set(settings) - cls.SETTINGS)}")
        # A block built with the documented defaults must equal a block built with nothing set.
        defaults = {k: v["default"] for k, v in settings.items() if v["default"] is not None}
        base = {"type": kind, **REQUIRED.get(kind, {})}
        with_defaults = cls({**base, **defaults}, SR).__dict__
        without = cls(base, SR).__dict__
        for attr, value in with_defaults.items():
            if attr not in SKIP and value != without[attr]:
                problems.append(f"{kind}: default differs for '{attr}': doc gives {value}, code gives {without[attr]}")

    if set(doc["blocks"]["gain"]["settings"]["curve"]["choices"]) != set(CURVES):
        problems.append("gain curve choices differ")
    band = doc["band"]["settings"]
    if set(band) != BAND_SETTINGS:
        problems.append("band setting names differ")
    if set(band["kind"]["choices"]) != set(KINDS):
        problems.append("band kind choices differ")
    with_defaults = make_filters([{"kind": "peak", "freq": 1000, "q": band["q"]["default"],
                                   "gain_db": band["gain_db"]["default"]}], SR)[0].__dict__
    if with_defaults != make_filters([{"kind": "peak", "freq": 1000}], SR)[0].__dict__:
        problems.append("band defaults differ")

    for p in problems:
        print("MISMATCH:", p)
    print("settings.json matches the code" if not problems else f"{len(problems)} problem(s)")
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main())
