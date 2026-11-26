#include "Bow.h"
#include "Restring.h"

namespace amp
{

Bow::Bow (const Cfg& cfg, const BuildContext& ctx)
    : Block (cfg, { "transpose", "bow_position", "bow_force_n", "bow_speed", "friction_static", "friction_dynamic", "swell_s",
                    "release_s", "bridge_loss_db", "string_tone_hz", "mix", "mute_db" })
{
    rate = ctx.rate;
    const double transpose = cfg.number ("transpose", 0.0);
    beta = cfg.number ("bow_position", 0.12);
    forceN = cfg.number ("bow_force_n", 1.5);
    speed = cfg.number ("bow_speed", 0.2);
    muS = cfg.number ("friction_static", 0.8);
    muD = cfg.number ("friction_dynamic", 0.3);
    const double swellS = cfg.number ("swell_s", 0.05), releaseS = cfg.number ("release_s", 2.0);
    lossDb = cfg.number ("bridge_loss_db", 0.55);
    toneHz = cfg.number ("string_tone_hz", 2000.0);
    mix = cfg.number ("mix", 1.0);

    if (std::abs (transpose) > 24)
        throw PresetError ("bow: transpose must be from -24 to 24, got " + juce::String (transpose));

    if (beta < 0.02 || beta > 0.5)
        throw PresetError ("bow: bow_position must be from 0.02 to 0.5, got " + juce::String (beta));

    if (forceN < 0 || speed <= 0 || muD < 0 || muS < muD)
        throw PresetError ("bow: bow_force_n must be 0 or more, bow_speed above 0, and friction_static at least friction_dynamic (0 or more)");

    if (swellS <= 0 || releaseS <= 0 || lossDb <= 0 || toneHz <= 0 || toneHz >= ctx.checkRate / 2)
        throw PresetError ("bow: swell_s, release_s and bridge_loss_db must be above 0 and string_tone_hz between 0 and half the sample rate");

    if (mix < 0 || mix > 1)
        throw PresetError ("bow: mix must be from 0 to 1, got " + juce::String (mix));

    ratio = std::pow (2.0, -transpose / 12);
    player.prepare (rate, dbToGain (cfg.number ("mute_db", -60.0)), 192, 2.5, true);
    every = std::max (1, juce::roundToInt (rate / 1000));
    follow = 1 - std::exp (-every / (0.01 * rate));
    swell = 1 - std::exp (-1 / (swellS * rate));
    fall = std::exp (-1 / (releaseS * rate));

    // Helmholtz motion: the string's velocity at the bridge jumps by about bow speed / bow position once a period.
    outGain = 0.5 * beta / speed;

    const double longest = rate / 57.0 * ratio;  // the lowest note the Player finds is 60 Hz
    toBridge.assign ((size_t) longest + 8, 0.0);
    toNut.assign ((size_t) longest + 8, 0.0);
}

void Bow::reset() noexcept
{
    player.reset();
    std::fill (toBridge.begin(), toBridge.end(), 0.0);
    std::fill (toNut.begin(), toNut.end(), 0.0);
    write = rampLeft = untilUpdate = 0;
    readBridge = readNut = stepBridge = stepNut = 0.0;
    pole = gain = smooth = level = bow = 0.0;
    notePeriod = followed = lastReport = 0.0;
    pending = playing = stopped = false;
    stuck = true;
    samples = stuckSamples = slips = 0;
}

double Bow::friction (double hv, double forceN, double muStatic, double muDynamic, bool& stuck) noexcept
{
    // Stuck: the friction gives the string exactly the bow's speed (a change of hv), if it can.
    const double a = forceN / (2 * impedance), limit = a * muStatic, h = std::abs (hv);

    if (stuck && h <= limit)
        return hv;

    // Slipping by x > 0 (same side as hv): x = h - a mu(x), times (v0 + x) a quadratic x^2 + b x + c = 0.
    const double b = v0 - h + a * muDynamic, c = v0 * (limit - h), disc = b * b - 4 * c;

    if (c < 0 || (b < 0 && disc >= 0))
    {
        // One answer above 0 (it can not stick), or two (it was slipping): the bigger one, the slipping branch.
        stuck = false;
        const double x = 0.5 * (-b + std::sqrt (std::max (0.0, disc)));
        return hv > 0 ? h - x : x - h;
    }

    stuck = true;  // no slipping answer: it catches
    return hv;
}

double Bow::readLine (const std::vector<double>& line, double back) const noexcept
{
    const int size = (int) line.size();
    double from = write - back;
    from += from < 0 ? size : 0;
    const auto k = (int) from;
    const double frac = from - k;
    return line[(size_t) k] * (1 - frac) + line[(size_t) ((k + 1) % size)] * frac;
}

void Bow::update() noexcept
{
    followed += follow * (notePeriod - followed);
    const double period = juce::jlimit (4.0, (double) toNut.size() - 4, followed * ratio);
    const double w = 2 * juce::MathConstants<double>::pi / period;
    const double sustainS = 60 * period / (rate * lossDb);  // bridge_loss_db on every round trip: 60 dB in this many s
    pole = tonePole (period, rate, sustainS, toneHz);
    const double keep = std::exp (-3 * std::log (10.0) * period / (rate * (stopped ? 0.05 : sustainS)));  // stopped: 60 dB in 50 ms
    gain = std::min (1.0, keep / lowpassLevel (w, pole));  // never more than 1 at 0 Hz

    // The bridge side carries the low-pass: its read is shortened by the low-pass's delay at the note.
    const double bridge = std::max (1.0, beta * period - lowpassDelay (w, pole)), nut = std::max (1.0, (1 - beta) * period);

    if (readBridge == 0)
    {
        readBridge = bridge;
        readNut = nut;
        stepBridge = stepNut = 0;
    }
    else
    {
        stepBridge = (bridge - readBridge) / every;
        stepNut = (nut - readNut) / every;
    }

    rampLeft = every;
}

void Bow::process (double* x, int n) noexcept
{
    const int size = (int) toNut.size();

    for (int i = 0; i < n; ++i)
    {
        const double in = x[i];

        // The player (not physics): a pick attack, then the same note twice in a row = a new note; otherwise follow.
        const bool found = player.hear (in);

        if (player.attacked())
        {
            pending = true;
            lastReport = 0.0;
        }

        if (found)
        {
            const double p = player.period();

            if (! pending)
                notePeriod = p;
            else if (lastReport > 0 && std::abs (p / lastReport - 1) < 0.03)
            {
                pending = false;
                notePeriod = followed = p;
                stopped = false;
                untilUpdate = 0;

                if (! playing)
                    readBridge = 0;  // the first note: start the reads where they belong

                playing = true;
            }
            else
                lastReport = p;
        }

        if (playing && ! stopped && player.isStopped())
        {
            stopped = true;  // a hand on the strings: the bow lifts, the string is damped
            untilUpdate = 0;
        }

        // The bow follows your level: it rises in swell_s, falls no faster than release_s.
        level = std::max (std::abs (in), level * fall);
        bow = stopped || ! playing ? 0.0 : bow + swell * (level - bow);

        double out = 0.0;

        if (playing)
        {
            if (--untilUpdate <= 0)
            {
                untilUpdate = every;
                update();
            }

            if (rampLeft > 0)
            {
                readBridge += stepBridge;
                readNut += stepNut;
                --rampLeft;
            }

            // The waves coming back to the bow: from the bridge (with the string's losses, upside down) and the nut.
            smooth += (1 - pole) * (readLine (toBridge, readBridge) - smooth);
            const double fromBridge = -gain * smooth, fromNut = -readLine (toNut, readNut);
            const double bowSpeed = speed * bow / 0.5, force = forceN * bow / 0.5;
            const bool wasStuck = stuck;
            const double kick = force > 0 ? friction (bowSpeed - (fromBridge + fromNut), force, muS, muD, stuck) : 0.0;

            if (force > 0)
            {
                ++samples;
                stuckSamples += stuck ? 1 : 0;
                slips += wasStuck && ! stuck ? 1 : 0;
            }

            toBridge[(size_t) write] = fromNut + kick;
            toNut[(size_t) write] = fromBridge + kick;
            write = (write + 1) % size;
            out = gain * smooth * outGain;
        }

        x[i] = (1 - mix) * in + mix * out;
    }
}

juce::String Bow::describe() const
{
    // Schelleng (1973): above this force the bow can not let go when the Helmholtz corner comes (raucous).
    const auto s = "bow: " + juce::String (forceN, 2) + " N at " + juce::String (speed, 2) + " m/s";

    if (muS <= muD)
        return s + " (static = dynamic friction: it never sticks)";

    return s + "; Helmholtz motion needs under about " + juce::String (2 * impedance * speed / (beta * (muS - muD)), 2) + " N (Schelleng)";
}

} // namespace amp
