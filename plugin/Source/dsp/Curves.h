#pragma once

#include "Common.h"

#include <vector>

namespace amp
{

// log(1 + exp(z)) without overflow.
inline double softplus (double z) noexcept
{
    return std::max (z, 0.0) + std::log1p (std::exp (-std::abs (z)));
}

// Grid current: above 0 V the grid voltage can only rise slowly (about 2% of the input) after a soft
// corner about 0.1 V wide.
constexpr double gridSlope = 0.02;
constexpr double gridKneeV = 0.1;

inline double gridClamp (double vg) noexcept
{
    const double z = vg * (1 / gridKneeV);  // a multiply is faster than a divide (in feedback loops, every try waits for it)

    if (z < -40.0)
        return vg;  // the correction is below 1e-17 V here, so it would not change vg at all

    const double soft = z > 0 ? z + std::log1p (std::exp (-z)) : std::log1p (std::exp (z));
    return vg - (1 - gridSlope) * gridKneeV * soft;
}

// A clipping curve: a fixed shape, the 'custom' curve, or a table (the 'triode' curve).
struct Curve
{
    enum class Kind { hard, soft, tube, diode, custom, table, line };

    double operator() (double x) const noexcept
    {
        switch (kind)
        {
            case Kind::hard:  return std::max (-1.0, std::min (1.0, x));
            case Kind::soft:  return std::tanh (x);
            case Kind::tube:  return x >= 0 ? std::tanh (x) : 1.5 * std::tanh (x / 1.5);
            case Kind::diode: return x / std::pow (1 + std::pow (std::abs (x), 2.5), 1 / 2.5);
            case Kind::custom:
            {
                const double level = x >= 0 ? top : bottom;
                const double p = x >= 0 ? topPower : bottomPower;
                return x / std::pow (1 + std::pow (std::abs (x / level), p), 1 / p);
            }
            case Kind::line:  return slope * x;  // no clipping at all (a stage's linear twin, for X-ray listening)
            case Kind::table: break;
        }

        const double i = (x - tableLo) * tablePerVolt;

        if (i <= 0)
            return table.front();

        if (i >= (double) (table.size() - 1))
            return table.back();

        const auto k = (size_t) i;
        return table[k] + (i - (double) k) * (table[k + 1] - table[k]);
    }

    Kind kind = Kind::soft;
    double top = 1.0, topPower = 1.0, bottom = 1.0, bottomPower = 1.0, slope = 1.0;
    std::vector<double> table;
    double tableLo = 0.0, tablePerVolt = 100.0;  // 1 / the table step
};

Curve makeCustomCurve (double top, double topSoftness, double bottom, double bottomSoftness);

struct TriodeInfo
{
    double plateV = 0, plateMa = 0, gain = 0;
};

// A preamp triode stage (Koren 1996 model + plate resistor load line). Input = grid swing in volts
// around the bias point. Output = plate swing / small-signal gain, flipped upright.
Curve makeTriodeCurve (const juce::String& tube, double plateV, double plateOhms, double biasV, TriodeInfo& info);
juce::StringArray triodeNames();

constexpr double pentodeEx = 1.35;

// A push-pull pair of power tubes into an output transformer.
struct PushPull
{
    // pairs = how many pairs work in parallel (2 pairs = the 4 power tubes of a 100 W amp).
    static PushPull make (const juce::String& tube, double plateV, double screenV, double biasV,
                          double raaOhms, double mismatch, int pairs = 1);

    double lookup (double vg) const noexcept
    {
        const double i = (vg - lo) * perVolt;

        if (i <= 0)
            return current.front();

        if (i >= (double) (current.size() - 1))
            return current.back();

        const auto k = (size_t) i;
        return current[k] + (i - (double) k) * (current[k + 1] - current[k]);
    }

    // What shape() needs for one headroom h from sag, worked out only when h changes (not on every try).
    struct Supply
    {
        double perH = 1, in = 1, out = 1, hPow = 1;
    };

    Supply supply (double h) const noexcept
    {
        const double hPow = std::pow (h, pentodeEx);
        return { 1 / h, quarter * hPow / (h * vmax), h * vmax * scale, hPow };
    }

    // x = drive in volts at the grids, s = supply (h), sa / sb = bias shift of each grid. Full output equals |bias_v|.
    // draw = the current both tubes take from the supply, in amps (the scaling rule above: h^1.35 * I(vg / h)).
    double shape (double x, const Supply& s, double sa, double sb, double& draw) const noexcept
    {
        const double ia = lookup (gridClamp (bias + sa + x) * s.perH);
        const double ib = lookup (gridClamp (bias + sb - x) * s.perH) * weaker;
        draw = (ia + ib) * s.hPow;
        const double v = (ia - ib) * s.in;
        return s.out * v / std::sqrt (std::sqrt (1 + v * v * v * v));
    }

    // The pair without its output soft limit, at full supply and no bias shift. The real pair is never
    // steeper: sag scales its slope by pow (h, pentodeEx - 1) <= 1, the soft limit only flattens it.
    double unlimited (double x) const noexcept
    {
        const double ia = lookup (gridClamp (bias + x));
        const double ib = lookup (gridClamp (bias - x)) * weaker;
        return quarter * (ia - ib) * scale;
    }

    std::vector<double> current;
    double lo = -400.0, step = 0.05, perVolt = 20.0;  // perVolt = 1 / step
    double bias = -45, quarter = 850, vmax = 1, scale = 1, weaker = 1;
    double idleMa = 0, idleWatts = 0, maxWatts = 0;  // idle current and heat per tube, output of all of them
    int pairs = 1;
};

juce::StringArray pentodeNames();

// One power tube's plate current in amps (Koren's pentode model with the tube's fitted values), for the theory checks.
double pentodeCurrent (const juce::String& tube, double vg, double screenV, double plateV);

// np.arange (start, stop, step): the same count and values as NumPy makes them. The tube tables were first made
// with NumPy; keeping its grid keeps the sound exactly the same.
std::vector<double> numpyArange (double start, double stop, double step);

} // namespace amp
