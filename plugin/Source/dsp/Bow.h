#pragma once

#include "Blocks.h"
#include "Player.h"

namespace amp
{

// Bow: your playing bows a virtual string. Your level is the bow's speed and force (both scale with it, in m/s and N
// at a DI peak of 0.5 V), rising in swell_s and falling in release_s, so a plucked note becomes a bowed one that goes
// on as long as you keep it going. Your note sets the string's length (it follows slides and new notes), a hand on the
// strings lifts the bow and stops the string.
//
// The physics (McIntyre, Schumacher and Woodhouse 1983): the string is two delay lines that meet at the bow, one to the
// nut and back, one to the bridge and back (bow_position = the bow's distance from the bridge, as a share of the
// string). The bridge takes the losses: bridge_loss_db on every round trip (it goes into the body), whatever the note,
// so low notes ring longer; its highs through the per-note 1-pole of restring (string_tone_hz: 10 times the loss).
// Measured: that loss PER TRIP decides whether the motion stays clean, as Raman's extra waves die by it (a loss set per
// second gave a 55 Hz string 3-5 slips per period). At the bow, every sample, the
// friction force is solved exactly: the string STICKS to the bow (moves with it) while that takes less than
// friction_static x the bow force, else it SLIPS, the friction falling with the slip speed:
//   mu = friction_dynamic + (friction_static - friction_dynamic) v0 / (v0 + |slip|), v0 = 0.1 m/s.
// With the string's wave impedance Z (0.3 kg/s, a middle guitar string) that is a quadratic. Where it has two answers
// the bow keeps doing what it did (stuck stays stuck, slipping keeps slipping), as a real bow does (hysteresis).
// Out: the string's velocity at the bridge, scaled so full playing gives about 0.5 V.
class Bow final : public Block
{
public:
    Bow (const Cfg& cfg, const BuildContext& ctx);

    void process (double* x, int n) noexcept override;
    void reset() noexcept override;
    juce::String describe() const override;

    static constexpr double impedance = 0.3, v0 = 0.1;  // kg/s, m/s

    // One step of the friction at the bow: hv = the bow's speed minus the string's speed the waves alone would give.
    // Returns the change of the string's speed the friction makes (force / 2Z); `stuck` keeps the state.
    static double friction (double hv, double forceN, double muStatic, double muDynamic, bool& stuck) noexcept;

    // For the checks: samples bowed, of them stuck, and how often it started to slip.
    long long samples = 0, stuckSamples = 0, slips = 0;

private:
    double readLine (const std::vector<double>& line, double back) const noexcept;
    void update() noexcept;

    double rate = 48000, ratio = 1, beta = 0.12, forceN = 1.5, speed = 0.2, muS = 0.8, muD = 0.3;
    double lossDb = 0.55, toneHz = 2000, mix = 1, outGain = 1;
    double swell = 0, fall = 0, level = 0, bow = 0;
    Player player;
    std::vector<double> toBridge, toNut;  // waves leaving the bow towards the bridge and the nut
    int write = 0;
    double readBridge = 0, readNut = 0, stepBridge = 0, stepNut = 0;
    int rampLeft = 0, every = 48, untilUpdate = 0;
    double pole = 0, gain = 0, smooth = 0;
    double notePeriod = 0, followed = 0, follow = 0, lastReport = 0;
    bool pending = false, playing = false, stopped = false, stuck = true;
};

} // namespace amp
