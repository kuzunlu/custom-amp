#include "Player.h"

namespace amp
{

void Player::prepare (double rateIn, double muteLevelIn, int windowIn, double checkMs, bool recentIn)
{
    rate = rateIn;
    muteLevel = muteLevelIn;
    window = windowIn;
    recent = recentIn;
    baseRate = 1 - std::exp (-1 / (0.02 * rate));
    fastFall = std::exp (-1 / (0.005 * rate));
    slowFall = std::exp (-1 / (0.1 * rate));
    decimate = std::max (1, juce::roundToInt (rate / 12000.0));
    checkEvery = std::max (1, juce::roundToInt (checkMs / 1000 * rate / decimate));
    low.assign (1024, 0.0);
    diff.assign (1024, 0.0);
    flat.assign (1024, 0.0);
    reset();
}

void Player::reset() noexcept
{
    std::fill (low.begin(), low.end(), 0.0);
    lowWrite = decimCount = sinceCheck = 0;
    decimSum = fastLevel = slowLevel = notePeriod = base = 0.0;
    muted = armed = true;
    attack = false;
}

bool Player::hear (double x) noexcept
{
    const double level = std::abs (x);
    fastLevel = std::max (level, fastLevel * fastFall);
    slowLevel = std::max (level, slowLevel * slowFall);
    muted = slowLevel < muteLevel || fastLevel < 0.1 * slowLevel;

    // A pick attack: the level doubles against where it was (base follows it about 20 ms late). Armed again once the
    // level is back near base, so one attack counts once.
    base += baseRate * (slowLevel - base);
    attack = armed && slowLevel > 2 * base + muteLevel;

    if (attack)
        armed = false;
    else if (slowLevel < 1.25 * base)
        armed = true;

    decimSum += x;

    if (++decimCount < decimate)
        return false;

    low[(size_t) lowWrite] = decimSum / decimate;
    lowWrite = (lowWrite + 1) % (int) low.size();
    decimSum = 0.0;
    decimCount = 0;

    if (++sinceCheck < checkEvery)
        return false;

    sinceCheck = 0;
    return track();
}

bool Player::track() noexcept
{
    const int lowSize = (int) low.size();
    const double lowRate = rate / decimate;
    const int maxLag = std::min ((int) (lowRate / 60.0), lowSize - window - 1), minLag = std::max (2, (int) (lowRate / 1000.0));
    const int start = lowWrite - window - maxLag, used = window + maxLag + 1;

    // The ring in order (oldest first), so the loops below need no wrapping.
    for (int j = 0; j < used; ++j)
        flat[(size_t) j] = low[(size_t) (((start + j) % lowSize + lowSize) % lowSize)];

    const double* const newest = flat.data() + window + maxLag - 1;
    const auto at = [&] (int j) { return flat[(size_t) j]; };
    const auto back = [&] (int j) { return newest[-j]; };  // j = 0: the newest

    double energy = 0.0;

    for (int j = 0; j < window; ++j)
        energy += recent ? back (j) * back (j) : at (j) * at (j);

    if (std::sqrt (energy / window) < muteLevel)
        return false;  // nothing playing: keep the last note

    double running = 0.0;
    int found = -1;

    for (int lag = 1; lag <= maxLag; ++lag)
    {
        double d = 0.0;

        for (int j = 0; j < window; ++j)
        {
            const double e = recent ? back (j) - back (j + lag) : at (j) - at (j + lag);
            d += e * e;
        }

        running += d;
        diff[(size_t) lag] = running > 0 ? d * lag / running : 1.0;
    }

    for (int lag = minLag; lag < maxLag && found < 0; ++lag)
        if (diff[(size_t) lag] < 0.15)
        {
            found = lag;

            while (found + 1 < maxLag && diff[(size_t) found + 1] < diff[(size_t) found])
                ++found;
        }

    if (found < 0)
        return false;

    // Between two lags: the bottom of a parabola through the three around it.
    const double a = diff[(size_t) found - 1], b = diff[(size_t) found], c = diff[(size_t) found + 1];
    const double shift = a - 2 * b + c != 0 ? 0.5 * (a - c) / (a - 2 * b + c) : 0.0;
    notePeriod = (found + juce::jlimit (-0.5, 0.5, shift)) * decimate;
    return true;
}

} // namespace amp
