#include "Restring.h"

#include <complex>

namespace amp
{

namespace
{
const double pi = juce::MathConstants<double>::pi;

// Phase delay (samples) at w radians per sample of a first-order all-pass (a + z^-1) / (1 + a z^-1).
double allpassDelay (double w, double a) { return 1 - 2 / w * std::atan2 (a * std::sin (w), 1 + a * std::cos (w)); }
}

double lowpassDelay (double w, double p) { return std::atan2 (p * std::sin (w), 1 - p * std::cos (w)) / w; }
double lowpassLevel (double w, double p) { return (1 - p) / std::abs (1.0 - p * std::polar (1.0, -w)); }

double tonePole (double period, double rate, double sustainS, double toneHz)
{
    // Per round trip the note keeps m, harmonics at toneHz m^10: |lp(wt)| / |lp(w)| = m^9; squared, a quadratic in the
    // pole. (1 - cos written as 2 sin^2 keeps the digits at high sample rates.)
    const double w = 2 * pi / period, wt = 2 * pi * toneHz / rate;

    if (wt <= 1.0001 * w)
        return 0.0;

    const double perRound = -3 * std::log (10.0) * period / (rate * sustainS);  // ln m
    const double c1 = 2 * std::pow (std::sin (w / 2), 2), ct = 2 * std::pow (std::sin (wt / 2), 2);
    const double oneMinusR = std::max (-std::expm1 (18 * perRound), 0.0);
    const double r = std::max (1 - oneMinusR, 1.0001 * c1 / ct);  // a 1-pole can not cut more than this
    const double q = 1 - r, b = q - c1 + r * ct;
    const double pole = juce::jlimit (0.0, 1 - 1e-9, q / (b + std::sqrt (std::max (0.0, b * b - q * q))));

    // A string can not gain energy: the loop gain (m / |lp(w)| at 0 Hz) must stay at most 1, so the low-pass may cut the
    // note itself no further than m. With string_tone_hz under about 3 x the note the two ask for more than that; then
    // sustain_s wins and the highs fade less than asked. |lp(w)| = m: a quadratic in the pole too.
    const double a = -std::expm1 (2 * perRound), bm = a + std::exp (2 * perRound) * c1;  // 1 - m^2, 1 - m^2 cos w
    return std::min (pole, a / (bm + std::sqrt (std::max (0.0, bm * bm - a * a))));
}

Restring::Restring (const Cfg& cfg, const BuildContext& ctx)
    : Block (cfg, { "transpose", "stretch_cents", "sustain_s", "string_tone_hz", "bend_semitones", "bend_s", "mix", "mute_db" })
{
    rate = ctx.rate;
    const double transpose = cfg.number ("transpose", 0.0);
    stretchCents = cfg.number ("stretch_cents", 0.0);
    sustainS = cfg.number ("sustain_s", 4.0);
    toneHz = cfg.number ("string_tone_hz", 3000.0);
    bendSemis = cfg.number ("bend_semitones", 0.0);
    bendS = cfg.number ("bend_s", 0.2);
    mix = cfg.number ("mix", 1.0);

    if (std::abs (transpose) > 24 || std::abs (bendSemis) > 12)
        throw PresetError ("restring: transpose must be from -24 to 24 and bend_semitones from -12 to 12");

    if (stretchCents < 0 || stretchCents > 1200)
        throw PresetError ("restring: stretch_cents must be from 0 to 1200, got " + juce::String (stretchCents));

    if (sustainS <= 0 || bendS <= 0 || toneHz <= 0 || toneHz >= ctx.checkRate / 2)
        throw PresetError ("restring: sustain_s and bend_s must be above 0 and string_tone_hz between 0 and half the sample rate");

    if (mix < 0 || mix > 1)
        throw PresetError ("restring: mix must be from 0 to 1, got " + juce::String (mix));

    // The 10th harmonic of a stiff string is sqrt((1 + 100 B) / (1 + B)) times 10 x the note: B from those cents.
    const double r2 = std::pow (2.0, 2 * stretchCents / 1200);
    B = (r2 - 1) / (100 - r2);
    ratio = std::pow (2.0, -transpose / 12);  // the virtual string's period / the played note's
    // A short, recent window, looked at every 2.5 ms: it finds a new note sooner than the room's tracker.
    player.prepare (rate, dbToGain (cfg.number ("mute_db", -60.0)), 192, 2.5, true);
    every = std::max (1, juce::roundToInt (rate / 1000));                   // retuned every 1 ms
    follow = 1 - std::exp (-every / (0.01 * rate));                         // follows the note within about 10 ms

    // The longest note the Player finds is 60 Hz (a little lower between two lags).
    const double longest = rate / 57.0;
    history.assign ((size_t) (2 * longest) + 8, 0.0);

    for (auto& v : voices)
        v.line.assign ((size_t) (longest * ratio * std::pow (2.0, -std::min (0.0, bendSemis) / 12)) + 8, 0.0);
}

void Restring::reset() noexcept
{
    player.reset();
    std::fill (history.begin(), history.end(), 0.0);
    historyWrite = 0;

    for (auto& v : voices)
    {
        std::fill (v.line.begin(), v.line.end(), 0.0);
        v.active = false;
    }

    current = 0;
    notePeriod = followed = lastReport = 0.0;
    pending = false;
    untilUpdate = 0;
}

double Restring::fitStiffness (double period, double B, double p, double rate, int& count)
{
    const double w1 = 2 * pi / period;
    count = B > 0 ? juce::jlimit (0, sections, (int) std::floor (period - lowpassDelay (w1, p) - 2)) : 0;

    // Harmonic n of a stiff string whose note (harmonic 1) is rate / period: the first 20 up to 2.5 kHz.
    const double f1 = rate / period, f0 = f1 / std::sqrt (1 + B);
    std::array<double, 20> w {}, want {};
    int harmonics = 0;

    for (int n = 1; n <= 20; ++n)
    {
        const double f = n * f0 * std::sqrt (1 + B * n * n);

        if ((n > 2 && f > 2500) || f >= rate / 2)
            break;

        w[(size_t) harmonics] = 2 * pi * f / rate;
        want[(size_t) harmonics] = 2 * pi * n / w[(size_t) harmonics];  // the loop's delay that puts a mode there
        ++harmonics;
    }

    if (count == 0 || harmonics < 2)
    {
        count = 0;
        return 0.0;
    }

    // The delay line takes what is left at the note; the coefficient makes the others land as close as it can
    // (least squares of their relative errors).
    const auto error = [&] (double a)
    {
        const double d = want[0] - count * allpassDelay (w[0], a) - lowpassDelay (w[0], p);

        if (d < 2)
            return 1e30;

        double sum = 0.0;

        for (int i = 1; i < harmonics; ++i)
        {
            const double e = (d + count * allpassDelay (w[(size_t) i], a) + lowpassDelay (w[(size_t) i], p) - want[(size_t) i]) / want[(size_t) i];
            sum += e * e;
        }

        return sum;
    };

    // A coarse look, then a golden-section search around the best point.
    const int points = 41;
    int best = 0;
    double bestError = 1e300;

    for (int i = 0; i < points; ++i)
        if (const double e = error (-0.9999 + 0.9999 * i / (points - 1)); e < bestError)
        {
            bestError = e;
            best = i;
        }

    double lo = -0.9999 + 0.9999 * std::max (0, best - 1) / (points - 1), hi = -0.9999 + 0.9999 * std::min (points - 1, best + 1) / (points - 1);
    const double golden = (std::sqrt (5.0) - 1) / 2;

    for (int i = 0; i < 30; ++i)
    {
        const double c = hi - golden * (hi - lo), d = lo + golden * (hi - lo);

        if (error (c) < error (d))
            hi = d;
        else
            lo = c;
    }

    return 0.5 * (lo + hi);
}

double Restring::wanted (const Voice& v) const noexcept
{
    const double bend = bendSemis != 0 ? bendSemis * std::exp (-v.age / bendS) : 0.0;
    return followed * ratio * (bend != 0 ? std::pow (2.0, -bend / 12) : 1.0);
}

void Restring::update (Voice& v) noexcept
{
    const double period = v.period, w = 2 * pi / period;

    // The low-pass: harmonics at string_tone_hz fall 10 times as fast as the note (per second, as on a real string).
    const double p = tonePole (period, rate, sustainS, toneHz);
    const double perRound = -3 * std::log (10.0) * period / (rate * sustainS);  // ln m
    v.pole = p;

    if (B > 0 && ! v.stopped)
        v.a = fitStiffness (period, B, p, rate, v.count);

    // Stopped by a hand: it dies in 50 ms (60 dB).
    const double keep = std::exp (v.stopped ? -3 * std::log (10.0) * period / (rate * 0.05) : perRound);
    v.gain = std::min (1.0, keep / lowpassLevel (w, p));  // never more than 1 at 0 Hz
    v.read = juce::jlimit (2.0, (double) v.line.size() - 3, period - v.count * allpassDelay (w, v.a) - lowpassDelay (w, p));
}

void Restring::tick (Voice& v) noexcept
{
    const double out = loop (v, readLine (v, v.read));
    v.line[(size_t) v.write] = out;
    v.write = (v.write + 1) % (int) v.line.size();
    v.recent[(size_t) (v.ticks & 7)] = out;
    ++v.ticks;
}

double Restring::readLine (const Voice& v, double back) const noexcept
{
    const int size = (int) v.line.size();
    double from = v.write - back;
    from += from < 0 ? size : 0;
    const auto k = (int) from;
    const double frac = from - k;
    return v.line[(size_t) k] * (1 - frac) + v.line[(size_t) ((k + 1) % size)] * frac;
}

double Restring::loop (Voice& v, double x) noexcept
{
    for (int k = 0; k < v.count; ++k)
    {
        const double y = v.a * (x - v.y1[(size_t) k]) + v.x1[(size_t) k];
        v.x1[(size_t) k] = x;
        v.y1[(size_t) k] = y;
        x = y;
    }

    v.smooth += (1 - v.pole) * (x - v.smooth);
    return v.gain * v.smooth;
}

void Restring::pluck (double inputPeriod) noexcept
{
    auto& old = voices[(size_t) current];

    if (old.active && ! old.stopped)
    {
        old.stopped = true;  // the pick stops the old string
        old.rampLeft = 0;
        update (old);
    }

    current = 1 - current;
    auto& v = voices[(size_t) current];
    notePeriod = followed = inputPeriod;
    v.active = true;
    v.stopped = false;
    v.silentFor = 0;
    v.age = 0.0;
    v.smooth = 0.0;
    v.x1.fill (0.0);
    v.y1.fill (0.0);
    v.recent.fill (0.0);
    v.pos = 0.0;
    v.ticks = 0;
    v.rampLeft = 0;
    v.period = juce::jlimit (2.0, (double) v.line.size() - 4, followed * ratio);  // the note without the pluck bend
    v.speed = juce::jlimit (0.25, 4.0, v.period / wanted (v));                   // the bend: a tighter string at first
    update (v);

    // The shape: the input's last whole period, ending where it last went up through zero (so the string starts
    // without a jump). `ago` = samples before now.
    const int hsize = (int) history.size();
    const auto input = [this, hsize] (double ago)
    {
        double at = historyWrite - ago;
        at += at < 0 ? hsize : 0;
        const auto k = (int) at;
        const double frac = at - k;
        return history[(size_t) (k % hsize)] * (1 - frac) + history[(size_t) ((k + 1) % hsize)] * frac;
    };
    const int span = (int) std::ceil (inputPeriod);
    double end = 1;

    for (int j = 1; j <= span; ++j)
        if (input (j + 1) < 0 && input (j) >= 0)
        {
            end = j;
            break;
        }

    // The string's line, one period of that shape over and over (stretched from inputPeriod to the string's length).
    const double length = v.period;
    const auto shape = [&] (double back)
    {
        double t = (length - std::fmod (back, length)) / length * inputPeriod;  // back = length: the shape's start
        t -= t >= inputPeriod ? inputPeriod : 0;
        return input (end + inputPeriod - t);
    };
    const int size = (int) v.line.size();

    for (int k = 1; k < size; ++k)
        v.line[(size_t) ((v.write - k + size) % size)] = shape (k);

    // The filters as if the string had rung like this for 25 ms (no start-up click from their empty memory).
    for (int j = (int) (0.025 * rate); j >= 1; --j)
        loop (v, shape (v.read + j));
}

void Restring::process (double* x, int n) noexcept
{
    const int hsize = (int) history.size();

    for (int i = 0; i < n; ++i)
    {
        const double in = x[i];
        history[(size_t) historyWrite] = in;
        historyWrite = (historyWrite + 1) % hsize;

        // The player (not physics): a pick attack, then the same note twice in a row = a new pluck; otherwise follow.
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
                pluck (p);
            }
            else
                lastReport = p;
        }

        auto& cur = voices[(size_t) current];

        if (cur.active && ! cur.stopped && player.isStopped())
        {
            cur.stopped = true;  // a hand on the strings
            cur.rampLeft = 0;
            update (cur);
        }

        if (--untilUpdate <= 0)
        {
            untilUpdate = every;
            followed += follow * (notePeriod - followed);

            if (cur.active && ! cur.stopped)
            {
                // The note moved (vibrato, a bend, a hammer-on) or the pluck bend glides: the tension, not the length.
                cur.speedStep = (juce::jlimit (0.25, 4.0, cur.period / wanted (cur)) - cur.speed) / every;
                cur.rampLeft = every;
            }
        }

        double sum = 0.0;

        for (auto& v : voices)
        {
            if (! v.active)
                continue;

            if (v.rampLeft > 0)
            {
                v.speed += v.speedStep;
                --v.rampLeft;
            }

            // Its own steps up to where the output is, then the output between them (Catmull-Rom).
            v.pos += v.speed;

            while ((double) v.ticks < v.pos + 3)
                tick (v);

            const auto k = (long long) v.pos;
            const double t = v.pos - (double) k;
            const double p0 = v.recent[(size_t) ((k - 1) & 7)], p1 = v.recent[(size_t) (k & 7)];
            const double p2 = v.recent[(size_t) ((k + 1) & 7)], p3 = v.recent[(size_t) ((k + 2) & 7)];
            const double out = p1 + 0.5 * t * (p2 - p0 + t * (2 * p0 - 5 * p1 + 4 * p2 - p3 + t * (3 * (p1 - p2) + p3 - p0)));
            v.age += 1 / rate;
            sum += out;

            if (v.stopped && ++v.silentFor > (int) (0.3 * rate))  // 50 ms per 60 dB: 360 dB down
            {
                v.active = false;
                std::fill (v.line.begin(), v.line.end(), 0.0);
            }
        }

        x[i] = (1 - mix) * in + mix * sum;
    }
}

juce::String Restring::describe() const
{
    juce::String s = "restring: " + juce::String (ratio == 1 ? "same notes" : "transposed (period x " + juce::String (ratio, 4) + ")");

    if (B > 0)
        s << ", stiff: B = " << juce::String (B, 6);

    return s;
}

} // namespace amp
