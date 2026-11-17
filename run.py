"""Run a guitar recording through an amp preset.

    python run.py presets/crunch.json audio/test_di.wav out/crunch.wav
"""
import argparse
import math
import time
from pathlib import Path

import numpy as np

from ampsim.chain import load_amp
from ampsim.wavio import read_wav, write_wav


def main():
    parser = argparse.ArgumentParser(description="Run a guitar recording through an amp preset.")
    parser.add_argument("preset", help="preset .json file")
    parser.add_argument("input", help="dry guitar .wav file")
    parser.add_argument("output", help=".wav file to write")
    parser.add_argument("--no-normalize", action="store_true", help="keep the raw output level (may clip)")
    args = parser.parse_args()

    x, sr = read_wav(args.input)
    x = x.mean(axis=1)  # the amp is mono
    amp = load_amp(args.preset, sr)

    start = time.perf_counter()
    y = amp.process(x)
    seconds = time.perf_counter() - start

    peak = float(np.max(np.abs(y)))
    if not args.no_normalize and peak > 0:
        y = y * (10 ** (-1 / 20) / peak)  # peak at -1 dBFS

    Path(args.output).parent.mkdir(parents=True, exist_ok=True)
    write_wav(args.output, y, sr)
    peak_db = 20 * math.log10(peak) if peak > 0 else float("-inf")
    print(f"{len(x) / sr:.1f} s of audio done in {seconds:.1f} s. Raw output peak: {peak_db:.1f} dBFS.")
    print(f"Wrote {args.output}")


if __name__ == "__main__":
    main()
