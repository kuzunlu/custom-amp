"""Check that shared/settings.json matches the code: same blocks, setting names, choices and defaults.

    python tools/check_settings.py
"""
import json
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT))

from ampsim.blocks import BLOCK_TYPES, Feedback, Gain, Split, Tonestack  # noqa: E402
from ampsim.curves import PENTODES, TUBES  # noqa: E402
from ampsim.filters import BAND_SETTINGS, KINDS, make_filters  # noqa: E402

SR = 48000
# Settings a block can not be built without, and attributes that can not be compared simply.
REQUIRED = {"ir": {"file": str(ROOT / "audio" / "fake_4x12.wav")}, "split": {"paths": [{}]}}
SKIP = {"blocks", "path", "paths", "pre", "post", "filters", "filter", "targets", "target_ids", "ir", "curve", "scalar_curve",
        "shape", "scalar_shape"}


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
        # Settings that belong to one curve are checked with that curve chosen.
        defaults = {k: v["default"] for k, v in settings.items() if v["default"] is not None and "curve" not in v}
        groups = [({}, defaults)]
        for curve in getattr(cls, "CURVE_SETTINGS", {}):
            groups.append(({"curve": curve}, {k: v["default"] for k, v in settings.items() if v.get("curve") == curve}))
        for extra, values in groups:
            base = {"type": kind, **REQUIRED.get(kind, {}), **extra}
            with_defaults = cls({**base, **values}, SR).__dict__
            without = cls(base, SR).__dict__
            for attr, value in with_defaults.items():
                if attr not in SKIP and value != without[attr]:
                    problems.append(f"{kind} {extra}: default differs for '{attr}': doc gives {value}, code gives {without[attr]}")
        for curve, names in getattr(cls, "CURVE_SETTINGS", {}).items():
            tagged = {k for k, v in settings.items() if v.get("curve") == curve}
            if tagged != names:
                problems.append(f"{kind}: settings marked for curve '{curve}' differ: doc {sorted(tagged)}, code {sorted(names)}")

    if set(doc["blocks"]["gain"]["settings"]["curve"]["choices"]) != set(Gain.CURVE_NAMES):
        problems.append("gain curve choices differ")
    if set(doc["blocks"]["gain"]["settings"]["tube"]["choices"]) != set(TUBES):
        problems.append("triode tube choices differ")
    if set(doc["blocks"]["power"]["settings"]["tube"]["choices"]) != set(PENTODES):
        problems.append("power tube choices differ")
    if set(doc["blocks"]["feedback"]["settings"]["chain"]["allowed"]) != set(Feedback.INNER_TYPES):
        problems.append("blocks allowed inside a feedback loop differ")
    tonestack = doc["blocks"]["tonestack"]["settings"]
    if set(tonestack["style"]["choices"]) != set(Tonestack.STYLES):
        problems.append("tonestack style choices differ")
    if set(tonestack["parts"]["names"]) != set(Tonestack.STYLES["british"]):
        problems.append("tonestack part names differ")
    band = doc["band"]["settings"]
    if set(band) != BAND_SETTINGS:
        problems.append("band setting names differ")
    if set(band["kind"]["choices"]) != set(KINDS):
        problems.append("band kind choices differ")
    with_defaults = make_filters([{"kind": "peak", "freq": 1000, "q": band["q"]["default"],
                                   "gain_db": band["gain_db"]["default"]}], SR)[0].__dict__
    if with_defaults != make_filters([{"kind": "peak", "freq": 1000}], SR)[0].__dict__:
        problems.append("band defaults differ")

    path = doc["split_path"]["settings"]
    if set(path) != Split.PATH_SETTINGS:
        problems.append("split path setting names differ")
    path_defaults = {k: v["default"] for k, v in path.items()}
    if Split({"type": "split", "paths": [path_defaults]}, SR).paths[0][1] != Split({"type": "split", "paths": [{}]}, SR).paths[0][1]:
        problems.append("split path defaults differ")

    for p in problems:
        print("MISMATCH:", p)
    print("settings.json matches the code" if not problems else f"{len(problems)} problem(s)")
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main())
