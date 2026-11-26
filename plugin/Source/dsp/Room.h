#pragma once

#include "Blocks.h"
#include "Player.h"

#include <complex>

namespace amp
{

// The room: the amp's sound goes back to the guitar (acoustic feedback, as in front of a loud amp). It is the last
// block. The sound travels distance / 343 m/s, gets weaker with distance, and shakes the string, which answers at the
// note being played and its harmonics (a resonator tuned by a pitch tracker on the clean input). That motion is added
// to the guitar's signal, so it goes round the amp again. Its process() passes the sound on unchanged: the Engine runs
// the loop around the whole amp, in pieces shorter than the sound's travel time.
//
// Two parts are not physics, they stand in for the guitarist: the pitch tracker follows ONE note (with a chord, the
// strongest), and the string counts as MUTED when the input falls fast (a hand on the strings) or is very quiet.
class Room final : public Block
{
public:
    Room (const Cfg& cfg, const BuildContext& ctx);

    void prepare (int maxSamples) override;
    void process (double*, int) noexcept override {}
    void reset() noexcept override;
    juce::String describe() const override;

    int maxChunk() const noexcept { return chunk; }
    void addFeedback (double* input, int n) noexcept;  // input += the string's motion, driven by the sound of `delay` ago
    void hear (const double* output, int n) noexcept;  // what left the speaker just now
    double noteHz() const noexcept { return period > 0 ? rate / period : 0.0; }
    bool isMuted() const noexcept { return muted; }

    bool enabled = true;  // off while the engine measures its level

private:
    void setNote (double period) noexcept;  // a note from the Player: the string's length and its losses

    double rate = 48000, coupling = 0, sustainS = 3, tone = 0.1, muteLevel = 0.002, distance = 0.5, harmonic = 1;
    int delay = 1, chunk = 1;

    std::vector<double> air;     // the speaker's sound on its way to the guitar
    int airWrite = 0;
    std::vector<double> string;  // the string's motion driven by that sound (a resonator one period long)
    int stringWrite = 0;
    double period = 0, readPeriod = 0, loopGain = 0, mutedGain = 0, smooth = 0;

    // find_spot: how much later than distance_m the sound arrives (samples), and the two phases it compares.
    bool findSpot = false;
    double extra = 0, spotRate = 0, spotAverage = 0, spotStep = 0;
    std::complex<double> pushSum, ringSum, phasor { 1.0, 0.0 };

    Player player;
    bool muted = true;
};

} // namespace amp
