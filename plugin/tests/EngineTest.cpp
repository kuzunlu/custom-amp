// Runs the C++ engine without a DAW: the theory checks, the reference recordings, CPU time, test audio.
//
//   EngineTest check [repo folder]    theory checks, settings.json against the blocks, every preset against its
//                                     reference recording in tests/reference (fails when the sound changed)
//   EngineTest record [repo folder]   records the references again (only after a change MEANT to change the sound)
//   EngineTest render <preset.json> <in.wav> <out.wav> [factorLog2] [blockSize]
//                                     the whole plugin engine; prints the CPU time
//   EngineTest make-audio [repo folder]
//                                     audio/test_di.wav (a 10 s fake guitar riff) and audio/fake_4x12.wav (a fake cab)
//
// The checks compare the engine with theory and with published data. Until 2026-11-24 a Python prototype was a
// second, independent copy of all this math (the engine matched it to -131 dB or better); these checks replace it.

#include "../Source/dsp/Engine.h"

#include <juce_audio_formats/juce_audio_formats.h>

#include <array>
#include <complex>
#include <iostream>
#include <set>

namespace
{
int failures = 0;

void check (bool ok, const juce::String& what)
{
    std::cout << (ok ? "  ok    " : "  FAIL  ") << what << "\n";

    if (! ok)
        ++failures;
}

const double pi = juce::MathConstants<double>::pi;

juce::var loadJson (const juce::File& file)
{
    juce::var v;
    auto result = juce::JSON::parse (file.loadFileAsString(), v);

    if (result.failed())
        throw amp::PresetError (file.getFileName() + ": " + result.getErrorMessage());

    return v;
}

std::vector<double> sine (double freq, double amplitude, double seconds, double rate)
{
    std::vector<double> x ((size_t) (seconds * rate));

    for (size_t i = 0; i < x.size(); ++i)
        x[i] = amplitude * std::sin (2 * pi * freq * (double) i / rate);

    return x;
}

// The amplitude of one frequency in the second half of y (the tests use whole numbers of cycles there).
double tone (const std::vector<double>& y, double freq, double rate)
{
    std::complex<double> sum;
    const size_t from = y.size() / 2;

    for (size_t i = from; i < y.size(); ++i)
        sum += y[i] * std::polar (1.0, -2 * pi * freq * (double) i / rate);

    return 2 * std::abs (sum) / (double) (y.size() - from);
}

// Harmonic k of the note, in % of the note.
double harmonicPercent (const std::vector<double>& y, double freq, int k, double rate)
{
    return 100 * tone (y, freq * k, rate) / tone (y, freq, rate);
}

// Plays x through a chain of blocks at `rate` (no oversampling: the rate IS the amp's rate).
std::vector<double> runChain (const juce::var& chain, std::vector<double> x, double rate)
{
    auto* preset = new juce::DynamicObject();
    preset->setProperty ("chain", chain);
    amp::Amp a (juce::var (preset), rate, rate, juce::File(), juce::File ("C:/no/such/folder/ir.wav"));
    const int block = 4096;
    a.prepare (block, block);

    for (size_t i = 0; i < x.size(); i += (size_t) block)
    {
        const int n = (int) std::min ((size_t) block, x.size() - i);
        a.processCore (x.data() + i, n);
        a.processTail (x.data() + i, n);
    }

    return x;
}

std::vector<double> runJson (const juce::String& chainJson, const std::vector<double>& x, double rate)
{
    return runChain (juce::JSON::parse (chainJson), x, rate);
}

//==============================================================================
// The published closed form of the classic tone stack (Yeh and Smith 2006): ideal source, no load.
std::complex<double> yeh (double f, double t, double m, double l, double C1, double C2, double C3,
                          double R1, double R2, double R3, double R4)
{
    const double b1 = t*C1*R1 + m*C3*R3 + l*(C1*R2 + C2*R2) + (C1*R3 + C2*R3);
    const double b2 = (t*(C1*C2*R1*R4 + C1*C3*R1*R4) - m*m*(C1*C3*R3*R3 + C2*C3*R3*R3)
                       + m*(C1*C3*R1*R3 + C1*C3*R3*R3 + C2*C3*R3*R3)
                       + l*(C1*C2*R1*R2 + C1*C2*R2*R4 + C1*C3*R2*R4)
                       + l*m*(C1*C3*R2*R3 + C2*C3*R2*R3)
                       + (C1*C2*R1*R3 + C1*C2*R3*R4 + C1*C3*R3*R4));
    const double b3 = (l*m*(C1*C2*C3*R1*R2*R3 + C1*C2*C3*R2*R3*R4)
                       - m*m*(C1*C2*C3*R1*R3*R3 + C1*C2*C3*R3*R3*R4)
                       + m*(C1*C2*C3*R1*R3*R3 + C1*C2*C3*R3*R3*R4)
                       + t*C1*C2*C3*R1*R3*R4 - t*m*C1*C2*C3*R1*R3*R4
                       + t*l*C1*C2*C3*R1*R2*R4);
    const double a1 = (C1*R1 + C1*R3 + C2*R3 + C2*R4 + C3*R4) + m*C3*R3 + l*(C1*R2 + C2*R2);
    const double a2 = (m*(C1*C3*R1*R3 - C2*C3*R3*R4 + C1*C3*R3*R3 + C2*C3*R3*R3)
                       + l*m*(C1*C3*R2*R3 + C2*C3*R2*R3)
                       - m*m*(C1*C3*R3*R3 + C2*C3*R3*R3)
                       + l*(C1*C2*R2*R4 + C1*C2*R1*R2 + C1*C3*R2*R4 + C2*C3*R2*R4)
                       + (C1*C2*R1*R4 + C1*C3*R1*R4 + C1*C2*R3*R4 + C1*C2*R1*R3 + C1*C3*R3*R4 + C2*C3*R3*R4));
    const double a3 = (l*m*(C1*C2*C3*R1*R2*R3 + C1*C2*C3*R2*R3*R4)
                       - m*m*(C1*C2*C3*R1*R3*R3 + C1*C2*C3*R3*R3*R4)
                       + m*(C1*C2*C3*R3*R3*R4 + C1*C2*C3*R1*R3*R3 - C1*C2*C3*R1*R3*R4)
                       + l*C1*C2*C3*R1*R2*R4 + C1*C2*C3*R1*R3*R4);
    const std::complex<double> s (0.0, 2 * pi * f);
    return (b1*s + b2*s*s + b3*s*s*s) / (1.0 + a1*s + a2*s*s + a3*s*s*s);
}

void toneStackChecks()
{
    std::cout << "Tone stack: the block against the published formula (Yeh and Smith 2006)\n";
    struct Style { const char* name; double C1, C2, C3, R1, R2, R3, R4; };
    const Style styles[] { { "british", 470e-12, 22e-9, 22e-9, 220e3, 1e6, 25e3, 33e3 },
                           { "american", 250e-12, 20e-9, 20e-9, 250e3, 1e6, 25e3, 56e3 } };
    const double rate = 48000.0;

    // Its impulse response, read at each frequency. The digital filter is the bilinear transform of the circuit,
    // which moves frequency f to (rate / pi) tan (pi f / rate) exactly, so the formula is read there.
    std::vector<double> impulse (1 << 17, 0.0);
    impulse[0] = 1.0;
    std::vector<double> freqs;

    for (int i = 0; i < 60; ++i)
        freqs.push_back (20.0 * std::pow (1000.0, i / 59.0));

    for (auto& p : styles)
    {
        double worst = 0.0;

        for (auto knobs : std::initializer_list<std::array<int, 3>> { { 5, 5, 5 }, { 0, 0, 0 }, { 10, 10, 10 }, { 10, 0, 3 }, { 2, 8, 10 }, { 7, 1, 0 } })
        {
            const auto chain = juce::String ("[ { \"type\": \"tonestack\", \"style\": \"") + p.name + "\", \"treble\": " + juce::String (knobs[0])
                               + ", \"mid\": " + juce::String (knobs[1]) + ", \"bass\": " + juce::String (knobs[2])
                               + ", \"treble_taper\": \"linear\", \"mid_taper\": \"linear\", \"bass_taper\": \"linear\","
                                 " \"source_ohms\": 0.001, \"load_ohms\": 1e12 } ]";
            const auto h = runJson (chain, impulse, rate);
            const double t = knobs[0] / 10.0, m = knobs[1] / 10.0, l = knobs[2] / 10.0;
            // the block puts 1 ohm in a pot turned to an end; give the formula the same tiny value
            const double tEff = t > 0.5 ? 1 - std::max (p.R1 * (1 - t), 1.0) / p.R1 : std::max (p.R1 * t, 1.0) / p.R1;
            const double mEff = std::max (p.R3 * m, 1.0) / p.R3, lEff = std::max (p.R2 * l, 1.0) / p.R2;

            for (auto f : freqs)
            {
                std::complex<double> mine;

                for (size_t n = 0; n < h.size(); ++n)
                    mine += h[n] * std::polar (1.0, -2 * pi * f * (double) n / rate);

                const double analogF = rate / pi * std::tan (pi * f / rate);
                const auto theirs = yeh (analogF, tEff, mEff, lEff, p.C1, p.C2, p.C3, p.R1, p.R2, p.R3, p.R4);
                worst = std::max (worst, std::abs (20 * std::log10 (std::abs (mine) / std::abs (theirs))));
            }
        }

        check (worst < 0.001, juce::String (p.name) + ", 6 knob settings, 20 Hz to 20 kHz: within " + juce::String (worst, 5) + " dB");
    }
}

void triodeChecks()
{
    std::cout << "Triode against tube data (12AX7, 300 V, 100k, -1.5 V)\n";
    amp::TriodeInfo info;
    amp::makeTriodeCurve ("12AX7", 300, 100e3, -1.5, info);
    check (std::abs (info.plateV - 205) < 3 && std::abs (info.plateMa - 0.95) < 0.03 && std::abs (info.gain - 61) < 1.5,
           "operating point: plate " + juce::String (info.plateV, 1) + " V, " + juce::String (info.plateMa, 3) + " mA, gain "
               + juce::String (info.gain, 1) + " (tube data: about 190-205 V, gain about 60)");

    const double rate = 384000.0;
    const auto y = runJson (R"([ { "type": "gain", "curve": "triode" } ])", sine (220, 0.5, 1.0, rate), rate);
    const double h2 = harmonicPercent (y, 220, 2, rate), h3 = harmonicPercent (y, 220, 3, rate);
    check (std::abs (h2 - 2.64) < 0.05 && std::abs (h3 - 0.126) < 0.02,
           "at 0.5 V: 2nd harmonic " + juce::String (h2, 2) + "%, 3rd " + juce::String (h3, 3) + "% (2.64% / 0.126% before, in the Python version too)");
    const auto cold = runJson (R"([ { "type": "gain", "curve": "triode", "bias_v": -2.5 } ])", sine (220, 2.0, 1.0, rate), rate);
    const double coldH2 = harmonicPercent (cold, 220, 2, rate);
    check (std::abs (coldH2 - 24) < 4, "cold bias -2.5 V at 2 V: 2nd harmonic " + juce::String (coldH2, 1) + "% (about 24%)");
}

void feedbackChecks()
{
    std::cout << "Feedback loop against its equation\n";
    const double rate = 192000.0;
    const auto loopGain = [rate] (const juce::String& amount, const juce::String& path, const juce::String& inner, double freq, double level)
    {
        const auto y = runJson ("[ { \"type\": \"feedback\", \"amount\": " + amount + ", \"path\": " + path + ", \"chain\": " + inner + " } ]",
                                 sine (freq, level, 0.5, rate), rate);
        return tone (y, freq, rate) / level;
    };
    const juce::String ten = R"([ { "type": "eq", "level_db": 20 } ])";  // a clean 10x gain

    for (double f : { 200.0, 1000.0, 5000.0 })
    {
        const double g = loopGain ("0.1", "[]", ten, f, 0.01);
        check (std::abs (g - 5.0) < 0.005, "gain 10, amount 0.1 at " + juce::String (f, 0) + " Hz: " + juce::String (g, 4) + " (theory 10 / (1 + 10 x 0.1) = 5)");
    }

    const auto shelf = amp::Biquad::make ("highshelf", 3000, rate, 0.707, -12, rate);

    for (double f : { 200.0, 8000.0 })
    {
        const auto z = std::polar (1.0, 2 * pi * f / rate);
        const auto h = (shelf.b0 + shelf.b1 / z + shelf.b2 / (z * z)) / (1.0 + shelf.a1 / z + shelf.a2 / (z * z));
        const double theory = std::abs (10.0 / (1.0 + 10.0 * 0.1 * h));
        const double g = loopGain ("0.1", R"([ { "kind": "highshelf", "freq": 3000, "gain_db": -12 } ])", ten, f, 0.01);
        check (std::abs (g - theory) < 0.01 * theory, "with a -12 dB shelf in the path at " + juce::String (f, 0) + " Hz: "
                                                          + juce::String (g, 3) + " (theory " + juce::String (theory, 3) + ")");
    }

    // Loop gain 63: a loop with a 1-sample delay would oscillate; the solver must not.
    const double strong = loopGain ("2.0", "[]", R"([ { "type": "gain", "drive_db": 30, "curve": "hard" } ])", 220, 0.1);
    const double strongTheory = std::pow (10.0, 30 / 20.0) / (1 + 2 * std::pow (10.0, 30 / 20.0));
    check (std::isfinite (strong) && std::abs (strong - strongTheory) < 0.01 * strongTheory,
           "loop gain 63 is stable: " + juce::String (strong, 4) + " (theory " + juce::String (strongTheory, 4) + ")");

    for (auto [amount, theory] : { std::pair { "-0.25", 4.0 }, std::pair { "-0.4", 10.0 } })
    {
        const double g = loopGain (amount, "[]", R"([ { "type": "eq", "level_db": 6.020599913279624 } ])", 1000, 0.01);
        check (std::abs (g - theory) < 0.002 * theory, juce::String ("positive feedback, gain 2, amount ") + amount + ": "
                                                           + juce::String (g, 4) + " (theory " + juce::String (theory, 1) + ")");
    }
}

void couplingChecks()
{
    std::cout << "Coupling capacitor: a 1st-order high-pass at 1 / (2 pi R C)\n";
    // 1 nF into 994718 ohm = a 160.00 Hz corner. A tiny signal keeps the hard curve straight (only the filter acts).
    const double rate = 96000.0, level = 0.001;
    const juce::String stage = R"([ { "type": "gain", "curve": "hard", "coupling_nf": 1, "grid_ohms": 994718.394 } ])";

    for (auto [f, theory] : { std::pair { 160.0, -3.0103 }, std::pair { 16.0, 20 * std::log10 (16 / std::sqrt (16.0 * 16 + 160.0 * 160)) } })
    {
        const double db = 20 * std::log10 (tone (runJson (stage, sine (f, level, 1.0, rate), rate), f, rate) / level);
        check (std::abs (db - theory) < 0.02, "at " + juce::String (f, 0) + " Hz: " + juce::String (db, 3) + " dB (theory "
                                                  + juce::String (theory, 3) + " dB; a 2nd-order filter would be -40 dB at 16 Hz)");
    }
}

void powerChecks()
{
    std::cout << "Power tubes against their datasheets\n";
    struct Point { const char* tube; double grid, ma, maPerV; };

    for (auto& p : { Point { "EL34", -13.5, 100, 11.0 }, Point { "6L6GC", -14.0, 72, 6.0 }, Point { "EL84", -7.3, 48, 11.3 },
                     Point { "6V6GT", -12.5, 45, 4.1 }, Point { "KT88", -15.0, 140, 11.5 }, Point { "6550", -14.0, 140, 11.0 } })
    {
        const double i = 1000 * amp::pentodeCurrent (p.tube, p.grid, 250, 250);
        const double gm = (amp::pentodeCurrent (p.tube, p.grid + 1e-3, 250, 250) - amp::pentodeCurrent (p.tube, p.grid - 1e-3, 250, 250)) / 2e-3 * 1000;
        check (std::abs (i - p.ma) < 0.005 * p.ma && std::abs (gm - p.maPerV) < 0.01 * p.maPerV,
               juce::String (p.tube) + " at 250 V, " + juce::String (p.grid, 1) + " V: " + juce::String (i, 1) + " mA, "
                   + juce::String (gm, 2) + " mA/V (sheet: " + juce::String (p.ma, 0) + " mA, " + juce::String (p.maPerV, 1) + " mA/V)");
    }

    struct Rated { const char* tube; double plateV, screenV, bias, raa, watts; };

    for (auto& a : { Rated { "EL34", 450, 450, -38, 3400, 48 }, Rated { "6L6GC", 460, 460, -50, 4200, 45 }, Rated { "EL84", 320, 320, -10, 8000, 17 },
                     Rated { "6V6GT", 420, 420, -35, 8000, 22 }, Rated { "KT88", 560, 300, -45, 4500, 100 }, Rated { "6550", 600, 300, -55, 5000, 100 } })
    {
        const auto pair = amp::PushPull::make (a.tube, a.plateV, a.screenV, a.bias, a.raa, 0.0);
        check (std::abs (pair.maxWatts - a.watts) < 2, juce::String (a.tube) + " pair: about " + juce::String (pair.maxWatts, 1)
                                                           + " W (known: " + juce::String (a.watts, 0) + " W)");
    }

    const double rate = 384000.0;
    double second[2] {};

    for (int k = 0; k < 2; ++k)
    {
        const auto y = runJson (juce::String (R"([ { "type": "power", "tube": "EL34", "plate_v": 450, "bias_v": -38, "raa_ohms": 3400, "mismatch": )")
                                     + (k == 0 ? "0" : "0.1") + " } ]",
                                 sine (220, 30, 1.0, rate), rate);
        second[k] = harmonicPercent (y, 220, 2, rate);
    }

    check (second[0] < 0.01 && std::abs (second[1] - 0.95) < 0.25, "push-pull cancels even harmonics: 2nd " + juce::String (second[0], 4)
                                                                       + "% matched, " + juce::String (second[1], 2) + "% with 10% mismatch (about 0.95%)");
}

//==============================================================================
// A setting's choices in settings.json: a list, or an object of {choice: description}.
std::vector<juce::var> choiceValues (const juce::var& choices)
{
    std::vector<juce::var> values;

    if (auto* list = choices.getArray())
        values.assign (list->begin(), list->end());
    else if (auto* named = choices.getDynamicObject())
        for (auto& p : named->getProperties())
            values.push_back (p.name.toString());

    return values;
}

// settings.json (the help text the plugin shows) must describe the blocks as they are.
void settingsChecks (const juce::File& root)
{
    std::cout << "shared/settings.json against the blocks\n";
    const auto doc = loadJson (root.getChildFile ("shared/settings.json"))["blocks"];
    const auto signal = [] {
        // a note dying away from 1 V, so gates open and close and sag and bias shift move
        auto x = sine (220, 1.0, 1.0, 96000.0);

        for (size_t i = 0; i < x.size(); ++i)
            x[i] *= std::exp (-8.0 * (double) i / 96000.0);

        return x;
    }();
    const auto required = [] (const juce::String& type)
    {
        if (type == "ir") return juce::JSON::parse (R"({ "file": "no_such_file.wav" })");
        if (type == "split") return juce::JSON::parse (R"({ "paths": [ {} ] })");
        return juce::JSON::parse ("{}");
    };
    const auto block = [&required] (const juce::String& type, std::initializer_list<std::pair<juce::String, juce::var>> extra)
    {
        auto* o = new juce::DynamicObject();
        o->setProperty ("type", type);

        const auto needed = required (type);  // kept alive while its properties are copied

        for (auto& p : needed.getDynamicObject()->getProperties())
            o->setProperty (p.name, p.value);

        for (auto& [k, v] : extra)
            o->setProperty (juce::Identifier (k), v);

        return juce::var (o);
    };
    const auto problem = [&signal] (const juce::var& cfg) -> juce::String
    {
        try { runChain (juce::var (juce::Array<juce::var> { cfg }), signal, 96000.0); }
        catch (const std::exception& e) { return e.what(); }
        return {};
    };

    for (auto& entry : doc.getDynamicObject()->getProperties())
    {
        const auto type = entry.name.toString();
        const auto settings = entry.value["settings"].getDynamicObject()->getProperties();

        // The names: the block's error for an unknown setting lists every name it takes.
        const auto message = problem (block (type, { { "not_a_setting", 1 } }));
        std::set<juce::String> taken, documented { "type", "id" };

        for (auto& name : juce::StringArray::fromTokens (message.fromFirstOccurrenceOf ("Allowed: ", false, false), ", ", ""))
            if (name.trim().isNotEmpty())
                taken.insert (name.trim());

        for (auto& s : settings)
            documented.insert (s.name.toString());

        juce::StringArray onlyBlock, onlyDoc;

        for (auto& n : taken)
            if (documented.count (n) == 0)
                onlyBlock.add (n);

        for (auto& n : documented)
            if (taken.count (n) == 0)
                onlyDoc.add (n);

        check (taken == documented, type + ": settings.json has the same setting names as the block"
                                        + (taken == documented ? juce::String() : " (only in the block: " + onlyBlock.joinIntoString (", ")
                                                                                       + "; only in settings.json: " + onlyDoc.joinIntoString (", ") + ")"));

        // The defaults: a block given every documented default plays exactly like one given nothing
        // (settings that belong to one curve are checked with that curve).
        std::map<juce::String, std::vector<std::pair<juce::String, juce::var>>> groups { { {}, {} } };

        for (auto& s : settings)
            if (! s.value["default"].isVoid() && s.value["type"] != "block-list" && ! required (type).hasProperty (s.name))
                groups[s.value["curve"].toString()].push_back ({ s.name.toString(), s.value["default"] });

        for (auto& [curve, values] : groups)
        {
            auto with = block (type, {}), without = block (type, {});

            if (curve.isNotEmpty())
                for (auto* cfg : { &with, &without })
                    cfg->getDynamicObject()->setProperty ("curve", curve);

            for (auto& [k, v] : values)
                with.getDynamicObject()->setProperty (juce::Identifier (k), v);

            juce::String why;
            bool same = false;

            try
            {
                same = runChain (juce::var (juce::Array<juce::var> { with }), signal, 96000.0)
                       == runChain (juce::var (juce::Array<juce::var> { without }), signal, 96000.0);
            }
            catch (const std::exception& e) { why = e.what(); }

            check (same, type + (curve.isNotEmpty() ? " (curve " + curve + ")" : juce::String()) + ": the documented defaults are the block's own"
                             + (why.isNotEmpty() ? ": " + why : juce::String()));
        }

        // Every documented choice works.
        int choicesTried = 0;
        juce::StringArray broken;

        for (auto& s : settings)
            for (auto& choice : choiceValues (s.value["choices"]))
            {
                auto cfg = block (type, { { s.name.toString(), choice } });

                if (s.value["curve"].toString().isNotEmpty())
                    cfg.getDynamicObject()->setProperty ("curve", s.value["curve"]);

                ++choicesTried;

                if (const auto why = problem (cfg); why.isNotEmpty())
                    broken.add (s.name.toString() + " = " + choice.toString() + ": " + why);
            }

        if (choicesTried > 0)
            check (broken.isEmpty(), type + ": all " + juce::String (choicesTried) + " documented choices work"
                                         + (broken.isEmpty() ? juce::String() : " (" + broken.joinIntoString ("; ") + ")"));
    }

    const auto names = [] (const juce::var& choices)
    {
        juce::StringArray a;

        for (auto& v : choiceValues (choices))
            a.add (v.toString());

        a.sort (false);
        return a;
    };
    auto triodes = amp::triodeNames(), pentodes = amp::pentodeNames();
    triodes.sort (false);
    pentodes.sort (false);
    check (names (doc["gain"]["settings"]["tube"]["choices"]) == triodes && names (doc["power"]["settings"]["tube"]["choices"]) == pentodes,
           "the tube lists are the same as the code's");
}

//==============================================================================
// Every preset against its recording: a change that is not meant to change the sound must not.
const double referenceSeconds = 0.3;

std::vector<juce::File> referencePresets (const juce::File& root)
{
    auto files = root.getChildFile ("presets").findChildFiles (juce::File::findFiles, false, "*.json");
    files.addArray (root.getChildFile ("tests/presets").findChildFiles (juce::File::findFiles, false, "*.json"));
    files.sort();
    return { files.begin(), files.end() };
}

std::vector<float> renderReference (const juce::File& presetFile)
{
    amp::Engine::Options options;
    options.preset = loadJson (presetFile);
    options.presetFolder = presetFile.getParentDirectory();
    options.irOverride = juce::File ("C:/no/such/folder/ir.wav");  // always the built-in test cab, whatever audio/ holds
    options.sampleRate = 48000.0;
    options.maxBlock = 256;
    options.factorLog2 = 3;
    options.fixedLevel = 1.0;  // the amp's own volts, no auto level
    amp::Engine engine (options);
    auto x = amp::makeLevelRiff (48000.0);
    x.resize ((size_t) (referenceSeconds * 48000.0));

    for (size_t i = 0; i < x.size(); i += 256)
        engine.process (x.data() + i, (int) std::min ((size_t) 256, x.size() - i));

    return { x.begin(), x.end() };
}

void referenceChecks (const juce::File& root)
{
    std::cout << "Every preset against its reference recording (tests/reference, " << referenceSeconds << " s of the level riff)\n";

    for (auto& file : referencePresets (root))
    {
        const auto refFile = root.getChildFile ("tests/reference/" + file.getFileNameWithoutExtension() + ".f32");
        juce::MemoryBlock data;

        if (! refFile.loadFileAsData (data))
        {
            check (false, file.getFileName() + ": no recording. Make one: EngineTest record");
            continue;
        }

        std::vector<float> ref (data.getSize() / sizeof (float));
        std::memcpy (ref.data(), data.getData(), ref.size() * sizeof (float));

        try
        {
            const auto now = renderReference (file);
            float peak = 0.0f, worst = 0.0f;

            for (size_t i = 0; i < std::min (now.size(), ref.size()); ++i)
            {
                peak = std::max (peak, std::abs (ref[i]));
                worst = std::max (worst, std::abs (now[i] - ref[i]));
            }

            const double db = juce::Decibels::gainToDecibels ((double) worst / std::max (1e-30f, peak), -300.0);
            check (now.size() == ref.size() && db < -100.0, file.getFileName() + ": " + juce::String (juce::roundToInt (db)) + " dB from its recording");
        }
        catch (const std::exception& e)
        {
            check (false, file.getFileName() + ": " + e.what());
        }
    }
}

int record (const juce::File& root)
{
    const auto folder = root.getChildFile ("tests/reference");
    folder.createDirectory();

    for (auto& file : referencePresets (root))
    {
        const auto y = renderReference (file);
        const auto out = folder.getChildFile (file.getFileNameWithoutExtension() + ".f32");
        out.replaceWithData (y.data(), y.size() * sizeof (float));
        std::cout << "recorded " << out.getFullPathName() << "\n";
    }

    return 0;
}

//==============================================================================
// Test audio: a 10 s fake guitar riff made with the Karplus-Strong plucked-string method, and the fake 4x12 cab.
void writeWav (const juce::File& file, const std::vector<double>& x)
{
    juce::AudioBuffer<float> buffer (1, (int) x.size());

    for (size_t i = 0; i < x.size(); ++i)
        buffer.setSample (0, (int) i, (float) x[i]);

    file.deleteFile();
    juce::WavAudioFormat wav;
    std::unique_ptr<juce::OutputStream> stream = std::make_unique<juce::FileOutputStream> (file);
    auto writer = wav.createWriterFor (stream, juce::AudioFormatWriterOptions{}.withSampleRate (48000.0).withNumChannels (1).withBitsPerSample (24));

    if (writer == nullptr)
        throw std::runtime_error ("can not write " + file.getFullPathName().toStdString());

    writer->writeFromAudioSampleBuffer (buffer, 0, buffer.getNumSamples());
}

std::vector<double> pluck (int midi, double hold, juce::Random& rng, double decay, double bright)
{
    // One plucked string: a triangle (the string pulled aside by the pick) runs around a delay loop that slowly smooths it.
    const double rate = 48000.0, freq = 440 * std::pow (2.0, (midi - 69) / 12.0);
    const int period = std::max (2, (int) std::round (rate / freq - 0.5));
    const int n = (int) ((hold + 0.03) * rate);
    std::vector<double> y ((size_t) n + 1, 0.0);
    const double pickPoint = 0.5 - 0.4 * bright;  // nearer the bridge = more highs
    std::vector<double> burst ((size_t) period);
    double mean = 0.0;

    for (int i = 0; i < period; ++i)
    {
        const double pos = (double) i / period;
        burst[(size_t) i] = (pos < pickPoint ? pos / pickPoint : (1 - pos) / (1 - pickPoint)) + 0.25 * (rng.nextDouble() * 2 - 1);
        mean += burst[(size_t) i] / period;
    }

    for (int i = 0; i < period; ++i)
        y[(size_t) i + 1] = burst[(size_t) i] - mean;

    for (int i = period + 1; i <= n; ++i)
        y[(size_t) i] = decay * 0.5 * (y[(size_t) (i - period)] + y[(size_t) (i - period - 1)]);

    y.erase (y.begin());
    const int fade = (int) (0.03 * rate);  // the hand stops the string

    for (int i = 0; i < fade; ++i)
        y[y.size() - (size_t) fade + (size_t) i] *= 1.0 - (double) i / (fade - 1);

    return y;
}

int makeAudio (const juce::File& root)
{
    struct Style { double decay, bright, strum; };
    const Style ring { 0.998, 0.6, 0.012 }, mute { 0.93, 0.3, 0.004 }, lead { 0.997, 0.7, 0.0 };
    struct Event { double t0; std::vector<int> notes; double duration; Style style; };
    std::vector<Event> events { { 0.0, { 40, 47, 52, 56, 59, 64 }, 2.0, ring } };

    for (int i = 0; i < 4; ++i)
        events.push_back ({ 2.0 + i * 0.25, { 40, 47 }, 0.25, mute });

    events.push_back ({ 3.0, { 43, 50, 55 }, 0.5, ring });
    events.push_back ({ 3.5, { 45, 52, 57 }, 0.5, ring });

    for (int i = 0; i < 4; ++i)
        events.push_back ({ 4.0 + i * 0.25, { 40, 47 }, 0.25, mute });

    events.push_back ({ 5.0, { 50, 57, 62 }, 0.5, ring });
    events.push_back ({ 5.5, { 48, 55, 60 }, 0.5, ring });
    const int leadNotes[] { 64, 67, 69, 71, 74, 76 };

    for (int i = 0; i < 6; ++i)
        events.push_back ({ 6.0 + i * 0.25, { leadNotes[i] }, 0.25, lead });

    events.push_back ({ 7.5, { 40, 47, 52 }, 2.0, ring });

    const double rate = 48000.0;
    std::vector<double> out ((size_t) (10 * rate), 0.0);
    juce::Random rng (1);

    for (auto& e : events)
        for (size_t k = 0; k < e.notes.size(); ++k)
        {
            const double offset = (double) k * e.style.strum;
            const auto note = pluck (e.notes[k], e.duration - offset, rng, e.style.decay, e.style.bright);
            const double gain = 0.8 + 0.2 * rng.nextDouble();
            const auto start = (size_t) ((e.t0 + offset) * rate);

            for (size_t i = 0; i < note.size() && start + i < out.size(); ++i)
                out[start + i] += note[i] * gain;
        }

    // The pickup and cable: a small peak around 3.5 kHz, then the highs fall away fast.
    auto pickup = amp::Biquad::make ("lowpass", 3500, rate, 1.2, 0, rate);
    double peak = 0.0;

    for (auto& v : out)
    {
        v = pickup.tick (v);
        peak = std::max (peak, std::abs (v));
    }

    for (auto& v : out)
        v *= 0.5 / peak;  // peak at about -6 dBFS, like a real DI track

    const auto audio = root.getChildFile ("audio");
    audio.createDirectory();
    writeWav (audio.getChildFile ("test_di.wav"), out);
    writeWav (audio.getChildFile ("fake_4x12.wav"), amp::makeFakeCabIr());
    std::cout << "wrote " << audio.getChildFile ("test_di.wav").getFullPathName() << " and fake_4x12.wav\n";
    return 0;
}

//==============================================================================
int render (const juce::StringArray& args)
{
    const juce::File presetFile (args[1]);
    juce::AudioFormatManager formats;
    formats.registerBasicFormats();
    std::unique_ptr<juce::AudioFormatReader> reader (formats.createReaderFor (juce::File (args[2])));

    if (reader == nullptr)
        throw std::runtime_error ("can not read " + args[2].toStdString() + " (make it: EngineTest make-audio)");

    juce::AudioBuffer<float> input ((int) reader->numChannels, (int) reader->lengthInSamples);
    reader->read (&input, 0, input.getNumSamples(), 0, true, true);
    std::vector<double> x ((size_t) input.getNumSamples(), 0.0);

    // The amp is mono: the average of the channels.
    for (int i = 0; i < input.getNumSamples(); ++i)
    {
        double sum = 0.0;

        for (int ch = 0; ch < input.getNumChannels(); ++ch)
            sum += (double) input.getSample (ch, i);

        x[(size_t) i] = sum / input.getNumChannels();
    }

    amp::Engine::Options options;
    options.preset = loadJson (presetFile);
    options.presetFolder = presetFile.getParentDirectory();
    options.sampleRate = reader->sampleRate;
    options.factorLog2 = args.size() > 4 ? args[4].getIntValue() : 3;
    options.maxBlock = args.size() > 5 ? args[5].getIntValue() : 256;

    const auto buildStart = juce::Time::getMillisecondCounterHiRes();
    amp::Engine engine (options);
    const auto buildMs = juce::Time::getMillisecondCounterHiRes() - buildStart;

    const auto start = juce::Time::getMillisecondCounterHiRes();

    for (size_t i = 0; i < x.size(); i += (size_t) options.maxBlock)
        engine.process (x.data() + i, (int) std::min ((size_t) options.maxBlock, x.size() - i));

    const auto ms = juce::Time::getMillisecondCounterHiRes() - start;
    const double seconds = (double) x.size() / reader->sampleRate;

    std::cout << engine.amp.name << "\n";

    for (auto& line : engine.amp.report)
        std::cout << "   " << line << "\n";

    std::cout << "   built in " << juce::roundToInt (buildMs) << " ms, latency " << engine.latencySamples()
              << " samples, output level " << juce::Decibels::gainToDecibels (engine.makeup) << " dB\n"
              << "   " << seconds << " s of audio in " << ms / 1000.0 << " s = "
              << 100.0 * ms / 1000.0 / seconds << "% of one CPU core\n";

    juce::AudioBuffer<float> output (1, (int) x.size());

    for (size_t i = 0; i < x.size(); ++i)
        output.setSample (0, (int) i, (float) x[i]);

    const juce::File outFile (args[3]);
    outFile.deleteFile();
    juce::WavAudioFormat wav;
    std::unique_ptr<juce::OutputStream> stream = std::make_unique<juce::FileOutputStream> (outFile);
    auto writer = wav.createWriterFor (stream, juce::AudioFormatWriterOptions{}.withSampleRate (reader->sampleRate)
                                                                              .withNumChannels (1)
                                                                              .withBitsPerSample (32)
                                                                              .withSampleFormat (juce::AudioFormatWriterOptions::SampleFormat::floatingPoint));

    if (writer == nullptr)
        throw std::runtime_error ("can not write " + args[3].toStdString());

    writer->writeFromAudioSampleBuffer (output, 0, output.getNumSamples());
    return 0;
}
}

int main (int argc, char* argv[])
{
    juce::StringArray args;

    for (int i = 1; i < argc; ++i)
        args.add (argv[i]);

    std::cout << std::unitbuf;
    const juce::File root = args.size() > 1 && args[0] != "render" ? juce::File (juce::File::getCurrentWorkingDirectory().getChildFile (args[1]))
                                                                   : juce::File::getCurrentWorkingDirectory();

    try
    {
        if (args.size() >= 1 && args[0] == "check")
        {
            toneStackChecks();
            triodeChecks();
            feedbackChecks();
            couplingChecks();
            powerChecks();
            settingsChecks (root);
            referenceChecks (root);
            std::cout << (failures == 0 ? "All passed\n" : juce::String (failures) + " failed\n");
            return failures == 0 ? 0 : 1;
        }

        if (args.size() >= 1 && args[0] == "record")
            return record (root);

        if (args.size() >= 1 && args[0] == "make-audio")
            return makeAudio (root);

        if (args.size() >= 4 && args[0] == "render")
            return render (args);
    }
    catch (const std::exception& e)
    {
        std::cerr << "error: " << e.what() << "\n";
        return 1;
    }

    std::cerr << "usage: EngineTest check|record|make-audio [repo folder]\n"
                 "       EngineTest render <preset.json> <in.wav> <out.wav> [factorLog2] [blockSize]\n";
    return 2;
}
