"""Clipping curves with settings: 'custom' (top and bottom set separately) and 'triode' (a real tube).

Each builder returns (numpy_curve, scalar_curve, top):
    numpy_curve  works on whole arrays, scalar_curve on one number (for feedback loops),
    top          the input level where the top of the curve starts to clip (used by bias shift).
"""
import math

import numpy as np

# Koren's triode model numbers (N. Koren, "Improved VT models for SPICE simulations", 1996).
TUBES = {
    "12AX7": {"mu": 100.0, "ex": 1.4, "kg1": 1060.0, "kp": 600.0, "kvb": 300.0},
    "12AT7": {"mu": 60.0, "ex": 1.35, "kg1": 460.0, "kp": 300.0, "kvb": 300.0},
    "12AU7": {"mu": 21.5, "ex": 1.3, "kg1": 1180.0, "kp": 84.0, "kvb": 300.0},
}

# Grid current: above 0 V the grid conducts and, with the resistance in front of it, the grid voltage
# can only rise slowly (about 2% of the input) after a soft corner about 0.1 V wide.
GRID_SLOPE = 0.02
GRID_KNEE_V = 0.1


def softness_to_power(softness):
    """0 = nearly hard corner, 1 = very soft corner."""
    return 2 ** (3 * (1 - softness))


def make_custom(top, top_softness, bottom, bottom_softness):
    if top <= 0 or bottom <= 0:
        raise ValueError("custom curve: top and bottom must be above 0")
    for name, s in (("top_softness", top_softness), ("bottom_softness", bottom_softness)):
        if not 0 <= s <= 1:
            raise ValueError(f"custom curve: {name} must be from 0 to 1, got {s}")
    pt, pb = softness_to_power(top_softness), softness_to_power(bottom_softness)

    # Each side: slope 1 for small signals, then a bend that ends flat at its level.
    def numpy_curve(x):
        up = top * (x / top) / (1 + np.abs(x / top) ** pt) ** (1 / pt)
        down = bottom * (x / bottom) / (1 + np.abs(x / bottom) ** pb) ** (1 / pb)
        return np.where(x >= 0, up, down)

    def scalar_curve(x):
        level, p = (top, pt) if x >= 0 else (bottom, pb)
        return x / (1 + abs(x / level) ** p) ** (1 / p)

    return numpy_curve, scalar_curve, top


def koren_plate_current(vp, vg, mu, ex, kg1, kp, kvb):
    """Plate current (amps) of a triode at plate voltage vp and grid voltage vg (both against the cathode)."""
    e1 = vp / kp * np.logaddexp(0.0, kp * (1 / mu + vg / np.sqrt(kvb + vp * vp)))
    return 2 * np.maximum(e1, 0.0) ** ex / kg1


def triode_plate_voltage(vg, tube, plate_v, plate_ohms):
    """Plate voltage for each grid voltage: where the tube's current meets the plate resistor's load line."""
    p = TUBES[tube]
    vg = vg - (1 - GRID_SLOPE) * GRID_KNEE_V * np.logaddexp(0.0, vg / GRID_KNEE_V)
    lo, hi = np.zeros_like(vg), np.full_like(vg, plate_v)
    for _ in range(60):  # halve the range 60 times: far more exact than needed
        mid = (lo + hi) / 2
        too_high = mid + plate_ohms * koren_plate_current(mid, vg, **p) > plate_v
        hi = np.where(too_high, mid, hi)
        lo = np.where(too_high, lo, mid)
    return (lo + hi) / 2


# Power pentodes: Koren's pentode model, with kg1 and kp fitted so each tube matches its datasheet
# class A test point exactly (plate = screen = 250 V): EL34 -13.5 V 100 mA 11 mA/V, 6L6GC -14 V 72 mA
# 6 mA/V, EL84 -7.3 V 48 mA 11.3 mA/V, 6V6GT -12.5 V 45 mA 4.1 mA/V. mu is the tube's triode mu.
# rsat: a fully-on tube acts like this resistance, so the plate can not swing all the way down.
# Worked back from the known output of typical amps (EL34 pair 450 V 3.4k about 50 W, 6L6GC pair
# 460 V 4.2k about 45 W, EL84 pair 320 V 8k about 17 W, 6V6GT pair 420 V 8k about 22 W).
# It also covers the output transformer's losses.
PENTODES = {
    "EL34": {"mu": 11.0, "kg1": 682.9, "kp": 44.0, "rsat": 490.0},
    "6L6GC": {"mu": 8.7, "kg1": 1603.0, "kp": 44.8, "rsat": 520.0},
    "EL84": {"mu": 19.5, "kg1": 624.0, "kp": 158.7, "rsat": 450.0},
    "6V6GT": {"mu": 9.8, "kg1": 2205.0, "kp": 45.0, "rsat": 830.0},
}
PENTODE_EX = 1.35
PENTODE_KVB = 24.0


def pentode_current(vg, screen_v, plate_v, mu, kg1, kp, rsat=None):
    """Plate current (amps) of a pentode at grid voltage vg."""
    e1 = screen_v / kp * np.logaddexp(0.0, kp * (1 / mu + vg / screen_v))
    return 2 * np.maximum(e1, 0.0) ** PENTODE_EX / kg1 * np.arctan(plate_v / PENTODE_KVB)


def grid_clamp(vg):
    """Grid current: above 0 V the grid voltage can only rise slowly."""
    return vg - (1 - GRID_SLOPE) * GRID_KNEE_V * np.logaddexp(0.0, vg / GRID_KNEE_V)


def make_pushpull(tube, plate_v, screen_v, bias_v, raa_ohms, mismatch):
    """A push-pull pair of power tubes into an output transformer.

    Returns shape(x, h, sa, sb) for arrays, the same for single numbers, and info about the idle point.
    x  = drive in volts at the grids (tube A gets bias + x, tube B gets bias - x),
    h  = headroom from sag (the supply drops to h times its value),
    sa, sb = bias shift of each grid.
    The output is the transformer voltage, scaled so that full output equals |bias_v|.
    """
    if tube not in PENTODES:
        raise ValueError(f"unknown power tube '{tube}'. Use one of: {', '.join(PENTODES)}")
    if bias_v >= 0:
        raise ValueError(f"power bias_v must be below 0, got {bias_v}")
    if not 0 <= mismatch < 1:
        raise ValueError(f"mismatch must be from 0 up to (not including) 1, got {mismatch}")
    if plate_v <= 0 or raa_ohms <= 0:
        raise ValueError("power plate_v and raa_ohms must be above 0")
    p = PENTODES[tube]
    step = 0.05
    grid = np.arange(-400.0, 50.0 + step, step)
    current = pentode_current(grid, screen_v, plate_v, **p)
    lo, n, cur_list = float(grid[0]), len(grid), current.tolist()
    quarter = raa_ohms / 4  # each half of the primary, when one tube works alone
    vmax = plate_v * quarter / (quarter + p["rsat"])  # the biggest swing: rsat and the load share the supply
    scale = abs(bias_v) / vmax
    weaker = 1 - mismatch

    # The supply dropping to h times its value scales the tube's current curve (Koren's model is
    # homogeneous in the grid and screen voltages): I(vg, h*screen) = h**ex * I(vg/h, screen).
    def shape(x, h, sa, sb):
        ia = np.interp(grid_clamp(bias_v + sa + x) / h, grid, current)
        ib = np.interp(grid_clamp(bias_v + sb - x) / h, grid, current) * weaker
        v = quarter * (ia - ib) * h**PENTODE_EX / (h * vmax)
        return h * vmax * v / (1 + v**4) ** 0.25 * scale

    def lookup(vg):
        i = (vg - lo) / step
        if i <= 0:
            return cur_list[0]
        if i >= n - 1:
            return cur_list[-1]
        k = int(i)
        return cur_list[k] + (i - k) * (cur_list[k + 1] - cur_list[k])

    def clamp(vg):
        z = vg / GRID_KNEE_V
        soft = z + math.log1p(math.exp(-z)) if z > 0 else math.log1p(math.exp(z))
        return vg - (1 - GRID_SLOPE) * GRID_KNEE_V * soft

    def scalar_shape(x, h, sa, sb):
        ia = lookup(clamp(bias_v + sa + x) / h)
        ib = lookup(clamp(bias_v + sb - x) / h) * weaker
        v = quarter * (ia - ib) * h**PENTODE_EX / (h * vmax)
        return h * vmax * v / (1 + v**4) ** 0.25 * scale

    idle = float(pentode_current(np.array(bias_v), screen_v, plate_v, **p))
    peak = float(pentode_current(np.array(0.0), screen_v, plate_v, **p))
    swing = min(vmax, quarter * peak)
    info = {
        "idle_ma": idle * 1000,
        "idle_watts": idle * plate_v,  # heat in each tube when not playing
        "max_watts": swing**2 / (2 * quarter),  # rough clean output power of the pair
    }
    return shape, scalar_shape, info


def make_triode(tube, plate_v, plate_ohms, bias_v):
    """A preamp triode stage. Input = grid swing in volts around the bias point.

    Output = the plate swing divided by the stage's small-signal gain, and flipped back upright,
    so small signals come out the same size and the real tube's uneven clipping shapes the big ones.
    Returns also the operating point, for display and tests.
    """
    if tube not in TUBES:
        raise ValueError(f"unknown tube '{tube}'. Use one of: {', '.join(TUBES)}")
    if bias_v >= 0:
        raise ValueError(f"triode bias_v must be below 0 (the grid sits below the cathode), got {bias_v}")
    step = 0.01
    grid = np.arange(bias_v - 60.0, bias_v + 30.0 + step, step)
    plate = triode_plate_voltage(grid, tube, plate_v, plate_ohms)
    v0 = float(triode_plate_voltage(np.array([bias_v]), tube, plate_v, plate_ohms)[0])
    dv = 1e-3
    up, down = triode_plate_voltage(np.array([bias_v + dv, bias_v - dv]), tube, plate_v, plate_ohms)
    gain = (down - up) / (2 * dv)
    if gain <= 0:
        raise ValueError("triode: the tube is cut off at this bias (no gain). Use a bias_v closer to 0")
    xs = grid - bias_v
    ys = (v0 - plate) / gain
    lo_x, n = float(xs[0]), len(xs)
    ys_list = ys.tolist()

    def numpy_curve(x):
        return np.interp(x, xs, ys)

    def scalar_curve(x):
        i = (x - lo_x) / step
        if i <= 0:
            return ys_list[0]
        if i >= n - 1:
            return ys_list[-1]
        k = int(i)
        return ys_list[k] + (i - k) * (ys_list[k + 1] - ys_list[k])

    info = {"plate_v": v0, "plate_ma": (plate_v - v0) / plate_ohms * 1000, "gain": float(gain)}
    return numpy_curve, scalar_curve, -bias_v, info
