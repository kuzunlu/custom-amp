#pragma once

#include "Blocks.h"

namespace amp
{

// Sympathetic strings: strings tuned to fixed notes that nobody plays, next to the ones you play (a sitar's or a
// sarangi's extra strings, a piano with its sustain pedal down, a guitar's own open strings). Your playing shakes them
// (coupling_db); each one only takes up the energy that matches its own note and harmonics, and rings on after you stop.
// Each string is a resonator one period long: what it did one period ago comes round again, a little weaker
// (-60 dB after sustain_s at its note), its highs fading faster (a 1-pole low-pass at string_tone_hz in the loop; the
// read is shortened by that filter's delay so the string stays in tune). Output = the input + mix x all the strings.
// The gain makes up the low-pass's loss at the note, but the low-pass passes 0 Hz untouched: a string whose note it
// cuts by more than the square root of what the note keeps per period gets its own, gentler low-pass, so it can never
// gain energy (a high string with the defaults grew 23 dB per second at 0 Hz before).
class SympatheticStrings final : public Block
{
public:
    SympatheticStrings (const Cfg& cfg, const BuildContext& ctx);

    void process (double* x, int n) noexcept override;
    void reset() noexcept override;
    juce::String describe() const override;

    // A note name ("A2", "F#4", "Bb3") or a number of Hz, as a frequency. Throws PresetError for anything else.
    static double noteHz (const juce::var& note);

private:
    struct String
    {
        double hz = 110, read = 400, gain = 0.99, smooth = 0, tone = 0.1;  // tone: this string's own low-pass
        std::vector<double> line;
        int write = 0;
    };

    std::vector<String> strings;
    double coupling = 0.03, mix = 1, tone = 0.1;
};

} // namespace amp
