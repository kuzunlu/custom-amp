"""Turn a small circuit of resistors and capacitors into a digital filter.

A circuit is a list of parts between named points ("nodes"). "gnd" is ground.
The input is a voltage source with a small resistance, driving the node "in".
The output is the voltage at the node "out", which drives a load resistance to ground.

How it works:
1. Nodal analysis: for one frequency s, the node voltages v solve (G + s*C) v = b.
   G holds the resistors (1/R), C holds the capacitors. This is what circuit simulators do.
2. The answer H(s) = v_out / v_in is a ratio of two polynomials in s. Their coefficients are found
   by working out the circuit at a few points on a circle and using the FFT (exact, and stable).
3. The bilinear transform turns H(s) into a digital filter H(z) with the same response at audio frequencies.
"""
import numpy as np

REF_HZ = 1000.0  # the circle of test points has this radius (keeps the numbers well scaled)


def node_matrices(parts, source_ohms, load_ohms):
    """Build G and C for the circuit. parts: list of (kind, value, node_a, node_b), kind "R" or "C"."""
    names = sorted({n for _, _, a, b in parts for n in (a, b)} - {"gnd"})
    for needed in ("in", "out"):
        if needed not in names:
            raise ValueError(f"circuit has no node called '{needed}'")
    index = {n: i for i, n in enumerate(names)}
    size = len(names)
    g = np.zeros((size, size))
    c = np.zeros((size, size))

    def add(matrix, value, a, b):
        for n, m in ((a, b), (b, a)):
            if n != "gnd":
                matrix[index[n], index[n]] += value
                if m != "gnd":
                    matrix[index[n], index[m]] -= value

    for kind, value, a, b in parts:
        if kind == "R":
            add(g, 1.0 / value, a, b)
        elif kind == "C":
            add(c, value, a, b)
        else:
            raise ValueError(f"unknown part kind '{kind}'")
    # The source: a voltage behind source_ohms is the same as a current u/source_ohms into "in".
    add(g, 1.0 / source_ohms, "in", "gnd")
    add(g, 1.0 / load_ohms, "out", "gnd")
    b = np.zeros(size)
    b[index["in"]] = 1.0 / source_ohms
    return g, c, b, index["out"]


def analog_response(parts, freqs, source_ohms=1.0, load_ohms=1e6):
    """H at real frequencies (Hz), solved directly. Used to check the polynomial version."""
    g, c, b, out = node_matrices(parts, source_ohms, load_ohms)
    return np.array([np.linalg.solve(g + 2j * np.pi * f * c, b)[out] for f in freqs])


def analog_polynomials(parts, source_ohms=1.0, load_ohms=1e6):
    """Coefficients of H(s) = num(s) / den(s), lowest power first, with den[0] = 1."""
    g, c, b, out = node_matrices(parts, source_ohms, load_ohms)
    order = sum(1 for kind, *_ in parts if kind == "C")
    points = 2 * (order + 1)
    w0 = 2 * np.pi * REF_HZ
    den_values, num_values = [], []
    for k in range(points):
        s = w0 * np.exp(2j * np.pi * k / points)
        m = g + s * c
        d = np.linalg.det(m)
        den_values.append(d)
        num_values.append(np.linalg.solve(m, b)[out] * d)
    scale = w0 ** np.arange(points)
    den = (np.fft.fft(den_values) / points / scale)[: order + 1].real
    num = (np.fft.fft(num_values) / points / scale)[: order + 1].real
    return num / den[0], den / den[0]


def bilinear(num, den, sr):
    """Analog H(s) to digital H(z) with s = 2*sr*(1 - 1/z)/(1 + 1/z). Returns (b, a) with a[0] = 1."""
    n = len(den) - 1
    k = 2.0 * sr
    minus, plus = np.array([1.0, -1.0]), np.array([1.0, 1.0])
    b = np.zeros(n + 1)
    a = np.zeros(n + 1)
    for power in range(n + 1):
        term = np.array([k**power])
        for _ in range(power):
            term = np.convolve(term, minus)
        for _ in range(n - power):
            term = np.convolve(term, plus)
        b += num[power] * term
        a += den[power] * term
    return b / a[0], a / a[0]


def digital_filter(parts, sr, source_ohms=1.0, load_ohms=1e6):
    return bilinear(*analog_polynomials(parts, source_ohms, load_ohms), sr)
