#pragma once

#include "Common.h"

#include <vector>

namespace amp
{

// log(1 + exp(z)) without overflow (numpy's logaddexp(0, z)).
inline double softplus (double z) noexcept
{
    return std::max (z, 0.0) + std::log1p (std::exp (-std::abs (z)));
}

// Grid current: above 0 V the grid voltage can only rise slowly (about 2% of the input) after a soft
// corner about 0.1 V wide (GRID_SLOPE and GRID_KNEE_V in curves.py).
constexpr double gridSlope = 0.02;
constexpr double gridKneeV = 0.1;

inline double gridClamp (double vg) noexcept
{
    const double z = vg / gridKneeV;

    if (z < -40.0)
        return vg;  // the correction is below 1e-17 V here, so it would not change vg at all

    const double soft = z > 0 ? z + std::log1p (std::exp (-z)) : std::log1p (std::exp (z));
    return vg - (1 - gridSlope) * gridKneeV * soft;
}

// A clipping curve: a fixed shape, the 'custom' curve, or a table (the 'triode' curve).
struct Curve
{
    enum class Kind { hard, soft, tube, diode, custom, table };

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
            case Kind::table: break;
        }

        const double i = (x - tableLo) / tableStep;

        if (i <= 0)
            return table.front();

        if (i >= (double) (table.size() - 1))
            return table.back();

        const auto k = (size_t) i;
        return table[k] + (i - (double) k) * (table[k + 1] - table[k]);
    }

    Kind kind = Kind::soft;
    double top = 1.0, topPower = 1.0, bottom = 1.0, bottomPower = 1.0;
    std::vector<double> table;
    double tableLo = 0.0, tableStep = 0.01;
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

// A push-pull pair of power tubes into an output transformer (make_pushpull in curves.py).
struct PushPull
{
    static PushPull make (const juce::String& tube, double plateV, double screenV, double biasV,
                          double raaOhms, double mismatch);

    double lookup (double vg) const noexcept
    {
        const double i = (vg - lo) / step;

        if (i <= 0)
            return current.front();

        if (i >= (double) (current.size() - 1))
            return current.back();

        const auto k = (size_t) i;
        return current[k] + (i - (double) k) * (current[k + 1] - current[k]);
    }

    // x = drive in volts at the grids, h = headroom from sag, hPow = pow (h, pentodeEx),
    // sa / sb = bias shift of each grid. Full output equals |bias_v|.
    double shape (double x, double h, double hPow, double sa, double sb) const noexcept
    {
        const double ia = lookup (gridClamp (bias + sa + x) / h);
        const double ib = lookup (gridClamp (bias + sb - x) / h) * weaker;
        const double v = quarter * (ia - ib) * hPow / (h * vmax);
        return h * vmax * v / std::sqrt (std::sqrt (1 + v * v * v * v)) * scale;
    }

    std::vector<double> current;
    double lo = -400.0, step = 0.05;
    double bias = -45, quarter = 850, vmax = 1, scale = 1, weaker = 1;
    double idleMa = 0, idleWatts = 0, maxWatts = 0;
};

constexpr double pentodeEx = 1.35;
juce::StringArray pentodeNames();

// np.arange (start, stop, step): the same count and the same values as NumPy makes them.
std::vector<double> numpyArange (double start, double stop, double step);

} // namespace amp
