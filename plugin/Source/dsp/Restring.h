#pragma once

#include "Blocks.h"
#include "Player.h"

#include <array>

namespace amp
{

// Restring: you play your guitar, but you hear a virtual string with physics you choose, playing the same notes.
// Each pick attack plucks it with your own DI's last period (stretched to the new string's length), so the pick's
// place and brightness carry over; then it rings on its own: tuned transpose semitones from your note, its harmonics
// stretched sharp like a stiff string (stretch_cents at the 10th), falling 60 dB in sustain_s, its highs faster
// (string_tone_hz), its pitch gliding back from bend_semitones after each pluck (in bend_s). It follows your note
// while it rings (slides, bends, hammer-ons), and a hand on the strings stops it in 50 ms.
//
// The string: a delay line read one period back (between two samples), through 16 first-order all-passes (a stiff
// string's high harmonics come round sooner; fitted over its first 20 harmonics up to 2.5 kHz), a 1-pole low-pass
// set per note so the highs fade in SECONDS like a real string's (harmonics at string_tone_hz 10 times as fast as the
// note, the loss rising with the square of the frequency below that), and a gain for sustain_s. The read is shortened
// by both filters' delay at the note, so the note is in tune whatever they do.
//
// NOT physics, standing in for the player: the Player's pitch tracker, attack and hand-stop detection. A new string is
// plucked when the tracker finds the same note twice in a row after an attack (10 ms apart): that is the latency.
class Restring final : public Block
{
public:
    Restring (const Cfg& cfg, const BuildContext& ctx);

    void process (double* x, int n) noexcept override;
    void reset() noexcept override;
    juce::String describe() const override;

    static constexpr int sections = 16;  // all-passes for the stiffness

    // The all-pass coefficient that makes a string `period` samples long as stiff as B (inharmonicity: harmonic n at
    // n f0 sqrt(1 + B n^2)), with a 1-pole of pole p in the loop; and how many sections fit into the period.
    static double fitStiffness (double period, double B, double p, double rate, int& count);

private:
    struct Voice
    {
        std::vector<double> line;
        int write = 0;
        double period = 0;              // the loop's length at the note (samples)
        double read = 0, readStep = 0;  // the delay (samples), moving to its next value over one update
        int rampLeft = 0;
        double a = 0, designPeriod = 0; // the all-pass coefficient and the period it was fitted for
        int count = 0;                  // all-pass sections in use (fewer for very short periods)
        std::array<double, sections> x1 {}, y1 {};
        double pole = 0, gain = 0, smooth = 0;
        double age = 0;                 // seconds since its pluck
        bool active = false, stopped = false;
        int silentFor = 0;              // samples since it was stopped
    };

    void pluck (double inputPeriod) noexcept;
    void update (Voice& v, double period) noexcept;  // tunes v to a loop of `period` samples
    double wanted (const Voice& v) const noexcept;    // the period it should have now
    double readLine (const Voice& v, double back) const noexcept;
    double loop (Voice& v, double x) noexcept;        // the all-passes and the low-pass

    double rate = 48000, ratio = 1, B = 0, sustainS = 4, toneHz = 3000, bendSemis = 0, bendS = 0.2, mix = 1;
    double stretchCents = 0;
    Player player;
    std::vector<double> history;  // the input, for the pluck's shape
    int historyWrite = 0;
    std::array<Voice, 2> voices;
    int current = 0;
    double notePeriod = 0, followed = 0, follow = 0;  // the tracked note (input samples), smoothed
    double lastReport = 0;
    bool pending = false;
    int every = 48, untilUpdate = 0;
};

} // namespace amp
