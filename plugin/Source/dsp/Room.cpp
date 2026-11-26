#include "Room.h"

#include <complex>

namespace amp
{

Room::Room (const Cfg& cfg, const BuildContext& ctx)
    : Block (cfg, { "distance_m", "volume_db", "sustain_s", "string_tone_hz", "mute_db", "harmonic", "find_spot" })
{
    rate = ctx.rate;
    distance = cfg.number ("distance_m", 0.5);
    sustainS = cfg.number ("sustain_s", 3.0);
    harmonic = cfg.number ("harmonic", 1.0);
    findSpot = cfg.flag ("find_spot", false);

    if (harmonic < 1 || harmonic > 8 || harmonic != std::floor (harmonic))
        throw PresetError ("room: harmonic must be a whole number from 1 to 8, got " + juce::String (harmonic));

    const double toneHz = cfg.number ("string_tone_hz", 1000.0);

    if (distance < 0.1 || distance > 20)
        throw PresetError ("room: distance_m must be from 0.1 to 20, got " + juce::String (distance));

    if (sustainS <= 0 || toneHz <= 0 || toneHz >= rate / 2)
        throw PresetError ("room: sustain_s must be above 0 and string_tone_hz between 0 and half the sample rate");

    // The string loses its highs on every round trip: a 1-pole low-pass in the loop.
    tone = 1 - std::exp (-2 * juce::MathConstants<double>::pi * toneHz / rate);

    // Sound pressure falls with distance (6 dB less at twice the distance); volume_db is the level at 1 m.
    coupling = dbToGain (cfg.number ("volume_db", -20.0)) / distance;
    muteLevel = dbToGain (cfg.number ("mute_db", -60.0));  // volts at the input (a guitar: about 0.5 V played hard)
    delay = std::max (1, juce::roundToInt (distance / 343.0 * rate));  // the speed of sound
    chunk = std::min (delay, 64);
    player.prepare (rate, muteLevel);
    // With find_spot the sound may arrive up to one period (of a 50 Hz note) later than from distance_m.
    air.assign ((size_t) (delay + chunk + 2) + (findSpot ? (size_t) (rate / 50.0) + 2 : 0), 0.0);
    spotRate = 1.0 / (0.1 * rate);      // the player moves: about 100 ms to make up a phase error
    spotAverage = 1 - std::exp (-1 / (0.03 * rate));  // the phases are measured over about 30 ms
    string.assign ((size_t) (rate / 50.0) + 4, 0.0);  // one period of the lowest note it follows (60 Hz) fits
}

void Room::prepare (int maxSamples)
{
    juce::ignoreUnused (maxSamples);  // the engine never gives it more than maxChunk() at a time
}

void Room::reset() noexcept
{
    std::fill (air.begin(), air.end(), 0.0);
    std::fill (string.begin(), string.end(), 0.0);
    player.reset();
    airWrite = stringWrite = 0;
    smooth = 0.0;
    period = readPeriod = loopGain = mutedGain = 0.0;
    muted = true;
    extra = 0.0;
    pushSum = ringSum = {};
    phasor = 1.0;
}

void Room::addFeedback (double* input, int n) noexcept
{
    const int airSize = (int) air.size(), stringSize = (int) string.size();

    for (int i = 0; i < n; ++i)
    {
        const double di = input[i];

        // The note being played and a hand on the strings (Player: stands in for the guitarist).
        if (player.hear (di))
            setNote (player.period());

        muted = player.isMuted();

        // The string: the sound that left the speaker `delay` samples ago pushes it, and what it did one period ago
        // comes round again (a little weaker, the higher harmonics more so).
        double s = 0.0;

        if (period > 0)
        {
            double pushed;

            if (! findSpot)
            {
                pushed = coupling * air[(size_t) ((airWrite + i - delay + airSize) % airSize)];
            }
            else
            {
                // The sound left the speaker delay + extra samples ago (between two samples).
                const double at = airWrite + i - delay - extra + 2.0 * airSize;
                const auto a = (int) at;
                const double f = at - a;
                pushed = coupling * (air[(size_t) (a % airSize)] * (1 - f) + air[(size_t) ((a + 1) % airSize)] * f);
            }

            const double back = stringWrite - readPeriod;
            const double from = back < 0 ? back + stringSize : back;
            const auto k = (int) from;
            const double frac = from - k;
            const double old = string[(size_t) k] * (1 - frac) + string[(size_t) ((k + 1) % stringSize)] * frac;
            smooth += tone * (old - smooth);
            // A muted string can not move: the push does not reach the pickup, and what still rings dies in 50 ms.
            s = muted ? mutedGain * smooth : pushed + loopGain * smooth;

            // find_spot (stands in for the player, like the pitch tracker): at the chosen harmonic, compare the push
            // with what the string is ringing; when the push comes early, arrive later, and the other way round, until
            // they are in step and that harmonic grows. Within one period of it, so the distance stays near distance_m.
            if (findSpot && ! muted)
            {
                const double ringing = loopGain * smooth;
                pushSum += spotAverage * (pushed * phasor - pushSum);
                ringSum += spotAverage * (ringing * phasor - ringSum);
                const double early = std::arg (pushSum * std::conj (ringSum));  // > 0: the push leads the ringing
                const double ringPeriod = readPeriod > 0 ? period / harmonic : 0.0;
                extra += spotRate * early / spotStep;
                extra = extra < 0 ? extra + ringPeriod : extra >= ringPeriod ? extra - ringPeriod : extra;
                phasor *= std::polar (1.0, -spotStep);
                phasor /= std::abs (phasor);
            }
        }

        string[(size_t) stringWrite] = s;
        stringWrite = (stringWrite + 1) % stringSize;
        input[i] = di + s;

        if (probe != nullptr)
            probe->push_back ((float) s);
    }
}

void Room::hear (const double* output, int n) noexcept
{
    for (int i = 0; i < n; ++i)
    {
        air[(size_t) airWrite] = output[i];
        airWrite = (airWrite + 1) % (int) air.size();
    }
}

void Room::setNote (double newPeriod) noexcept
{
    period = newPeriod;

    // The low-pass delays the note a little (its phase): read that much sooner, or the string would ring flat.
    // A finger resting at 1 / harmonic of the string stops every mode that moves there: only that harmonic and its
    // multiples ring (a natural harmonic), so the string is one period of THAT harmonic long.
    const double ring = period / harmonic;
    const double w = 2 * juce::MathConstants<double>::pi / ring;
    const double lag = std::atan2 ((1 - tone) * std::sin (w), 1 - (1 - tone) * std::cos (w)) / w;
    readPeriod = std::max (2.0, ring - lag);

    // Per period: -60 dB after sustain_s (muted: after 50 ms).
    loopGain = std::pow (10.0, -3.0 * ring / (rate * sustainS));
    spotStep = 2 * juce::MathConstants<double>::pi / ring;  // the chosen harmonic, in radians per sample
    mutedGain = std::pow (10.0, -3.0 * ring / (rate * 0.05));
}

juce::String Room::describe() const
{
    return "the sound reaches the guitar after " + juce::String (1000.0 * delay / rate, 2) + " ms (" + juce::String (distance, 2) + " m)";
}

} // namespace amp
