#pragma once

#include "Blocks.h"

namespace amp
{

// Body: your playing shakes a solid object (a wine glass, a bar, a drum head, a metal plate, a bell), and you hear it
// ring with you. Modal synthesis: the object's motion is a sum of its modes, each a resonator at its own frequency with
// its own decay. The mode frequencies come from each shape's equations (as multiples of the lowest, set by `note`):
//   glass:    a thin ring's bending modes (Rayleigh): n (n^2 - 1) / sqrt (n^2 + 1), n = 2, 3, ... (a wine glass's rim)
//   bar:      a free bar (Euler-Bernoulli): x^2 for the roots of cos x cosh x = 1 (4.730, 7.853, ...): a glockenspiel
//             bar before it is tuned
//   membrane: a round drum head held at its edge: the zeros of the Bessel functions J_m (2.405, 3.832, 5.136, ...)
//   plate:    a rectangular plate with simply supported edges, sides 1 : sqrt 2 (Kirchhoff): m^2 + 2 n^2
//   bell:     NOT derived: the partials a bell founder tunes a church bell to (hum, prime, tierce, quint, nominal ...:
//             1, 2, 2.4, 3, 4, 5, 5.33, 6, 8 x the hum; Rossing, The Science of Percussion Instruments)
// Each mode falls 60 dB in decay_s x (lowest / its frequency)^high_damping (higher modes die sooner: losses in the
// material and to the air grow with frequency). Each mode answers a note right on it like a sympathetic string does
// (coupling / (1 - what it keeps per period)), so a long-ringing mode is louder and pickier. NOT physics: every mode is
// driven and heard equally (as if struck and listened to where all of them move; a real spot weights them by their
// shapes). Output = the input + mix x the object.
class Body final : public Block
{
public:
    Body (const Cfg& cfg, const BuildContext& ctx);

    void process (double* x, int n) noexcept override;
    void reset() noexcept override;
    juce::String describe() const override;

    // The object's mode frequencies as multiples of its lowest, in order, up to `highest` x the lowest.
    static std::vector<double> modeRatios (const juce::String& object, double highest);

private:
    struct Mode
    {
        double hz = 0, b0 = 0, a1 = 0, a2 = 0;  // y = b0 (x - x2) + a1 y1 + a2 y2: a band-pass resonator
        double x1 = 0, x2 = 0, y1 = 0, y2 = 0;
    };

    std::vector<Mode> modes;
    juce::String object;
    double mix = 1;
};

} // namespace amp
