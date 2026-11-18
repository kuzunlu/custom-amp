#pragma once

#include "Common.h"

#include <vector>

namespace amp
{

// One 2nd-order filter. Formulas from the RBJ "Audio EQ Cookbook" (same as ampsim/filters.py).
struct Biquad
{
    static Biquad make (const juce::String& kind, double freq, double rate, double q, double gainDb, double checkRate);

    double tick (double v) noexcept
    {
        const double y = b0 * v + z1;
        z1 = b1 * v - a1 * y + z2;
        z2 = b2 * v - a2 * y;
        return y;
    }

    // What tick() would give, without moving on to the next sample (for feedback loops).
    double peek (double v) const noexcept { return b0 * v + z1; }

    void reset() noexcept { z1 = z2 = 0.0; }

    double b0 = 1.0, b1 = 0.0, b2 = 0.0, a1 = 0.0, a2 = 0.0;
    double z1 = 0.0, z2 = 0.0;
};

// Filters applied one after another.
struct FilterList
{
    double tick (double v) noexcept
    {
        for (auto& f : filters)
            v = f.tick (v);
        return v;
    }

    double peek (double v) const noexcept
    {
        // peek() of a chain: each filter sees what the one before it would put out now.
        for (auto& f : filters)
            v = f.peek (v);
        return v;
    }

    void reset() noexcept
    {
        for (auto& f : filters)
            f.reset();
    }

    std::vector<Biquad> filters;
};

// Build filters from preset bands like {"kind": "peak", "freq": 700, "gain_db": -6, "q": 0.7}.
FilterList makeFilters (const juce::Array<juce::var>& bands, const BuildContext& ctx);

// A filter of any order from its coefficients (b, a with a[0] = 1), like Biquad but general.
struct Iir
{
    Iir() = default;
    Iir (std::vector<double> bIn, std::vector<double> aIn);

    double tick (double v) noexcept
    {
        const double y = b[0] * v + z[0];
        const size_t last = z.size() - 1;

        for (size_t k = 0; k < last; ++k)
            z[k] = b[k + 1] * v - a[k + 1] * y + z[k + 1];

        z[last] = b[last + 1] * v - a[last + 1] * y;
        return y;
    }

    double peek (double v) const noexcept { return b[0] * v + z[0]; }
    void reset() noexcept { std::fill (z.begin(), z.end(), 0.0); }

    std::vector<double> b, a, z;
};

} // namespace amp
