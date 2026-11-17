"""The amp blocks. Each block takes a mono signal and returns a new one of the same length."""
import math
from pathlib import Path

import numpy as np

from .filters import Biquad, check_keys, fft_convolve, make_filters, resample_fft, run_filters
from .oversample import oversampled
from .wavio import read_wav


def db_to_gain(db):
    return 10 ** (db / 20)


# Clipping curves. Input around -1..1 is mostly clean, bigger values get squashed.
CURVES = {
    "hard": lambda x: np.clip(x, -1.0, 1.0),
    "soft": np.tanh,
    # Top clips earlier than the bottom, like a triode. The uneven shape adds even harmonics.
    "tube": lambda x: np.where(x >= 0, np.tanh(x), 1.5 * np.tanh(x / 1.5)),
    # Between soft and hard: clean for longer, then a fast bend.
    "diode": lambda x: x / (1 + np.abs(x) ** 2.5) ** (1 / 2.5),
}


class Block:
    def __init__(self, cfg, sr, allowed):
        check_keys(cfg, {"type", "id"} | allowed, f"{cfg['type']} block {cfg.get('id', '')}".strip())
        self.id = cfg.get("id")
        self.sr = sr

    def process(self, x):
        raise NotImplementedError


class EQ(Block):
    """A list of filter bands, applied one after another."""

    def __init__(self, cfg, sr):
        super().__init__(cfg, sr, {"bands", "level_db"})
        self.filters = make_filters(cfg.get("bands", []), sr)
        self.level = db_to_gain(cfg.get("level_db", 0.0))

    def process(self, x):
        return run_filters(self.filters, x) * self.level


class Gain(Block):
    """One gain stage: pre filters -> drive -> clipping curve -> post filters -> level."""

    def __init__(self, cfg, sr):
        super().__init__(cfg, sr, {"drive_db", "curve", "bias", "level_db", "pre", "post", "oversample"})
        curve = cfg.get("curve", "soft")
        if curve not in CURVES:
            raise ValueError(f"unknown curve '{curve}'. Use one of: {', '.join(CURVES)}")
        self.curve = CURVES[curve]
        self.drive = db_to_gain(cfg.get("drive_db", 0.0))
        self.bias = cfg.get("bias", 0.0)
        self.level = db_to_gain(cfg.get("level_db", 0.0))
        self.factor = cfg.get("oversample", 8)
        self.pre = make_filters(cfg.get("pre", []), sr)
        # Uneven clipping leaves a DC offset, so a 10 Hz high-pass always ends the stage.
        self.post = make_filters(cfg.get("post", []), sr) + [Biquad("highpass", 10, sr)]
        # Set by a sag block: one value per sample, 1 = full headroom, lower = clips earlier.
        self.headroom = None

    def process(self, x):
        x = run_filters(self.pre, x) * self.drive
        curve, bias = self.curve, self.bias
        offset = float(curve(np.array(bias)))  # keeps silence at zero when bias is set
        if self.headroom is None:
            x = oversampled(x, lambda u: curve(u + bias) - offset, self.factor)
        else:
            # Same curve, but its ceiling is scaled down to the headroom. Quiet signals are not changed.
            x = oversampled(x, lambda u, h: h * (curve(u / h + bias) - offset), self.factor, ctrl=self.headroom)
        return run_filters(self.post, x) * self.level


class Sag(Block):
    """Power supply sag. Listens to the signal here and lowers the headroom of later gain stages.

    Playing hard pulls the supply voltage down for a moment, so those stages clip earlier.
    The signal itself passes through unchanged.
    """

    def __init__(self, cfg, sr):
        super().__init__(cfg, sr, {"amount", "sens_db", "attack_ms", "release_ms", "targets"})
        self.amount = cfg.get("amount", 0.3)
        if not 0 <= self.amount < 1:
            raise ValueError(f"sag amount must be from 0 up to (not including) 1, got {self.amount}")
        self.sens = db_to_gain(cfg.get("sens_db", 0.0))
        self.attack = math.exp(-1 / (cfg.get("attack_ms", 5) * 0.001 * sr))
        self.release = math.exp(-1 / (cfg.get("release_ms", 150) * 0.001 * sr))
        self.target_ids = cfg.get("targets", [])
        if not isinstance(self.target_ids, list):
            raise ValueError('sag targets must be a list, like ["v2"]')
        self.targets = []  # filled in by the chain, see chain.link_sag

    def envelope(self, x):
        """Follow the loudness: rise with the attack time, fall with the release time."""
        attack, release = self.attack, self.release
        e = 0.0
        out = [0.0] * len(x)
        for i, v in enumerate(np.abs(x).tolist()):
            c = attack if v > e else release
            e = v + c * (e - v)
            out[i] = e
        return np.array(out)

    def process(self, x):
        # tanh: the supply can only drop so far, even when playing very hard.
        headroom = 1 - self.amount * np.tanh(self.envelope(x) * self.sens)
        for target in self.targets:
            target.headroom = headroom
        return x


class IRLoader(Block):
    """Cabinet sound: convolution with an impulse response .wav file."""

    def __init__(self, cfg, sr):
        super().__init__(cfg, sr, {"file", "mix", "level_db"})
        ir, ir_sr = read_wav(Path(cfg["file"]))
        ir = ir.mean(axis=1)
        if ir_sr != sr:
            ir = resample_fft(ir, ir_sr, sr)
        self.ir = ir
        self.mix = cfg.get("mix", 1.0)
        self.level = db_to_gain(cfg.get("level_db", 0.0))

    def process(self, x):
        wet = fft_convolve(x, self.ir)[: len(x)]
        return ((1 - self.mix) * x + self.mix * wet) * self.level


BLOCK_TYPES = {"eq": EQ, "gain": Gain, "sag": Sag, "ir": IRLoader}
