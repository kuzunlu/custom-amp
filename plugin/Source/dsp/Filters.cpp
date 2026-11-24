#include "Filters.h"

namespace amp
{

static const juce::StringArray kinds { "highpass", "lowpass", "lowshelf", "highshelf", "peak", "highpass1", "lowpass1" };

static bool isFirstOrder (const juce::String& kind) { return kind == "highpass1" || kind == "lowpass1"; }

Biquad Biquad::make (const juce::String& kind, double freq, double rate, double q, double gainDb, double checkRate)
{
    if (! kinds.contains (kind))
        throw PresetError ("unknown filter kind '" + kind + "'. Use one of: " + kinds.joinIntoString (", "));

    if (! (freq > 0 && freq < checkRate / 2))
        throw PresetError ("filter freq " + juce::String (freq) + " Hz must be between 0 and "
                           + juce::String (checkRate / 2) + " Hz");

    if (isFirstOrder (kind))
    {
        // 1st order: one capacitor and one resistor (a coupling capacitor into a grid resistor, a transformer's
        // low end). The bilinear transform with its -3 dB corner exactly at freq.
        const double k = std::tan (juce::MathConstants<double>::pi * freq / rate);
        Biquad f;
        f.b0 = kind == "highpass1" ? 1 / (1 + k) : k / (1 + k);
        f.b1 = kind == "highpass1" ? -1 / (1 + k) : k / (1 + k);
        f.a1 = (k - 1) / (k + 1);
        return f;
    }

    const double w0 = 2 * juce::MathConstants<double>::pi * freq / rate;
    const double cosW = std::cos (w0), sinW = std::sin (w0);
    const double alpha = sinW / (2 * q);
    const double a = std::pow (10.0, gainDb / 40);
    const double sq = 2 * std::sqrt (a) * alpha;
    double b[3], den[3];

    if (kind == "lowpass")
    {
        b[0] = (1 - cosW) / 2; b[1] = 1 - cosW; b[2] = (1 - cosW) / 2;
        den[0] = 1 + alpha; den[1] = -2 * cosW; den[2] = 1 - alpha;
    }
    else if (kind == "highpass")
    {
        b[0] = (1 + cosW) / 2; b[1] = -(1 + cosW); b[2] = (1 + cosW) / 2;
        den[0] = 1 + alpha; den[1] = -2 * cosW; den[2] = 1 - alpha;
    }
    else if (kind == "peak")
    {
        b[0] = 1 + alpha * a; b[1] = -2 * cosW; b[2] = 1 - alpha * a;
        den[0] = 1 + alpha / a; den[1] = -2 * cosW; den[2] = 1 - alpha / a;
    }
    else if (kind == "lowshelf")
    {
        b[0] = a * ((a + 1) - (a - 1) * cosW + sq);
        b[1] = 2 * a * ((a - 1) - (a + 1) * cosW);
        b[2] = a * ((a + 1) - (a - 1) * cosW - sq);
        den[0] = (a + 1) + (a - 1) * cosW + sq;
        den[1] = -2 * ((a - 1) + (a + 1) * cosW);
        den[2] = (a + 1) + (a - 1) * cosW - sq;
    }
    else // highshelf
    {
        b[0] = a * ((a + 1) + (a - 1) * cosW + sq);
        b[1] = -2 * a * ((a - 1) + (a + 1) * cosW);
        b[2] = a * ((a + 1) + (a - 1) * cosW - sq);
        den[0] = (a + 1) - (a - 1) * cosW + sq;
        den[1] = 2 * ((a - 1) - (a + 1) * cosW);
        den[2] = (a + 1) - (a - 1) * cosW - sq;
    }

    Biquad f;
    const double a0 = den[0];
    f.b0 = b[0] / a0; f.b1 = b[1] / a0; f.b2 = b[2] / a0;
    f.a1 = den[1] / a0; f.a2 = den[2] / a0;
    return f;
}

FilterList makeFilters (const juce::Array<juce::var>& bands, const BuildContext& ctx)
{
    FilterList list;

    for (auto& band : bands)
    {
        const auto where = "filter band " + juce::JSON::toString (band, true);
        Cfg cfg (band, where);
        cfg.checkKeys ({ "kind", "freq", "q", "gain_db" });

        if (! cfg.has ("kind") || ! cfg.has ("freq"))
            throw PresetError (where + ": needs a 'kind' and a 'freq'");

        if (isFirstOrder (cfg.text ("kind", {})) && (cfg.has ("q") || cfg.has ("gain_db")))
            throw PresetError (where + ": a 1st-order " + cfg.text ("kind", {}) + " has no q or gain_db");

        list.filters.push_back (Biquad::make (cfg.text ("kind", {}), cfg.number ("freq", 0), ctx.rate,
                                              cfg.number ("q", 0.707), cfg.number ("gain_db", 0.0), ctx.checkRate));
    }

    return list;
}

Iir::Iir (std::vector<double> bIn, std::vector<double> aIn)
    : b (std::move (bIn)), a (std::move (aIn)), z (a.size() - 1, 0.0)
{
    jassert (b.size() == a.size() && a.size() >= 2);
}

} // namespace amp
