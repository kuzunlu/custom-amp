"""The amp blocks. Each block takes a mono signal and returns a new one of the same length."""
import math
from pathlib import Path

import numpy as np

from .filters import Biquad, check_keys, fft_convolve, make_filters, resample_fft, run_filters
from .oversample import downsample, oversampled, upsample
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

# The same curves for one number at a time (much faster than NumPy for single values).
# Keep these the same as CURVES above.
SCALAR_CURVES = {
    "hard": lambda x: max(-1.0, min(1.0, x)),
    "soft": math.tanh,
    "tube": lambda x: math.tanh(x) if x >= 0 else 1.5 * math.tanh(x / 1.5),
    "diode": lambda x: x / (1 + abs(x) ** 2.5) ** (1 / 2.5),
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

    def tick(self, v, h=1.0, commit=True):
        # h (headroom) is not used: an EQ does not clip.
        for f in self.filters:
            v = f.tick(v, commit)
        return v * self.level


class Gain(Block):
    """One gain stage: pre filters -> drive -> clipping curve -> post filters -> level."""

    def __init__(self, cfg, sr):
        super().__init__(cfg, sr, {"drive_db", "curve", "bias", "level_db", "pre", "post", "oversample"})
        curve = cfg.get("curve", "soft")
        if curve not in CURVES:
            raise ValueError(f"unknown curve '{curve}'. Use one of: {', '.join(CURVES)}")
        self.curve = CURVES[curve]
        self.scalar_curve = SCALAR_CURVES[curve]
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

    def tick(self, v, h=1.0, commit=True):
        """One sample at a time, for use inside a feedback loop (no oversampling here: the loop does it)."""
        for f in self.pre:
            v = f.tick(v, commit)
        curve, bias = self.scalar_curve, self.bias
        v = h * (curve(v * self.drive / h + bias) - curve(bias))
        for f in self.post:
            v = f.tick(v, commit)
        return v * self.level


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


def solve_falling(g, y0, tol=1e-10, max_steps=60):
    """Find y where g(y) = 0, for a g that always goes down as y goes up (so there is exactly one answer).

    First walk away from y0 in bigger and bigger steps until g changes sign,
    then close in with the "Illinois" method (a safer form of the secant method).
    """
    a, ga = y0, g(y0)
    if ga == 0:
        return a
    direction = 1 if ga > 0 else -1
    step = max(1e-4, 0.01 * abs(y0))
    b, gb = a + direction * step, g(a + direction * step)
    while (gb > 0) == (ga > 0):
        a, ga = b, gb
        step *= 2
        b, gb = a + direction * step, g(a + direction * step)

    c = b
    for _ in range(max_steps):
        c = b - gb * (b - a) / (gb - ga)
        gc = g(c)
        if abs(gc) < tol or abs(b - a) < tol:
            break
        if (gc > 0) != (gb > 0):
            a, ga = b, gb
        else:
            ga *= 0.5  # the Illinois trick: stops one end from getting stuck
        b, gb = c, gc
    return c


class Feedback(Block):
    """Runs a small chain in a loop: the chain gets  input - amount * path_filters(output).

    Positive amount = negative feedback, like the global feedback loop of a tube power amp:
    less gain, tighter and cleaner clipping. The path filters choose which tones get less feedback,
    so they come out louder (a high shelf cut = "presence", a low shelf cut = "resonance").

    There is no delay in the loop (a delay makes strong feedback oscillate). Instead, the output of each
    sample is found by solving the loop equation. It runs one sample at a time at `oversample` times
    the sample rate, so it is slow in Python.
    """

    INNER_TYPES = {"eq": EQ, "gain": Gain}

    def __init__(self, cfg, sr):
        super().__init__(cfg, sr, {"amount", "path", "chain", "oversample", "level_db"})
        self.amount = cfg.get("amount", 0.3)
        if self.amount < 0:
            raise ValueError("feedback amount must be 0 or more (positive feedback is not supported yet)")
        self.factor = cfg.get("oversample", 4)
        self.level = db_to_gain(cfg.get("level_db", 0.0))
        loop_sr = sr * self.factor
        self.path = make_filters(cfg.get("path", []), loop_sr)

        self.blocks = []
        for inner in cfg.get("chain", []):
            kind = inner.get("type")
            if kind not in self.INNER_TYPES:
                raise ValueError(f"feedback: '{kind}' blocks can not go inside a feedback loop. Use: eq, gain")
            if "oversample" in inner:
                raise ValueError("feedback: gain blocks inside the loop can not set 'oversample'. Set it on the feedback block")
            self.blocks.append(self.INNER_TYPES[kind](inner, loop_sr))

    def process(self, x):
        factor = self.factor
        steps = []
        for b in self.blocks:
            headroom = getattr(b, "headroom", None)
            steps.append((b.tick, None if headroom is None else np.repeat(headroom, factor).tolist()))
        path, amount = self.path, self.amount

        def loop(xv, y, n, commit):
            """What the chain puts out if the loop output is y. commit=True moves all filters on."""
            fb = y
            for f in path:
                fb = f.tick(fb, commit)
            v = xv - amount * fb
            for tick, headroom in steps:
                v = tick(v, 1.0 if headroom is None else headroom[n], commit)
            return v

        u = upsample(x, factor).tolist()
        out = [0.0] * len(u)
        y1 = y2 = 0.0  # last two outputs, for a good first guess
        for n, xv in enumerate(u):
            y = solve_falling(lambda yy: loop(xv, yy, n, False) - yy, 2 * y1 - y2)
            loop(xv, y, n, True)
            out[n] = y
            y1, y2 = y, y1
        return downsample(np.array(out), factor) * self.level


BLOCK_TYPES = {"eq": EQ, "gain": Gain, "sag": Sag, "feedback": Feedback, "ir": IRLoader}
