"""The amp blocks. Each block takes a mono signal and returns a new one of the same length."""
import math
from pathlib import Path

import numpy as np

from .circuit import digital_filter
from .curves import make_custom, make_pushpull, make_triode
from .filters import IIR, Biquad, check_keys, fft_convolve, make_filters, resample_fft, run_filters
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
    SETTINGS = set()  # the setting names each block allows (besides "type" and "id")

    def __init__(self, cfg, sr):
        check_keys(cfg, {"type", "id"} | self.SETTINGS, f"{cfg['type']} block {cfg.get('id', '')}".strip())
        self.id = cfg.get("id")
        self.sr = sr
        # Set by a gate block with apply_after: one gain value per sample, applied to this block's output.
        self.after_gain = None

    def process(self, x):
        raise NotImplementedError

    def children(self):
        """Blocks inside this block (feedback loops and split paths have some)."""
        return []


def run_blocks(blocks, x):
    for block in blocks:
        x = block.process(x)
        if block.after_gain is not None:
            x = x * block.after_gain
    return x


def build_chain(cfgs, sr):
    blocks = []
    for i, cfg in enumerate(cfgs, start=1):
        kind = cfg.get("type")
        if kind not in BLOCK_TYPES:
            raise ValueError(f"block {i}: unknown type '{kind}'. Use one of: {', '.join(BLOCK_TYPES)}")
        blocks.append(BLOCK_TYPES[kind](cfg, sr))
    return blocks


class EQ(Block):
    """A list of filter bands, applied one after another."""

    SETTINGS = {"bands", "level_db"}

    def __init__(self, cfg, sr):
        super().__init__(cfg, sr)
        self.filters = make_filters(cfg.get("bands", []), sr)
        self.level = db_to_gain(cfg.get("level_db", 0.0))

    def process(self, x):
        return run_filters(self.filters, x) * self.level

    def tick(self, v, h=1.0, commit=True):
        # h (headroom) is not used: an EQ does not clip.
        for f in self.filters:
            v = f.tick(v, commit)
        return v * self.level


class Tonestack(Block):
    """The classic passive bass/mid/treble circuit, worked out from its real part values.

    The circuit (used by most tube amps, with different values):
        in --C1-- treble pot top . treble pot wiper = out . treble pot bottom = x
        x --bass pot (variable resistor)-- y = mid pot top . mid pot wiper = w . mid pot bottom = ground
        in --R4-- z,   z --C2-- x,   z --C3-- w
    The knobs affect each other, just like in the real amp. The output level is the real (lossy) one.
    """

    SETTINGS = {
        "style", "parts", "treble", "mid", "bass", "treble_taper", "mid_taper", "bass_taper",
        "source_ohms", "load_ohms", "level_db",
    }
    # Pot tapers as common schematics list them: linear treble and mid, audio ("log") bass.
    TAPERS = {"treble": "linear", "mid": "linear", "bass": "log"}
    STYLES = {
        # Values as in common published schematics. Check them against the schematic of the amp you copy.
        "british": {"C1": 470e-12, "C2": 22e-9, "C3": 22e-9, "R1": 220e3, "R2": 1e6, "R3": 25e3, "R4": 33e3},
        "american": {"C1": 250e-12, "C2": 20e-9, "C3": 20e-9, "R1": 250e3, "R2": 1e6, "R3": 25e3, "R4": 56e3},
    }
    MIN_OHMS = 1.0  # a pot turned fully to one end still has a tiny resistance

    def __init__(self, cfg, sr):
        super().__init__(cfg, sr)
        style = cfg.get("style", "british")
        if style not in self.STYLES:
            raise ValueError(f"tonestack: unknown style '{style}'. Use one of: {', '.join(self.STYLES)}")
        parts = dict(self.STYLES[style])
        check_keys(cfg.get("parts", {}), set(parts), "tonestack parts")
        parts.update(cfg.get("parts", {}))
        knobs = {}
        for knob in ("treble", "mid", "bass"):
            value = cfg.get(knob, 5.0)
            if not 0 <= value <= 10:
                raise ValueError(f"tonestack: {knob} must be from 0 to 10, got {value}")
            taper = cfg.get(f"{knob}_taper", self.TAPERS[knob])
            if taper not in ("log", "linear"):
                raise ValueError(f"tonestack: {knob}_taper must be 'log' or 'linear'")
            # Audio ("log") taper: half way round gives 10% of the resistance, like real tone pots.
            knobs[knob] = (81 ** (value / 10) - 1) / 80 if taper == "log" else value / 10

        t, m, l = knobs["treble"], knobs["mid"], knobs["bass"]
        p, r = parts, self.MIN_OHMS
        self.parts = [
            ("C", p["C1"], "in", "top"),
            ("R", max(p["R1"] * (1 - t), r), "top", "out"),
            ("R", max(p["R1"] * t, r), "out", "x"),
            ("R", max(p["R2"] * l, r), "x", "y"),
            ("R", max(p["R3"] * (1 - m), r), "y", "w"),
            ("R", max(p["R3"] * m, r), "w", "gnd"),
            ("R", p["R4"], "in", "z"),
            ("C", p["C2"], "z", "x"),
            ("C", p["C3"], "z", "w"),
        ]
        self.source_ohms = cfg.get("source_ohms", 1000.0)
        self.load_ohms = cfg.get("load_ohms", 1e6)
        self.filter = IIR(*digital_filter(self.parts, sr, self.source_ohms, self.load_ohms))
        self.level = db_to_gain(cfg.get("level_db", 0.0))

    def process(self, x):
        return self.filter.process(x) * self.level

    def tick(self, v, h=1.0, commit=True):
        # h (headroom) is not used: a tone stack does not clip.
        return self.filter.tick(v, commit) * self.level


class Gain(Block):
    """One gain stage: pre filters -> drive -> clipping curve -> post filters -> level."""

    # Settings that only work with one curve, with their defaults.
    CURVE_DEFAULTS = {
        "custom": {"top": 1.0, "top_softness": 0.5, "bottom": 1.0, "bottom_softness": 0.5},
        "triode": {"tube": "12AX7", "plate_v": 300.0, "plate_ohms": 100e3, "bias_v": -1.5, "plate_volts": False},
    }
    CURVE_SETTINGS = {curve: set(defaults) for curve, defaults in CURVE_DEFAULTS.items()}
    SETTINGS = {
        "drive_db", "curve", "bias", "level_db", "pre", "post", "oversample",
        "bias_shift", "shift_attack_ms", "shift_release_ms",
    } | set().union(*CURVE_SETTINGS.values())
    CURVE_NAMES = (*CURVES, *CURVE_SETTINGS)

    def __init__(self, cfg, sr):
        super().__init__(cfg, sr)
        curve = cfg.get("curve", "soft")
        if curve not in self.CURVE_NAMES:
            raise ValueError(f"unknown curve '{curve}'. Use one of: {', '.join(self.CURVE_NAMES)}")
        for other, names in self.CURVE_SETTINGS.items():
            wrong = names & set(cfg) if other != curve else set()
            if wrong:
                raise ValueError(f"gain: the setting(s) {', '.join(sorted(wrong))} only work with curve '{other}'")
        self.top = 1.0  # where the top of the curve starts to clip (bias shift starts there)
        self.tube_info = None
        self.curve_params = {k: cfg.get(k, v) for k, v in self.CURVE_DEFAULTS.get(curve, {}).items()}
        if curve == "custom":
            self.curve, self.scalar_curve, self.top = make_custom(**self.curve_params)
        elif curve == "triode":
            params = {k: v for k, v in self.curve_params.items() if k != "plate_volts"}
            self.curve, self.scalar_curve, self.top, self.tube_info = make_triode(**params)
        else:
            self.curve = CURVES[curve]
            self.scalar_curve = SCALAR_CURVES[curve]
        self.drive = db_to_gain(cfg.get("drive_db", 0.0))
        self.bias = cfg.get("bias", 0.0)
        self.level = db_to_gain(cfg.get("level_db", 0.0))
        if self.curve_params.get("plate_volts"):
            self.level *= self.tube_info["gain"]  # put the stage's real gain back: output = plate swing in volts
        self.factor = cfg.get("oversample", 8)
        self.pre = make_filters(cfg.get("pre", []), sr)
        # Uneven clipping leaves a DC offset, so a 10 Hz high-pass always ends the stage.
        self.post = make_filters(cfg.get("post", []), sr) + [Biquad("highpass", 10, sr)]
        # Set by a sag block: one value per sample, 1 = full headroom, lower = clips earlier.
        self.headroom = None

        # Bias shift: signal past the top of the curve (grid current) quickly pushes the working point
        # down (colder); it then slowly drifts back, like the coupling capacitor of a real tube stage.
        self.shift_amount = cfg.get("bias_shift", 0.0)
        if not 0 <= self.shift_amount <= 1:
            raise ValueError(f"bias_shift must be from 0 to 1, got {self.shift_amount}")
        self.shift_attack = 1 - math.exp(-1 / (cfg.get("shift_attack_ms", 1) * 0.001 * sr))
        self.shift_release = 1 - math.exp(-1 / (cfg.get("shift_release_ms", 50) * 0.001 * sr))
        self.shift = 0.0  # the current shift (0 or below)

    def next_shift(self, s, g):
        """The shift for the next sample. g is the curve input without the shift."""
        over = g + s - self.top  # how far past the top of the curve
        if over > 0:
            s -= self.shift_amount * self.shift_attack * over
        return s - self.shift_release * s

    def track_shift(self, g):
        """The shift for every sample of g (one value per sample, worked out at the normal rate)."""
        out = [0.0] * len(g)
        s = self.shift
        for i, v in enumerate(g.tolist()):
            out[i] = s
            s = self.next_shift(s, v)
        self.shift = s
        return np.array(out)

    def process(self, x):
        x = run_filters(self.pre, x) * self.drive
        curve, bias = self.curve, self.bias
        offset = float(curve(np.array(bias)))  # keeps silence at zero when bias is set
        if self.headroom is None and self.shift_amount == 0:
            x = oversampled(x, lambda u: curve(u + bias) - offset, self.factor)
        else:
            # The curve's ceiling is scaled down to the headroom (quiet signals are not changed),
            # and the bias shift moves the working point.
            h = np.ones(len(x)) if self.headroom is None else self.headroom
            s = self.track_shift(x / h + bias) if self.shift_amount else np.zeros(len(x))
            x = oversampled(x, lambda u, h, s: h * (curve(u / h + bias + s) - offset), self.factor, ctrls=(h, s))
        return run_filters(self.post, x) * self.level

    def tick(self, v, h=1.0, commit=True):
        """One sample at a time, for use inside a feedback loop (no oversampling here: the loop does it)."""
        for f in self.pre:
            v = f.tick(v, commit)
        curve, bias = self.scalar_curve, self.bias
        g = v * self.drive / h + bias
        v = h * (curve(g + self.shift) - curve(bias))
        if commit and self.shift_amount:
            self.shift = self.next_shift(self.shift, g)
        for f in self.post:
            v = f.tick(v, commit)
        return v * self.level


class Power(Block):
    """A push-pull power amp: two power tubes driven with opposite signals into an output transformer.

    pre filters -> drive (volts at the grids) -> the tube pair -> post filters -> level.
    Even harmonics cancel (unless the tubes are mismatched), a cold bias gives crossover distortion,
    and each grid has its own bias shift, so hard playing pushes both tubes colder for a moment.
    """

    SETTINGS = {
        "tube", "plate_v", "screen_v", "bias_v", "raa_ohms", "mismatch", "drive_db", "level_db",
        "pre", "post", "oversample", "bias_shift", "shift_attack_ms", "shift_release_ms",
    }

    def __init__(self, cfg, sr):
        super().__init__(cfg, sr)
        plate_v = cfg.get("plate_v", 450.0)
        self.bias_v = cfg.get("bias_v", -45.0)  # about 39 mA idle for the default EL34 pair at 450 V
        self.shape, self.scalar_shape, self.tube_info = make_pushpull(
            cfg.get("tube", "EL34"), plate_v, cfg.get("screen_v") or plate_v, self.bias_v,
            cfg.get("raa_ohms", 3400.0), cfg.get("mismatch", 0.0))
        self.offset = self.scalar_shape(0.0, 1.0, 0.0, 0.0)  # mismatched tubes leave a step at rest
        self.drive = db_to_gain(cfg.get("drive_db", 0.0))
        self.level = db_to_gain(cfg.get("level_db", 0.0))
        self.factor = cfg.get("oversample", 8)
        self.pre = make_filters(cfg.get("pre", []), sr)
        self.post = make_filters(cfg.get("post", []), sr) + [Biquad("highpass", 10, sr)]
        self.headroom = None  # set by a sag block
        self.shift_amount = cfg.get("bias_shift", 0.0)
        if not 0 <= self.shift_amount <= 1:
            raise ValueError(f"bias_shift must be from 0 to 1, got {self.shift_amount}")
        self.shift_attack = 1 - math.exp(-1 / (cfg.get("shift_attack_ms", 1) * 0.001 * sr))
        self.shift_release = 1 - math.exp(-1 / (cfg.get("shift_release_ms", 50) * 0.001 * sr))
        self.sa = self.sb = 0.0  # bias shift of each grid

    def next_shifts(self, sa, sb, x):
        """Each grid charges its coupling capacitor when it goes above 0 V, then drifts back."""
        ga, gb = self.bias_v + sa + x, self.bias_v + sb - x
        if ga > 0:
            sa -= self.shift_amount * self.shift_attack * ga
        if gb > 0:
            sb -= self.shift_amount * self.shift_attack * gb
        return sa - self.shift_release * sa, sb - self.shift_release * sb

    def track_shifts(self, x):
        sa_out, sb_out = [0.0] * len(x), [0.0] * len(x)
        sa, sb = self.sa, self.sb
        for i, v in enumerate(x.tolist()):
            sa_out[i], sb_out[i] = sa, sb
            sa, sb = self.next_shifts(sa, sb, v)
        self.sa, self.sb = sa, sb
        return np.array(sa_out), np.array(sb_out)

    def process(self, x):
        x = run_filters(self.pre, x) * self.drive
        h = np.ones(len(x)) if self.headroom is None else self.headroom
        if self.shift_amount:
            sa, sb = self.track_shifts(x)
        else:
            sa = sb = np.zeros(len(x))
        shape, offset = self.shape, self.offset
        x = oversampled(x, lambda u, h, sa, sb: shape(u, h, sa, sb) - offset, self.factor, ctrls=(h, sa, sb))
        return run_filters(self.post, x) * self.level

    def tick(self, v, h=1.0, commit=True):
        for f in self.pre:
            v = f.tick(v, commit)
        v *= self.drive
        out = self.scalar_shape(v, h, self.sa, self.sb) - self.offset
        if commit and self.shift_amount:
            self.sa, self.sb = self.next_shifts(self.sa, self.sb, v)
        for f in self.post:
            out = f.tick(out, commit)
        return out * self.level


class Sag(Block):
    """Power supply sag. Listens to the signal here and lowers the headroom of later gain stages.

    Playing hard pulls the supply voltage down for a moment, so those stages clip earlier.
    The signal itself passes through unchanged.
    """

    SETTINGS = {"amount", "sens_db", "attack_ms", "release_ms", "targets"}

    def __init__(self, cfg, sr):
        super().__init__(cfg, sr)
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

    SETTINGS = {"file", "mix", "level_db"}

    def __init__(self, cfg, sr):
        super().__init__(cfg, sr)
        ir, ir_sr = read_wav(Path(cfg["file"]))
        ir = ir.mean(axis=1)
        if ir_sr != sr:
            # More samples per second means each sample must count less, or the cab gets louder.
            ir = resample_fft(ir, ir_sr, sr) * (ir_sr / sr)
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

    INNER_TYPES = {"eq": EQ, "tonestack": Tonestack, "gain": Gain, "power": Power}
    SETTINGS = {"amount", "path", "chain", "oversample", "level_db"}

    def __init__(self, cfg, sr):
        super().__init__(cfg, sr)
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
                raise ValueError(f"feedback: '{kind}' blocks can not go inside a feedback loop. Use: {', '.join(self.INNER_TYPES)}")
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

    def children(self):
        return self.blocks


class Split(Block):
    """Parallel paths: the same input goes into every path, and their outputs are added together.

    Like the 'jumpered' channels of an old amp, a clean blend, or two cabs mixed.
    """

    SETTINGS = {"paths", "level_db"}
    PATH_SETTINGS = {"chain", "level_db", "invert"}

    def __init__(self, cfg, sr):
        super().__init__(cfg, sr)
        paths = cfg.get("paths", [])
        if not paths:
            raise ValueError("split: needs at least one path")
        self.paths = []
        for i, path in enumerate(paths, start=1):
            check_keys(path, self.PATH_SETTINGS, f"split path {i}")
            sign = -1.0 if path.get("invert", False) else 1.0
            self.paths.append((build_chain(path.get("chain", []), sr), sign * db_to_gain(path.get("level_db", 0.0))))
        self.level = db_to_gain(cfg.get("level_db", 0.0))

    def process(self, x):
        out = np.zeros(len(x))
        for blocks, gain in self.paths:
            out += gain * run_blocks(blocks, x)
        return out * self.level

    def children(self):
        return [block for blocks, _ in self.paths for block in blocks]


class Gate(Block):
    """Noise gate: turns the sound down when it falls below the threshold.

    With apply_after, the gate listens here but turns down the output of a later block instead
    (listen at the clean input, mute after the high-gain stages where the hiss is loud).
    """

    SETTINGS = {"threshold_db", "hysteresis_db", "attack_ms", "hold_ms", "release_ms", "range_db", "apply_after"}

    def __init__(self, cfg, sr):
        super().__init__(cfg, sr)
        self.open_level = db_to_gain(cfg.get("threshold_db", -50.0))
        self.close_level = db_to_gain(cfg.get("threshold_db", -50.0) - cfg.get("hysteresis_db", 6.0))
        self.attack = 1 - math.exp(-1 / (cfg.get("attack_ms", 0.5) * 0.001 * sr))
        self.release = 1 - math.exp(-1 / (cfg.get("release_ms", 100) * 0.001 * sr))
        self.hold = int(cfg.get("hold_ms", 50) * 0.001 * sr)
        self.floor = db_to_gain(cfg.get("range_db", -80.0))
        # The level is the biggest peak in the last 20 ms: longer than one cycle of the lowest guitar note,
        # so the level does not dip between the peaks of one note.
        self.window = max(1, int(0.020 * sr))
        self.apply_after = cfg.get("apply_after")
        self.target = None  # filled in by the chain when apply_after is set

    def gains(self, x):
        """One gain value per sample: 1 = open, range_db = closed."""
        open_level, close_level, hold = self.open_level, self.close_level, self.hold
        attack, release, floor = self.attack, self.release, self.floor
        padded = np.concatenate([np.zeros(self.window - 1), np.abs(x)])
        levels = np.lib.stride_tricks.sliding_window_view(padded, self.window).max(axis=1)
        is_open, held, g = False, 0, floor
        out = [0.0] * len(x)
        for i, level in enumerate(levels.tolist()):
            if level > open_level:
                is_open, held = True, 0
            elif is_open and level < close_level:
                held += 1
                if held > hold:
                    is_open = False
            target = 1.0 if is_open else floor
            g += (attack if target > g else release) * (target - g)
            out[i] = g
        return np.array(out)

    def process(self, x):
        gains = self.gains(x)
        if self.target is not None:
            self.target.after_gain = gains
            return x
        return x * gains


BLOCK_TYPES = {
    "eq": EQ, "tonestack": Tonestack, "gain": Gain, "power": Power, "sag": Sag, "feedback": Feedback,
    "split": Split, "gate": Gate, "ir": IRLoader,
}
