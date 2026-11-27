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
        const double keep = std::pow (10.0, -3.0 * period / (rate * sustain));  // what the note keeps per period
        const auto lowpass = [w] (double t) { return t / (1.0 - (1.0 - t) * std::polar (1.0, -w)); };
        s.tone = tone;

        // The loop gain at 0 Hz is keep / |lp(note)|: above 1 the string gains energy. So the low-pass may cut the note
        // no further than sqrt (keep): then 0 Hz dies in 2 x sustain_s, the note still in sustain_s, and this string's
        // highs fade less than asked. |lp| = sqrt (keep): a quadratic in the pole (as in Restring's tonePole).
        if (std::abs (lowpass (tone)) < std::sqrt (keep))
        {
            const double c1 = 2 * std::pow (std::sin (w / 2), 2), a = 1 - keep, bm = a + keep * c1;
            s.tone = 1 - a / (bm + std::sqrt (std::max (0.0, bm * bm - a * a)));
        }

        const std::complex<double> lp = lowpass (s.tone);
        s.read = std::max (2.0, period + std::arg (lp) / w);
        s.gain = keep / std::abs (lp);
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
            s.smooth += s.tone * (old - s.smooth);
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
