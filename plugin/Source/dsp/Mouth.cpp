#include "Mouth.h"

#include "Strings.h"

namespace amp
{

namespace
{
// ln A(x) = sum c_n cos(n pi x / L) for "oo", "oh", "ah", "eh", "ee": fitted to Peterson and Barney's (1952) formants
// for men (F1, F2, F3: who'd 300 870 2240, hawed 570 840 2410, father 730 1090 2440, head 530 1840 2480, heed 270 2290
// 3010 Hz) with this tube at 96 kHz, one sample per section.
const std::array<Mouth::Shape, 5> vowels { {
    { -0.1032, 0.3886, 1.0226, 0.2845, 0.4821, 0.3751 },  // oo (who'd)
    { -0.9482, 0.0281, 0.9480, 0.0282, 0.4100, -0.0178 },  // oh (hawed)
    { -1.1980, -0.0213, 0.6215, -0.0213, 0.2317, -0.0213 },  // ah (father)
    { -0.2327, 0.0000, -0.4306, 0.0012, 0.0684, 0.0196 },  // eh (head)
    { 0.7357, 0.0000, -0.9470, -0.0110, -0.2026, -0.0128 },  // ee (heed)
} };
}

Mouth::Mouth (const Cfg& cfg, const BuildContext& ctx, const Shape* shape)
    : Block (cfg, { "vowel", "follow", "follow_ms", "length_cm", "mix", "lip_reflection", "lip_radiation_hz", "tune" })
{
    rate = ctx.rate;
    vowel = cfg.number ("vowel", 2.0);
    follow = cfg.number ("follow", 0.0);
    const double followMs = cfg.number ("follow_ms", 80.0);
    double cm = cfg.number ("length_cm", 17.15);
    mix = cfg.number ("mix", 1.0);
    const double lipReflection = cfg.number ("lip_reflection", 0.9), radiationHz = cfg.number ("lip_radiation_hz", 0.0);

    if (lipReflection <= 0 || lipReflection >= 1 || radiationHz < 0 || radiationHz >= ctx.checkRate / 2)
        throw PresetError ("mouth: lip_reflection must be between 0 and 1 and lip_radiation_hz from 0 (off) to half the sample rate");

    lips = -lipReflection;
    radiates = radiationHz > 0;
    radiation = 1 - std::exp (-2 * juce::MathConstants<double>::pi * radiationHz / rate);

    // tune: a note (a name like E5, or Hz) = the spacing of the high resonances, c / 2L.
    if (const auto tune = cfg.get ("tune"); ! tune.isVoid() && tune.toString().isNotEmpty())
    {
        if (cfg.has ("length_cm"))
            throw PresetError ("mouth: give tune or length_cm, not both");

        const auto text = tune.toString().trim();
        double hz = 0;

        try
        {
            hz = SympatheticStrings::noteHz (text.containsOnly ("0123456789.") ? juce::var (text.getDoubleValue()) : tune);
        }
        catch (const PresetError&)
        {
            throw PresetError ("mouth: tune '" + text + "' is not a note. Use a name like E5, F#5 or Bb5, or a number of Hz");
        }

        cm = 100 * soundSpeed / (2 * hz);

        if (cm < 8 || cm > 30)
            throw PresetError ("mouth: tune must be from 572 Hz (D5, a 30 cm tract) to 2144 Hz (C7, 8 cm), got " + juce::String (hz, 1) + " Hz");
    }

    if (vowel < 0 || vowel > 4 || std::abs (follow) > 4)
        throw PresetError ("mouth: vowel must be from 0 (oo) to 4 (ee) and follow from -4 to 4");

    if (followMs <= 0 || cm < 8 || cm > 30 || mix < 0 || mix > 1)
        throw PresetError ("mouth: follow_ms must be above 0, length_cm from 8 to 30 and mix from 0 to 1");

    // Each section as long as sound travels in `delay` samples: about 0.36 cm at any rate (one sample at 96 kHz).
    delay = std::max (1, juce::roundToInt (rate / 96000.0));
    sections = std::max (4, juce::roundToInt (cm / 100 * rate / (soundSpeed * delay)));
    length = sections * delay * soundSpeed / rate;
    fixedShape = shape != nullptr;
    custom = shape != nullptr ? *shape : Shape {};
    reflect.assign ((size_t) sections - 1, 0.0);
    fwd.assign ((size_t) (sections * delay), 0.0);
    bwd.assign ((size_t) (sections * delay), 0.0);
    outF.assign ((size_t) sections, 0.0);
    outB.assign ((size_t) sections, 0.0);
    rise = 1 - std::exp (-1 / (0.005 * rate));
    fall = std::exp (-1 / (followMs / 1000 * rate));
    forget = std::exp (-1 / (3.0 * rate));  // "the loudest lately": falls back over about 3 s
    shapeEvery = std::max (1, juce::roundToInt (rate / 1000));
    setShape (vowel);

    // Level: each vowel set so a guitar-like tone (a 110 Hz sawtooth, its harmonics falling 6 dB per octave) comes out
    // as loud as it went in, so moving the vowel moves the tone, not the volume. NOT physics: with these simple lips
    // (the same reflection at any opening) "oo" came out 12 dB louder than "ah", the wrong way round (a wide open mouth
    // radiates more: in speech "ah" is the loudest vowel).
    {
        const int n = (int) (0.2 * rate);
        std::vector<double> x ((size_t) n), y;

        for (int i = 0; i < n; ++i)
            x[(size_t) i] = 2 * std::fmod (110.0 * i / rate, 1.0) - 1;

        const double savedMix = mix, savedFollow = follow;
        mix = 1;
        follow = 0;

        for (int v = 0; v < (fixedShape ? 1 : 5); ++v)
        {
            reset();
            setShape ((double) v);  // every vowel's gain is still 1 here
            y = x;
            process (y.data(), n);
            double in = 0, out = 0;

            for (int i = n / 2; i < n; ++i)
            {
                in += x[(size_t) i] * x[(size_t) i];
                out += y[(size_t) i] * y[(size_t) i];
            }

            vowelGain[(size_t) v] = out > 0 ? std::sqrt (in / out) : 1.0;
        }

        if (fixedShape)
            vowelGain.fill (vowelGain[0]);

        mix = savedMix;
        follow = savedFollow;
        reset();
    }
}

void Mouth::reset() noexcept
{
    std::fill (fwd.begin(), fwd.end(), 0.0);
    std::fill (bwd.begin(), bwd.end(), 0.0);
    at = untilShape = 0;
    lastLip = level = loudest = lipState = 0.0;
    setShape (vowel);
}

void Mouth::setShape (double position) noexcept
{
    now = position;
    Shape c = custom;

    // Between two neighbouring vowels: their shapes mixed, and their levels (in dB).
    const auto k = std::min (3, (int) position);
    const double t = position - k;
    gain = std::exp ((1 - t) * std::log (vowelGain[(size_t) k]) + t * std::log (vowelGain[(size_t) k + 1]));

    if (! fixedShape)
        for (size_t n = 0; n < c.size(); ++n)
            c[n] = (1 - t) * vowels[(size_t) k][n] + t * vowels[(size_t) k + 1][n];

    const auto lnArea = [&c, this] (int i)
    {
        const double x = (i + 0.5) / sections;
        double sum = 0;

        for (size_t n = 0; n < c.size(); ++n)
            sum += c[n] * std::cos ((double) (n + 1) * juce::MathConstants<double>::pi * x);

        return sum;
    };

    double here = std::exp (lnArea (0));

    for (int j = 0; j + 1 < sections; ++j)
    {
        const double next = std::exp (lnArea (j + 1));
        reflect[(size_t) j] = (here - next) / (here + next);
        here = next;
    }
}

void Mouth::process (double* x, int n) noexcept
{
    const int last = sections - 1;

    for (int i = 0; i < n; ++i)
    {
        const double in = x[i];

        // follow: the vowel moves with the playing level against the loudest lately (0..1).
        if (follow != 0)
        {
            const double a = std::abs (in);
            level = a > level ? level + rise * (a - level) : level * fall;
            loudest = std::max (level, loudest * forget);

            if (--untilShape <= 0)
            {
                untilShape = shapeEvery;
                setShape (juce::jlimit (0.0, 4.0, vowel + follow * (loudest > 0 ? level / loudest : 0.0)));
            }
        }

        // The waves arriving at each section's end (forward) and start (backward), then the new ones leaving.
        for (int s = 0; s < sections; ++s)
        {
            outF[(size_t) s] = fwd[(size_t) (s * delay + at)];
            outB[(size_t) s] = bwd[(size_t) (s * delay + at)];
        }

        fwd[(size_t) at] = wallLoss * (glottis * outB[0] + in);

        for (int j = 0; j < last; ++j)
        {
            const double r = reflect[(size_t) j], f = outF[(size_t) j], b = outB[(size_t) j + 1];
            fwd[(size_t) ((j + 1) * delay + at)] = wallLoss * ((1 + r) * f - r * b);
            bwd[(size_t) (j * delay + at)] = wallLoss * (r * f + (1 - r) * b);
        }

        double lip;

        if (! radiates)
        {
            bwd[(size_t) (last * delay + at)] = wallLoss * lips * outF[(size_t) last];
            lip = (1 + lips) * outF[(size_t) last];
        }
        else
        {
            // The lips send back less of the highs: what comes back is the wave through a 1-pole low-pass (a simple
            // stand-in for Flanagan's radiation load); the rest passes out.
            lipState += radiation * (outF[(size_t) last] - lipState);
            bwd[(size_t) (last * delay + at)] = wallLoss * lips * lipState;
            lip = outF[(size_t) last] + lips * lipState;
        }

        at = (at + 1) % delay;

        // What leaves the lips, differentiated (the radiation).
        const double out = gain * (lip - lastLip);
        lastLip = lip;
        x[i] = (1 - mix) * in + mix * out;
    }
}

juce::String Mouth::describe() const
{
    return "mouth: a " + juce::String (100 * length, 2) + " cm tract in " + juce::String (sections) + " sections, its high resonances "
           + juce::String (soundSpeed / (2 * length), 0) + " Hz apart";
}

} // namespace amp
