#include "Strings.h"

#include <complex>

namespace amp
{

double SympatheticStrings::noteHz (const juce::var& note)
{
    if (note.isDouble() || note.isInt() || note.isInt64())
    {
        if ((double) note < 20 || (double) note > 5000)
            throw PresetError ("strings: a note in Hz must be from 20 to 5000, got " + note.toString());

        return (double) note;
    }

    // A letter, then # or b, then the octave: A4 = 440 Hz (MIDI note 69), C4 = middle C.
    const auto text = note.toString().trim();
    const juce::String letters ("C D EF G A B");  // the place of each letter in an octave (semitones from C)
    const int letter = letters.indexOfChar (juce::CharacterFunctions::toUpperCase (text[0]));
    int i = 1, semitone = letter;

    if (text.length() > 1 && (text[1] == '#' || text[1] == 'b'))
    {
        semitone += text[1] == '#' ? 1 : -1;
        ++i;
    }

    const auto octave = text.substring (i);

    if (text.isEmpty() || letter < 0 || letters[letter] == ' ' || octave.isEmpty() || ! octave.containsOnly ("-0123456789"))
        throw PresetError ("strings: '" + text + "' is not a note. Use a name like A2, F#4 or Bb3, or a number of Hz");

    const int midi = 12 * (octave.getIntValue() + 1) + semitone;
    const double hz = 440.0 * std::pow (2.0, (midi - 69) / 12.0);

    if (hz < 20 || hz > 5000)
        throw PresetError ("strings: '" + text + "' is outside 20 Hz to 5 kHz");

    return hz;
}

SympatheticStrings::SympatheticStrings (const Cfg& cfg, const BuildContext& ctx)
    : Block (cfg, { "notes", "coupling_db", "sustain_s", "string_tone_hz", "mix_db" })
{
    const double rate = ctx.rate;
    const double sustain = cfg.number ("sustain_s", 4.0), toneHz = cfg.number ("string_tone_hz", 2000.0);

    if (sustain <= 0 || toneHz <= 0 || toneHz >= ctx.checkRate / 2)
        throw PresetError ("strings: sustain_s must be above 0 and string_tone_hz between 0 and half the sample rate");

    coupling = dbToGain (cfg.number ("coupling_db", -30.0));
    mix = dbToGain (cfg.number ("mix_db", 0.0));
    tone = 1 - std::exp (-2 * juce::MathConstants<double>::pi * toneHz / rate);

    const auto notes = cfg.get ("notes");

    if (! notes.isVoid() && ! notes.isArray())
        throw PresetError ("strings: notes must be a list, like [\"D3\", \"A3\", \"D4\"]");

    // Without a list: a guitar's own open strings, which ring along when you play in their key.
    const juce::Array<juce::var> standard { "E2", "A2", "D3", "G3", "B3", "E4" };

    for (auto& note : notes.isArray() ? *notes.getArray() : standard)
    {
        String s;
        s.hz = noteHz (note);
        const double period = rate / s.hz;

        // The low-pass delays the note a little (its phase) and lowers it a little: read that much sooner, and give
        // that much back, so the string rings at its note for sustain_s.
        const double w = 2 * juce::MathConstants<double>::pi / period;
        const std::complex<double> lp = tone / (1.0 - (1.0 - tone) * std::polar (1.0, -w));
        s.read = std::max (2.0, period + std::arg (lp) / w);
        s.gain = std::pow (10.0, -3.0 * period / (rate * sustain)) / std::abs (lp);
        s.line.assign ((size_t) std::ceil (period) + 4, 0.0);
        strings.push_back (std::move (s));
    }

    if (strings.empty())
        throw PresetError ("strings: notes needs at least one note");
}

void SympatheticStrings::reset() noexcept
{
    for (auto& s : strings)
    {
        std::fill (s.line.begin(), s.line.end(), 0.0);
        s.write = 0;
        s.smooth = 0.0;
    }
}

void SympatheticStrings::process (double* x, int n) noexcept
{
    for (int i = 0; i < n; ++i)
    {
        const double in = x[i];
        double sum = 0.0;

        for (auto& s : strings)
        {
            // One period back, between two samples.
            const int size = (int) s.line.size();
            const double back = s.write - s.read;
            const double from = back < 0 ? back + size : back;
            const auto k = (int) from;
            const double frac = from - k;
            const double old = s.line[(size_t) k] * (1 - frac) + s.line[(size_t) ((k + 1) % size)] * frac;
            s.smooth += tone * (old - s.smooth);
            const double v = coupling * in + s.gain * s.smooth;
            s.line[(size_t) s.write] = v;
            s.write = (s.write + 1) % size;
            sum += v;
        }

        x[i] = in + mix * sum;
    }
}

juce::String SympatheticStrings::describe() const
{
    juce::StringArray hz;

    for (auto& s : strings)
        hz.add (juce::String (s.hz, 1));

    return juce::String ((int) strings.size()) + " strings at " + hz.joinIntoString (", ") + " Hz";
}

} // namespace amp
