"""Check that the C++ engine gives the same numbers as the Python prototype.

    python tools/cpp_check.py                      # all presets
    python tools/cpp_check.py presets/crunch.json  # some presets

Needs audio/ (python tools/make_test_audio.py) and the C++ test program (build.bat --target EngineTest).

The plugin runs the whole amp at 8x the sample rate. To compare the block math itself, both versions run
the same input at that high rate WITHOUT any oversampling of their own (Python: oversample = 1 everywhere):
  core: everything up to the cab IR, at 384 kHz
  tail: the cab IR and the eq blocks after it, at 48 kHz
The difference is printed in dB below the peak of the Python output.
"""
import argparse
import copy
import json
import subprocess
import sys
import tempfile
import time
from pathlib import Path

import numpy as np

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT))

from ampsim.chain import Amp  # noqa: E402
from ampsim.oversample import upsample  # noqa: E402
from ampsim.wavio import read_wav  # noqa: E402

ENGINE_TEST = ROOT / "build" / "EngineTest_artefacts" / "Release" / "EngineTest.exe"


def tail_start(chain):
    """Same rule as tailStart() in Engine.cpp: the first ir of the run of ir/eq blocks at the end."""
    start = len(chain)
    while start > 0 and chain[start - 1].get("type") in ("ir", "eq"):
        start -= 1
    while start < len(chain) and chain[start].get("type") == "eq":
        start += 1
    return start


def no_oversampling(blocks):
    """Set oversample = 1 on every block that has it (not inside feedback loops: they can not set it)."""
    for block in blocks:
        if block.get("type") in ("gain", "power", "feedback"):
            block["oversample"] = 1
        if block.get("type") == "split":
            for path in block.get("paths", []):
                no_oversampling(path.get("chain", []))


def difference_db(ref, out):
    peak = np.max(np.abs(ref))
    err = np.max(np.abs(out - ref))
    if err == 0:
        return float("-inf")
    return 20 * np.log10(err / peak)


def run_part(part, preset_path, preset, x, rate, work):
    chain = preset["chain"]
    start = tail_start(chain)
    sub = copy.deepcopy(preset)
    sub["chain"] = chain[:start] if part == "core" else chain[start:]
    if not sub["chain"]:
        return None
    if part == "core":
        no_oversampling(sub["chain"])

    t0 = time.perf_counter()
    ref = Amp(sub, rate).process(x.copy())
    py_seconds = time.perf_counter() - t0

    inp, outp = work / f"{part}_in.f64", work / f"{part}_out.f64"
    x.astype("<f8").tofile(inp)
    subprocess.run([str(ENGINE_TEST), part, str(preset_path), str(inp), str(outp), str(rate)],
                   check=True, capture_output=True, text=True)
    out = np.fromfile(outp, dtype="<f8")
    return difference_db(ref, out), py_seconds


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("presets", nargs="*", help="preset files (default: all in presets/)")
    parser.add_argument("--seconds", type=float, default=1.0, help="length of the test DI to use")
    parser.add_argument("--factor", type=int, default=8, help="the plugin's oversampling factor")
    args = parser.parse_args()

    if not ENGINE_TEST.exists():
        sys.exit(f"{ENGINE_TEST} not found: run build.bat --target EngineTest first")

    di, sr = read_wav(ROOT / "audio" / "test_di.wav")
    di = di.mean(axis=1)[: int(args.seconds * sr)]
    di_high = upsample(di, args.factor)
    presets = [Path(p) for p in args.presets] or sorted((ROOT / "presets").glob("*.json"))

    print(f"Difference C++ vs Python, dB below the output peak ({args.seconds} s of the test DI)")
    print(f"{'preset':24} {'core @ ' + str(sr * args.factor // 1000) + ' kHz':>16} {'tail @ ' + str(sr // 1000) + ' kHz':>14}")
    worst = float("-inf")
    with tempfile.TemporaryDirectory() as tmp:
        work = Path(tmp)
        for path in presets:
            preset = json.loads(path.read_text(encoding="utf-8"))
            cells = []
            for part, x, rate in (("core", di_high, sr * args.factor), ("tail", di * 10, sr)):
                result = run_part(part, path.resolve(), preset, x, rate, work)
                if result is None:
                    cells.append("(none)")
                else:
                    worst = max(worst, result[0])
                    cells.append(f"{result[0]:.0f} dB")
            print(f"{path.stem:24} {cells[0]:>16} {cells[1]:>14}", flush=True)
    print(f"Worst: {worst:.0f} dB")


if __name__ == "__main__":
    main()
