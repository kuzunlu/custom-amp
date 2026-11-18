#include "Curves.h"

#include <map>

namespace amp
{

std::vector<double> numpyArange (double start, double stop, double step)
{
    // NumPy fills arange as start + i * delta, with delta = (start + step) - start.
    const auto n = (size_t) std::ceil ((stop - start) / step);
    std::vector<double> values (n);
    const double delta = (start + step) - start;

    for (size_t i = 0; i < n; ++i)
        values[i] = i == 0 ? start : (i == 1 ? start + step : start + (double) i * delta);

    return values;
}

static double softnessToPower (double softness)
{
    return std::pow (2.0, 3 * (1 - softness));  // 0 = nearly hard corner, 1 = very soft corner
}

Curve makeCustomCurve (double top, double topSoftness, double bottom, double bottomSoftness)
{
    if (top <= 0 || bottom <= 0)
        throw PresetError ("custom curve: top and bottom must be above 0");

    if (topSoftness < 0 || topSoftness > 1)
        throw PresetError ("custom curve: top_softness must be from 0 to 1, got " + juce::String (topSoftness));

    if (bottomSoftness < 0 || bottomSoftness > 1)
        throw PresetError ("custom curve: bottom_softness must be from 0 to 1, got " + juce::String (bottomSoftness));

    Curve c;
    c.kind = Curve::Kind::custom;
    c.top = top;
    c.bottom = bottom;
    c.topPower = softnessToPower (topSoftness);
    c.bottomPower = softnessToPower (bottomSoftness);
    return c;
}

//==============================================================================
// Koren's triode model numbers (N. Koren, "Improved VT models for SPICE simulations", 1996).
struct TriodeModel { double mu, ex, kg1, kp, kvb; };

static const std::map<juce::String, TriodeModel>& triodes()
{
    static const std::map<juce::String, TriodeModel> tubes {
        { "12AX7", { 100.0, 1.4, 1060.0, 600.0, 300.0 } },
        { "12AT7", { 60.0, 1.35, 460.0, 300.0, 300.0 } },
        { "12AU7", { 21.5, 1.3, 1180.0, 84.0, 300.0 } },
    };
    return tubes;
}

juce::StringArray triodeNames() { return { "12AX7", "12AT7", "12AU7" }; }

static double korenPlateCurrent (double vp, double vg, const TriodeModel& p)
{
    const double e1 = vp / p.kp * softplus (p.kp * (1 / p.mu + vg / std::sqrt (p.kvb + vp * vp)));
    return 2 * std::pow (std::max (e1, 0.0), p.ex) / p.kg1;
}

// Plate voltage for one grid voltage: where the tube's current meets the plate resistor's load line.
static double triodePlateVoltage (double vg, const TriodeModel& p, double plateV, double plateOhms)
{
    vg = vg - (1 - gridSlope) * gridKneeV * softplus (vg / gridKneeV);
    double lo = 0.0, hi = plateV;

    for (int i = 0; i < 60; ++i)  // halve the range 60 times: far more exact than needed
    {
        const double mid = (lo + hi) / 2;

        if (mid + plateOhms * korenPlateCurrent (mid, vg, p) > plateV)
            hi = mid;
        else
            lo = mid;
    }

    return (lo + hi) / 2;
}

Curve makeTriodeCurve (const juce::String& tube, double plateV, double plateOhms, double biasV, TriodeInfo& info)
{
    auto found = triodes().find (tube);

    if (found == triodes().end())
        throw PresetError ("unknown tube '" + tube + "'. Use one of: " + triodeNames().joinIntoString (", "));

    if (biasV >= 0)
        throw PresetError ("triode bias_v must be below 0 (the grid sits below the cathode), got " + juce::String (biasV));

    const auto& p = found->second;
    const double step = 0.01;
    const auto grid = numpyArange (biasV - 60.0, biasV + 30.0 + step, step);
    const double v0 = triodePlateVoltage (biasV, p, plateV, plateOhms);
    const double dv = 1e-3;
    const double up = triodePlateVoltage (biasV + dv, p, plateV, plateOhms);
    const double down = triodePlateVoltage (biasV - dv, p, plateV, plateOhms);
    const double gain = (down - up) / (2 * dv);

    if (gain <= 0)
        throw PresetError ("triode: the tube is cut off at this bias (no gain). Use a bias_v closer to 0");

    Curve c;
    c.kind = Curve::Kind::table;
    c.tableStep = step;
    c.tableLo = grid.front() - biasV;
    c.table.resize (grid.size());

    for (size_t i = 0; i < grid.size(); ++i)
        c.table[i] = (v0 - triodePlateVoltage (grid[i], p, plateV, plateOhms)) / gain;

    info.plateV = v0;
    info.plateMa = (plateV - v0) / plateOhms * 1000;
    info.gain = gain;
    return c;
}

//==============================================================================
// Power pentodes: Koren's pentode model, kg1 and kp fitted to each datasheet's class A point
// (see PENTODES in curves.py). rsat: a fully-on tube acts like this resistance.
struct PentodeModel { double mu, kg1, kp, rsat; };

static const std::map<juce::String, PentodeModel>& pentodes()
{
    static const std::map<juce::String, PentodeModel> tubes {
        { "EL34", { 11.0, 682.9, 44.0, 490.0 } },
        { "6L6GC", { 8.7, 1603.0, 44.8, 520.0 } },
        { "EL84", { 19.5, 624.0, 158.7, 450.0 } },
        { "6V6GT", { 9.8, 2205.0, 45.0, 830.0 } },
    };
    return tubes;
}

juce::StringArray pentodeNames() { return { "EL34", "6L6GC", "EL84", "6V6GT" }; }

static constexpr double pentodeKvb = 24.0;

static double pentodeCurrent (double vg, double screenV, double plateV, const PentodeModel& p)
{
    const double e1 = screenV / p.kp * softplus (p.kp * (1 / p.mu + vg / screenV));
    return 2 * std::pow (std::max (e1, 0.0), pentodeEx) / p.kg1 * std::atan (plateV / pentodeKvb);
}

PushPull PushPull::make (const juce::String& tube, double plateV, double screenV, double biasV,
                         double raaOhms, double mismatch)
{
    auto found = pentodes().find (tube);

    if (found == pentodes().end())
        throw PresetError ("unknown power tube '" + tube + "'. Use one of: " + pentodeNames().joinIntoString (", "));

    if (biasV >= 0)
        throw PresetError ("power bias_v must be below 0, got " + juce::String (biasV));

    if (mismatch < 0 || mismatch >= 1)
        throw PresetError ("mismatch must be from 0 up to (not including) 1, got " + juce::String (mismatch));

    if (plateV <= 0 || raaOhms <= 0)
        throw PresetError ("power plate_v and raa_ohms must be above 0");

    const auto& p = found->second;
    PushPull pp;
    const auto grid = numpyArange (-400.0, 50.0 + pp.step, pp.step);
    pp.lo = grid.front();
    pp.current.resize (grid.size());

    for (size_t i = 0; i < grid.size(); ++i)
        pp.current[i] = pentodeCurrent (grid[i], screenV, plateV, p);

    pp.bias = biasV;
    pp.quarter = raaOhms / 4;  // each half of the primary, when one tube works alone
    pp.vmax = plateV * pp.quarter / (pp.quarter + p.rsat);  // rsat and the load share the supply
    pp.scale = std::abs (biasV) / pp.vmax;
    pp.weaker = 1 - mismatch;

    const double idle = pentodeCurrent (biasV, screenV, plateV, p);
    const double peak = pentodeCurrent (0.0, screenV, plateV, p);
    const double swing = std::min (pp.vmax, pp.quarter * peak);
    pp.idleMa = idle * 1000;
    pp.idleWatts = idle * plateV;  // heat in each tube when not playing
    pp.maxWatts = swing * swing / (2 * pp.quarter);  // rough clean output power of the pair
    return pp;
}

} // namespace amp
