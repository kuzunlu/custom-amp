#pragma once

#include "Blocks.h"

#include <array>

namespace amp
{

// Mouth: the sound goes through a vocal tract, as with a talk box (the amp's sound is sent up a tube into the mouth,
// which shapes it into vowels). Put it after the amp. `vowel` moves the tract from "oo" (0) through "oh", "ah", "eh"
// to "ee" (4); `follow` moves it with your playing (louder = further), like a mouth opening on each note.
//
// The physics: a Kelly-Lochbaum tube (Kelly and Lochbaum 1962). The tract is about 48 short sections, each as long as
// sound travels in a few samples; where the area changes, part of each pressure wave goes on and part bounces back,
// r = (A1 - A2) / (A1 + A2). The glottis end reflects 0.95 of the wave (nearly closed), the lips -0.9 (open; what
// passes is the sound, differentiated: a mouth radiates the flow's change), every section loses 0.02% (the walls).
// A plain tube 17.15 cm long then has its resonances (formants) at 502, 1500, 2500 Hz (theory 500, 1500, 2500).
// The five vowel shapes: ln A(x) = sum c_n cos(n pi x / L), n = 1..6, FITTED (not measured shapes) so that the
// tube's first three formants land on Peterson and Barney's (1952) averages for men, within 0.5%.
//
// The chime (measured 2026-11-27; the user's favourite sound): above about 3 kHz the shape hardly matters, so the tube
// rings like a plain pipe, every c / 2L (1 kHz at 17 cm) up to 20 kHz, each resonance about 55 Hz wide (40 ms of
// ringing), louder by 6 dB per octave (the radiation is differentiated): +25 dB at 19.5 kHz against the vowel. A real
// mouth damps them (its lips radiate more and reflect less at high frequencies). lip_reflection (default 0.9) and
// lip_radiation_hz (default off) set that; the defaults are the sound as it was, sample for sample. tune sets the
// length from the spacing of those resonances (the chime's pitch).
class Mouth final : public Block
{
public:
    using Shape = std::array<double, 6>;

    // `shape` set: use it instead of the vowels (for the checks: zeros = a plain tube).
    Mouth (const Cfg& cfg, const BuildContext& ctx, const Shape* shape = nullptr);

    void process (double* x, int n) noexcept override;
    void reset() noexcept override;
    juce::String describe() const override;

    double vowelNow() const noexcept { return now; }  // for the checks
    static constexpr double glottis = 0.95, wallLoss = 0.9998, soundSpeed = 343.0;

private:
    void setShape (double vowel) noexcept;  // the reflection coefficients for a vowel position (0..4)

    double rate = 48000, vowel = 2, follow = 0, mix = 1, length = 0.1715, gain = 1;
    double lips = -0.9;                                 // the lips' reflection (-lip_reflection)
    double radiation = 0, lipState = 0;                 // lip_radiation_hz: a 1-pole low-pass on what the lips send back
    bool radiates = false;
    std::array<double, 5> vowelGain { 1, 1, 1, 1, 1 };  // each vowel's level (see the constructor)
    int sections = 48, delay = 1, at = 0;
    bool fixedShape = false;
    Shape custom {};
    std::vector<double> reflect, fwd, bwd, outF, outB;  // fwd/bwd: each section's delay line (delay samples each way)
    double lastLip = 0, now = 2;
    double level = 0, loudest = 0, rise = 0, fall = 0, forget = 0;  // the playing level, against the loudest lately
    int untilShape = 0, shapeEvery = 48;
};

} // namespace amp
