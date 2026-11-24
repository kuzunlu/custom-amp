"""Check the front panel rules: the shared cases in plugin/tests/panel_cases.json (PluginTest checks the same
cases in the plugin) and the panels of all built-in presets.

    python tools/check_panel.py
"""
import json
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT))

from ampsim.chain import Amp  # noqa: E402


def main():
    cases = json.loads((ROOT / "plugin" / "tests" / "panel_cases.json").read_text(encoding="utf-8"))
    failures = 0

    for case in cases["cases"]:
        try:
            Amp({"chain": cases["chain"], "panel": case["panel"]}, 48000)
            error = None
        except ValueError as e:
            error = str(e)
        ok = error == case["error"]
        failures += not ok
        print("  ok  " if ok else "  FAIL", error if ok else f"got {error!r}, expected {case['error']!r}")

    for path in sorted((ROOT / "presets").glob("*.json")):
        preset = json.loads(path.read_text(encoding="utf-8"))
        try:
            Amp(preset, 48000)
            print("  ok  ", path.name, f"{len(preset.get('panel', []))} knobs")
        except ValueError as e:
            failures += 1
            print("  FAIL", path.name, e)

    print("All passed" if failures == 0 else f"{failures} failed")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
