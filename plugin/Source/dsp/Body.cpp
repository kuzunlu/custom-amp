#include "Body.h"

#include "Strings.h"

#include <cmath>
#include <complex>

namespace amp
{

std::vector<double> Body::modeRatios (const juce::String& object, double highest)
{
    std::vector<double> r;
    const double pi = juce::MathConstants<double>::pi;
    const auto bisect = [] (auto f, double lo, double hi)
    {
        for (int i = 0; i < 100; ++i)
        {
            const double mid = 0.5 * (lo + hi);
            (f (lo) * f (mid) <= 0 ? hi : lo) = mid;
        }

        return 0.5 * (lo + hi);
    };

    if (object == "glass")
    {
        const auto ring = [] (double n) { return n * (n * n - 1) / std::sqrt (n * n + 1); };

        for (int n = 2; ring (n) / ring (2) <= highest; ++n)
            r.push_back (ring (n) / ring (2));
    }
    else if (object == "bar")
    {
        // cos x cosh x = 1 has one root between k pi and (k + 1) pi for k = 1, 2, ...
        const auto f = [] (double x) { return std::cos (x) - 1 / std::cosh (x); };
        const double first = bisect (f, pi, 2 * pi);

        for (int k = 1;; ++k)
        {
            const double x = bisect (f, k * pi, (k + 1) * pi);

            if ((x / first) * (x / first) > highest)
                break;

            r.push_back ((x / first) * (x / first));
        }
    }
    else if (object == "membrane")
    {
        // Every zero of J_0, J_1, ... (found where J_m changes sign, then bisected), against the first zero of J_0.
        const double first = bisect ([] (double x) { return std::cyl_bessel_j (0.0, x); }, 2, 3);

        for (int m = 0; m < 200; ++m)
        {
            const auto j = [m] (double x) { return std::cyl_bessel_j ((double) m, x); };
            bool any = false;

            for (double x = std::max (0.5, (double) m); x < highest * first; x += 0.05)
                if (j (x) * j (x + 0.05) < 0)
                {
                    r.push_back (bisect (j, x, x + 0.05) / first);
                    any = true;
                }

            if (! any)
                break;  // the first zero of J_m is beyond the highest: so are those of every larger m
        }

        std::sort (r.begin(), r.end());
    }
    else if (object == "plate")
    {
        for (int m = 1; m * m <= 3 * highest; ++m)
            for (int n = 1; m * m + 2 * n * n <= 3 * highest; ++n)
                r.push_back ((m * m + 2.0 * n * n) / 3.0);

        std::sort (r.begin(), r.end());
    }
    else if (object == "bell")
    {
        for (double p : { 1.0, 2.0, 2.4, 3.0, 4.0, 5.0, 16.0 / 3, 6.0, 8.0 })
            if (p <= highest)
                r.push_back (p);
    }

    return r;
}

Body::Body (const Cfg& cfg, const BuildContext& ctx)
    : Block (cfg, { "object", "note", "decay_s", "high_damping", "coupling_db", "mix_db" })
{
    const double rate = ctx.rate;
    object = cfg.text ("object", "glass");
    const double decay = cfg.number ("decay_s", 3.0), damping = cfg.number ("high_damping", 1.0);
    const double coupling = dbToGain (cfg.number ("coupling_db", -40.0));
    mix = dbToGain (cfg.number ("mix_db", 0.0));

    if (! juce::StringArray { "glass", "bar", "membrane", "plate", "bell" }.contains (object))
        throw PresetError ("body: object must be glass, bar, membrane, plate or bell, got \"" + object + "\"");

    if (decay <= 0 || damping < 0)
        throw PresetError ("body: decay_s must be above 0 and high_damping 0 or more");

    // The lowest mode: a note name or Hz.
    const auto note = cfg.get ("note");
    const auto text = note.isVoid() ? juce::String ("A5") : note.toString().trim();
    double lowest = 0;

    try
    {
        lowest = SympatheticStrings::noteHz (text.containsOnly ("0123456789.") ? juce::var (text.getDoubleValue()) : juce::var (text));
    }
    catch (const PresetError&)
    {
        throw PresetError ("body: note '" + text + "' is not a note from 20 Hz to 5 kHz. Use a name like A5, F#4 or Bb3, or a number of Hz");
    }

    // Every mode up to 16 kHz (and well below half the rate), at most 64.
    const double top = std::min (16000.0, 0.45 * ctx.checkRate);

    for (double ratio : modeRatios (object, top / lowest))
    {
        if ((int) modes.size() >= 64)
            break;

        Mode m;
        m.hz = lowest * ratio;
        const double w = 2 * juce::MathConstants<double>::pi * m.hz / rate;
        const double t60 = decay * std::pow (lowest / m.hz, damping);
        const double r = std::pow (10.0, -3.0 / (t60 * rate));  // per sample
        m.a1 = 2 * r * std::cos (w);
        m.a2 = -r * r;

        // At its own frequency it answers coupling / (1 - g), g = what it keeps per period (as a sympathetic string).
        const double keep = std::pow (10.0, -3.0 / (m.hz * t60));
        const auto z = std::polar (1.0, -w);
        const double den = std::abs (1.0 - m.a1 * z - m.a2 * z * z), num = std::abs (1.0 - z * z);
        m.b0 = coupling / (1 - keep) * den / num;
        modes.push_back (m);
    }
}

void Body::reset() noexcept
{
    for (auto& m : modes)
        m.x1 = m.x2 = m.y1 = m.y2 = 0.0;
}

void Body::process (double* x, int n) noexcept
{
    for (int i = 0; i < n; ++i)
    {
        const double in = x[i];
        double sum = 0.0;

        for (auto& m : modes)
        {
            const double y = m.b0 * (in - m.x2) + m.a1 * m.y1 + m.a2 * m.y2;
            m.x2 = m.x1;
            m.x1 = in;
            m.y2 = m.y1;
            m.y1 = y;
            sum += y;
        }

        x[i] = in + mix * sum;
    }
}

juce::String Body::describe() const
{
    if (modes.empty())
        return "body: " + object + ", no modes";

    return "body: " + object + ", " + juce::String ((int) modes.size()) + " modes from " + juce::String (modes.front().hz, 1) + " to "
           + juce::String (modes.back().hz, 0) + " Hz";
}

} // namespace amp
