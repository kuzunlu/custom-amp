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
#include "../Source/dsp/Bow.h"
#include "../Source/dsp/Strings.h"

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

void preampSupplyChecks()
{
    std::cout << "Preamp on the power supply: follows it through a 100 ms filter, and clips lower by that share\n";
    const double rate = 96000.0;
    const int block = 96;  // 1 ms
    auto* preset = new juce::DynamicObject();
    // v1 DC-coupled: a coupling capacitor would tilt the clipped wave, and its peak would no longer be the clip level.
    preset->setProperty ("chain", juce::JSON::parse (R"([ { "type": "gain", "id": "v1", "curve": "hard", "level_db": 32.0412, "coupling_nf": 0 },
        { "type": "power", "id": "pa", "tube": "EL34", "plate_v": 450, "bias_v": -45, "raa_ohms": 3400, "supply_ohms": 250,
          "supply_uf": 47, "preamp_targets": [ "v1" ], "preamp_filter_ms": 100 } ])"));
    amp::Amp a (juce::var (preset), rate, rate, juce::File(), juce::File ("C:/no/such/folder/ir.wav"));
    a.prepare (block, block);
    amp::Gain* v1 = nullptr;
    amp::Power* pa = nullptr;

    for (auto* b : amp::flatten (a.core))
    {
        v1 = v1 != nullptr ? v1 : dynamic_cast<amp::Gain*> (b);
        pa = pa != nullptr ? pa : dynamic_cast<amp::Power*> (b);
    }

    // 0.5 s of a loud 110 Hz note (v1 clips at +-1 x 40 = 40 V into the pair), then 0.6 s of silence.
    auto x = sine (110, 2.0, 1.1, rate);
    std::fill (x.begin() + (int) (0.5 * rate), x.end(), 0.0);
    std::vector<float> v1Out;
    v1->probe = &v1Out;
    std::vector<double> supply, preamp;  // at the end of every 1 ms

    for (size_t i = 0; i < x.size(); i += block)
    {
        a.processCore (x.data() + i, block);
        supply.push_back (pa->supplyRatio());
        preamp.push_back (pa->preampHeadroom);
    }

    // At the end of the loud part (5 time constants in) it equals the supply's average over the last 100 ms.
    double average = 0.0;

    for (int ms = 400; ms < 500; ++ms)
        average += supply[(size_t) ms] / 100;

    check (std::abs (preamp[499] - average) < 0.003 && preamp[499] < 0.97,
           "after 0.5 s loud: preamp supply " + juce::String (preamp[499], 4) + ", the main supply's average " + juce::String (average, 4));

    // v1 clips at +-h: its peak over the last 100 ms of the loud part is 40 V x its supply share.
    float peak = 0.0f;

    for (size_t i = (size_t) (0.4 * rate); i < (size_t) (0.5 * rate); ++i)
        peak = std::max (peak, std::abs (v1Out[i]));

    check (std::abs (peak / 40.0 - preamp[449]) < 0.01, "v1 clips lower by that share: peak " + juce::String (peak, 2) + " V = 40 V x "
                                                            + juce::String (peak / 40.0, 4));

    // After the supply is back (50 ms after the note: its own recovery is about 9 ms), the preamp's drop shrinks to
    // e^-1 in 100 ms.
    const double before = 1 - preamp[549], after = 1 - preamp[649], ratio = after / before;
    check (supply[549] > 0.999 && std::abs (ratio - std::exp (-1.0)) < 0.05 * std::exp (-1.0),
           "it recovers through the 100 ms filter: " + juce::String (ratio, 3) + " of its drop is left after 100 ms (theory e^-1 = 0.368)");
}

void biasShiftChecks()
{
    std::cout << "Bias shift recovers through the coupling capacitor: C x R\n";
    const double rate = 96000.0;

    for (auto [settings, theoryMs, what] : { std::tuple { R"("coupling_nf": 10, "grid_ohms": 1000000)", 10.0, "10 nF into 1 Mohm" },
                                            std::tuple { "", 22.0, "the default 22 nF into 1 Mohm" },
                                            std::tuple { R"("coupling_nf": 10, "shift_release_ms": 5)", 5.0, "shift_release_ms 5 (overrides C x R)" },
                                            std::tuple { R"("coupling_nf": 0)", 50.0, "DC-coupled (no capacitor: 50 ms)" } })
    {
        auto* preset = new juce::DynamicObject();
        const juce::String extra (settings);
        preset->setProperty ("chain", juce::JSON::parse (R"([ { "type": "gain", "curve": "hard", "bias_shift": 1)"
                                                         + (extra.isNotEmpty() ? ", " + extra : juce::String()) + " } ]"));
        amp::Amp a (juce::var (preset), rate, rate, juce::File(), juce::File ("C:/no/such/folder/ir.wav"));
        a.prepare (1, 1);
        amp::Gain* g = dynamic_cast<amp::Gain*> (amp::flatten (a.core)[0]);

        // A hard 0.2 s burst charges the capacitor; then silence: the shift must fall to e^-1 in one time constant.
        auto x = sine (220, 3.0, 0.6, rate);
        std::fill (x.begin() + (int) (0.2 * rate), x.end(), 0.0);
        std::vector<double> shift;

        for (size_t i = 0; i < x.size(); ++i)
        {
            a.processCore (x.data() + i, 1);
            shift.push_back (g->biasShift());
        }

        const size_t from = (size_t) (0.201 * rate), to = from + (size_t) (theoryMs * 0.001 * rate);  // 1 ms after the burst
        const double ratio = shift[to] / shift[from];
        check (shift[from] < -0.01 && std::abs (ratio - std::exp (-1.0)) < 0.002,
               juce::String (what) + ": after " + juce::String (theoryMs, 0) + " ms " + juce::String (ratio, 4) + " of the shift is left (theory e^-1 = 0.3679)");
    }
}

void transformerChecks()
{
    std::cout << "Output transformer: a 1st-order bass roll-off, and a core that saturates on loud low notes\n";
    const double rate = 96000.0;
    const auto pair = [rate] (double satHz, double freq, double level)
    {
        const auto chain = R"([ { "type": "power", "tube": "EL34", "plate_v": 450, "bias_v": -45, "raa_ohms": 3400, "coupling_nf": 0, "transformer_sat_hz": )"
                           + juce::String (satHz) + " } ]";
        return runJson (chain, sine (freq, level, 1.0, rate), rate);
    };
    const auto db = [] (double a, double b) { return 20 * std::log10 (a / b); };

    // A small signal (the pair is almost straight): 10 Hz against 1 kHz = the 10 Hz corner of a 1st-order high-pass.
    const double corner = db (tone (pair (0, 10, 1.0), 10, rate), tone (pair (0, 1000, 1.0), 1000, rate));
    check (std::abs (corner + 3.0103) < 0.02, "small signal at the 10 Hz corner: " + juce::String (corner, 3) + " dB (theory -3.010 dB)");

    // The pair below clipping (20 V), a core that saturates at 90 Hz: a 40 Hz note at that level fills it. A 400 Hz
    // note at the same level and a quiet 40 Hz note do not change; the loud 40 Hz note loses bass, gains odd harmonics.
    // (With the pair clipping hard as well, their 3rd harmonics partly cancel: 40 V at 40 Hz, core at 40 Hz: 7.9% -> 3.0%.
    // Clipping flattens the peaks, the core bends the wave a quarter cycle later, around the zero crossings.)
    const auto high = db (tone (pair (90, 400, 20.0), 400, rate), tone (pair (0, 400, 20.0), 400, rate));
    const auto quiet = db (tone (pair (90, 40, 2.0), 40, rate), tone (pair (0, 40, 2.0), 40, rate));
    const auto loudSat = pair (90, 40, 20.0), loud = pair (0, 40, 20.0);
    const auto low = db (tone (loudSat, 40, rate), tone (loud, 40, rate));
    const double third = harmonicPercent (loudSat, 40, 3, rate), thirdBefore = harmonicPercent (loud, 40, 3, rate);
    check (std::abs (high) < 0.05 && std::abs (quiet) < 0.1 && low < -0.5 && third > thirdBefore + 5,
           "core full at 40 Hz: 400 Hz " + juce::String (high, 3) + " dB, quiet 40 Hz " + juce::String (quiet, 3)
               + " dB, 40 Hz " + juce::String (low, 2) + " dB with its 3rd harmonic " + juce::String (thirdBefore, 1) + "% -> "
               + juce::String (third, 1) + "%");
}

// A slowly dying note (harmonics fall faster), like a plucked string: 0.3 V peak.
std::vector<double> dyingNote (double freq, double seconds, double rate)
{
    std::vector<double> x ((size_t) (seconds * rate), 0.0);
    double peak = 0.0;

    for (size_t i = 0; i < x.size(); ++i)
    {
        const double t = (double) i / rate;

        for (int k = 1; k <= 8; ++k)
            x[i] += std::sin (2 * pi * freq * k * t) / k * std::exp (-t * (0.6 + 0.4 * k));

        peak = std::max (peak, std::abs (x[i]));
    }

    for (auto& v : x)
        v *= 0.3 / peak;

    return x;
}

void roomChecks (const juce::File& root)
{
    std::cout << "Room (acoustic feedback)\n";
    const double rate = 48000.0;
    amp::BuildContext ctx;
    ctx.rate = ctx.checkRate = rate;

    // The pitch tracker finds the note.
    for (double f : { 82.41, 110.0, 329.63 })
    {
        amp::Room room (amp::Cfg (juce::JSON::parse ("{ \"type\": \"room\" }"), "room"), ctx);
        auto x = sine (f, 0.3, 0.5, rate);

        for (size_t i = 0; i < x.size(); i += (size_t) room.maxChunk())
        {
            const int n = (int) std::min ((size_t) room.maxChunk(), x.size() - i);
            room.addFeedback (x.data() + i, n);
            std::vector<double> silence ((size_t) n, 0.0);
            room.hear (silence.data(), n);
        }

        check (std::abs (room.noteHz() / f - 1) < 0.005, "the pitch tracker finds " + juce::String (f, 2) + " Hz: " + juce::String (room.noteHz(), 2) + " Hz");
    }

    // The sound reaches the string after distance / 343 m/s, pushing it by volume / distance.
    {
        amp::Room room (amp::Cfg (juce::JSON::parse (R"({ "type": "room", "distance_m": 0.686, "volume_db": -20 })"), "room"), ctx);
        std::vector<float> pushes;
        auto x = sine (110, 0.3, 1.0, rate);
        const size_t click = (size_t) (0.5 * rate);

        for (size_t i = 0; i < x.size(); i += (size_t) room.maxChunk())
        {
            const int n = (int) std::min ((size_t) room.maxChunk(), x.size() - i);

            if (i == click)
                room.probe = &pushes;  // from the click on

            room.addFeedback (x.data() + i, n);
            std::vector<double> out ((size_t) n, 0.0);

            if (i == click)
                out[0] = 1.0;  // a click leaves the speaker

            room.hear (out.data(), n);
        }

        size_t first = 0;

        while (first < pushes.size() && std::abs (pushes[first]) < 1e-9f)
            ++first;

        const double expected = 0.686 / 343.0 * rate;
        check (first < pushes.size() && (double) first == std::round (expected) && std::abs (pushes[first] - 0.1 / 0.686) < 1e-6,
               "a click reaches the string after " + juce::String ((int) first) + " samples (0.686 m / 343 m/s = " + juce::String (expected, 1)
                   + "), pushing it by " + juce::String (first < pushes.size() ? pushes[first] : 0.0f, 4) + " (-20 dB / 0.686 m = 0.1458)");
    }

    // The whole loop, the clean preset with a room at -20 dB: a dying note sustains, in tune, until it is muted.
    const auto render = [&root, rate] (bool withRoom, double sustain = 3.0)
    {
        auto preset = loadJson (root.getChildFile ("presets/clean.json"));
        preset.getDynamicObject()->removeProperty ("panel");

        if (withRoom)
            preset["chain"].getArray()->add (juce::JSON::parse (R"({ "type": "room", "distance_m": 0.5, "volume_db": -20, "sustain_s": )"
                                                                + juce::String (sustain) + " }"));

        amp::Engine::Options options;
        options.preset = preset;
        options.presetFolder = root.getChildFile ("presets");
        options.irOverride = juce::File ("C:/no/such/folder/ir.wav");
        options.sampleRate = rate;
        options.maxBlock = 256;
        amp::Engine engine (options);
        auto x = dyingNote (110, 5.5, rate);
        std::fill (x.begin() + (int) (4.0 * rate), x.end(), 0.0);  // a hand on the strings at 4 s

        for (size_t i = 0; i < x.size(); i += 256)
            engine.process (x.data() + i, (int) std::min ((size_t) 256, x.size() - i));

        return x;
    };
    const auto level = [rate] (const std::vector<double>& y, double from, double to)
    {
        double sum = 0.0;

        for (size_t i = (size_t) (from * rate); i < (size_t) (to * rate); ++i)
            sum += y[i] * y[i];

        return 10 * std::log10 (sum / ((to - from) * rate) + 1e-30);
    };
    const auto dry = render (false), wet = render (true);
    const double dryFall = level (dry, 3.4, 3.6) - level (dry, 0.9, 1.1), wetFall = level (wet, 3.4, 3.6) - level (wet, 0.9, 1.1);
    const double afterMute = level (wet, 4.3, 4.5) - level (wet, 3.4, 3.6);

    // Its pitch: the strongest frequency near the note while it sustains (0.01 Hz steps). The rest of the loop (amp,
    // cab, air) adds phase, so the loop settles a little beside the string's resonance peak. A string that rings longer
    // has a sharper resonance (half-power width (1 - g) f / pi, g = what it keeps per period), so it is pulled less.
    // (Measured: 3 s string +0.68 Hz, width 0.73; 10 s string +0.29 Hz, width 0.22: the pull is not bounded by the
    // width, which only holds while the loop's extra phase stays under 45 degrees.)
    const auto pitch = [rate] (const std::vector<double>& y)
    {
        std::vector<double> held (y.begin() + (int) (2.0 * rate), y.begin() + (int) (4.0 * rate));
        double best = 0.0, bestHz = 0.0;

        for (double f = 108.0; f <= 112.0; f += 0.01)
            if (const double a = tone (held, f, rate); a > best)
            {
                best = a;
                bestHz = f;
            }

        return bestHz;
    };
    const auto width = [rate] (double sustain) { return (1 - std::pow (10.0, -3.0 / (110.0 * sustain))) * 110.0 / pi; };
    const double pulled3 = pitch (wet) - 110.0, pulled10 = pitch (render (true, 10.0)) - 110.0;

    check (dryFall < -15 && std::abs (wetFall) < 3, "a dying note sustains: from 1 s to 3.5 s " + juce::String (dryFall, 1) + " dB without the room, "
                                                        + juce::String (wetFall, 1) + " dB with it");
    check (std::abs (pulled3) < 1.1 && std::abs (pulled10) < std::abs (pulled3),
           "the feedback stays near the note (under 1%), and a longer-ringing string is pulled less: " + juce::String (pulled3, 2)
               + " Hz with a 3 s string (resonance width +-" + juce::String (width (3.0), 2) + "), " + juce::String (pulled10, 2)
               + " Hz with a 10 s string (+-" + juce::String (width (10.0), 2) + ")");
    check (afterMute < -100, "a hand on the strings stops it: " + juce::String (afterMute, 0) + " dB 0.3 s after");

    // Bloom to the octave (harmonic 2, the string touched at its middle): at 0.4 m on the clean preset it only blooms
    // when find_spot moves the sound's arrival into step; and the same playing gives the same result every time.
    const auto bloom = [&root, rate] (bool spot)
    {
        auto preset = loadJson (root.getChildFile ("presets/clean.json"));
        preset.getDynamicObject()->removeProperty ("panel");
        preset["chain"].getArray()->add (juce::JSON::parse (juce::String (R"({ "type": "room", "distance_m": 0.4, "volume_db": -20, "harmonic": 2, "find_spot": )")
                                                            + (spot ? "true" : "false") + " }"));
        amp::Engine::Options options;
        options.preset = preset;
        options.presetFolder = root.getChildFile ("presets");
        options.irOverride = juce::File ("C:/no/such/folder/ir.wav");
        options.sampleRate = rate;
        options.maxBlock = 256;
        amp::Engine engine (options);
        auto x = dyingNote (110, 4.0, rate);

        for (size_t i = 0; i < x.size(); i += 256)
            engine.process (x.data() + i, (int) std::min ((size_t) 256, x.size() - i));

        return x;
    };
    const auto octaveOverNote = [rate] (const std::vector<double>& y)
    {
        const std::vector<double> late (y.begin() + (int) (2.75 * rate), y.begin() + (int) (3.25 * rate));
        return 20 * std::log10 (tone (late, 220.0, rate) / tone (late, 110.0, rate));
    };
    const auto fixedSpot = bloom (false), spot = bloom (true), again = bloom (true);
    check (octaveOverNote (fixedSpot) < 0 && octaveOverNote (spot) > 10 && spot == again,
           "bloom to the octave at 0.4 m: at 3 s the octave is " + juce::String (octaveOverNote (fixedSpot), 1) + " dB against the note without "
               + "find_spot, " + juce::String (octaveOverNote (spot), 1) + " dB with it; two runs are the same sample for sample");
}

void linearTwinChecks()
{
    std::cout << "Linear twins (what X-ray listening subtracts)\n";
    const double rate = 384000.0;
    const juce::String triode = R"({ "type": "gain", "curve": "triode", "plate_volts": true)";
    const juce::String pair = R"({ "type": "power", "tube": "EL34", "plate_v": 450, "bias_v": -45, "transformer_sat_hz": 40)";
    const auto run = [rate] (const juce::String& block, bool linear, double level)
    {
        return runJson ("[ " + block + (linear ? ", \"linear\": true }" : " }") + " ]", sine (220, level, 1.0, rate), rate);
    };
    const auto quiet = [&] (const juce::String& block, double level)
    {
        return std::abs (tone (run (block, true, level), 220, rate) / tone (run (block, false, level), 220, rate) - 1);
    };
    const double triodeQuiet = quiet (triode, 0.01), pairQuiet = quiet (pair, 0.1);
    const double loud = harmonicPercent (run (triode, false, 0.5), 220, 2, rate), loudTwin = harmonicPercent (run (triode, true, 0.5), 220, 2, rate);
    check (triodeQuiet < 0.001 && pairQuiet < 0.001 && std::abs (loud - 2.64) < 0.05 && loudTwin < 1e-6,
           "quiet playing: the twin is " + juce::String (100 * triodeQuiet, 3) + "% (12AX7) and " + juce::String (100 * pairQuiet, 3)
               + "% (EL34 pair) from the real stage; at 0.5 V the triode makes " + juce::String (loud, 2) + "% 2nd harmonic, its twin "
               + juce::String (loudTwin, 6) + "%");
}

void stringsChecks()
{
    std::cout << "Sympathetic strings\n";
    bool namesOk = true;

    for (auto [name, hz] : { std::pair { "A4", 440.0 }, std::pair { "C4", 261.6256 }, std::pair { "F#4", 369.9944 }, std::pair { "Bb2", 116.5409 } })
        namesOk = namesOk && std::abs (amp::SympatheticStrings::noteHz (name) - hz) < 0.001;

    int refused = 0;

    for (auto* bad : { "H2", "A", "E#x", "" })
    {
        try { amp::SympatheticStrings::noteHz (bad); }
        catch (const amp::PresetError&) { ++refused; }
    }

    check (namesOk && refused == 4, "note names: A4 440, C4 261.63, F#4 369.99, Bb2 116.54 Hz; H2, A, E#x and an empty name are refused");

    // One string at A2, struck by a click: it rings at its note and falls 60 dB in sustain_s.
    const double rate = 96000.0;
    const juce::String a2 = R"([ { "type": "strings", "notes": [ "A2" ], "sustain_s": 4 } ])";
    std::vector<double> click ((size_t) (4.0 * rate), 0.0);
    click[0] = 1.0;
    auto rung = runJson (a2, click, rate);
    rung[0] -= 1.0;  // the strings' own sound: the output without the click that came in
    const auto window = [&rung, rate] (double from, double to) { return std::vector<double> (rung.begin() + (int) (from * rate), rung.begin() + (int) (to * rate)); };
    double best = 0.0, bestHz = 0.0;
    const auto held = window (0.5, 2.5);

    for (double f = 108.0; f <= 112.0; f += 0.005)
        if (const double a = tone (held, f, rate); a > best)
        {
            best = a;
            bestHz = f;
        }

    const double fall = 20 * std::log10 (tone (window (3.0, 3.5), 110.0, rate) / tone (window (1.0, 1.5), 110.0, rate));
    check (std::abs (bestHz - 110.0) < 0.05 && std::abs (fall + 30.0) < 1.5,
           "an A2 string rings at " + juce::String (bestHz, 3) + " Hz (110) and falls " + juce::String (fall, 1) + " dB in 2 s (60 dB in 4 s: -30)");

    // It only takes up its own note: played at A2 it answers far louder than a semitone higher (A#2).
    const auto answer = [&a2, rate] (double f)
    {
        const auto x = sine (f, 0.1, 1.0, rate);
        auto y = runJson (a2, x, rate);
        double sum = 0.0;

        for (size_t i = (size_t) (0.8 * rate); i < x.size(); ++i)
            sum += (y[i] - x[i]) * (y[i] - x[i]);

        return 10 * std::log10 (sum);
    };
    const double fits = answer (110.0), misses = answer (116.5409);
    check (fits - misses > 20, "played at its note it answers " + juce::String (fits - misses, 1) + " dB louder than a semitone higher");

    // After the note stops it rings on: -7.5 dB after 0.5 s (60 dB in 4 s).
    auto note = sine (110.0, 0.1, 2.0, rate);
    std::fill (note.begin() + (int) (1.0 * rate), note.end(), 0.0);
    const auto out = runJson (a2, note, rate);
    const auto level = [&out, rate] (double t) { return tone (std::vector<double> (out.begin() + (int) (t * rate), out.begin() + (int) ((t + 0.1) * rate)), 110.0, rate); };
    const double after = 20 * std::log10 (level (1.5) / level (1.0));
    check (std::abs (after + 7.5) < 1.0, "after the note stops it rings on: " + juce::String (after, 1) + " dB in 0.5 s (theory -7.5)");
}

// The frequency of the strongest peak between lo and hi Hz in y[from..to) s (Hann window, then a golden-section search).
double peakHz (const std::vector<double>& y, double from, double to, double lo, double hi, double rate)
{
    const auto a = (size_t) (from * rate), b = std::min (y.size(), (size_t) (to * rate));
    const auto size = [&] (double f)
    {
        std::complex<double> sum;

        for (size_t i = a; i < b; ++i)
            sum += y[i] * (0.5 - 0.5 * std::cos (2 * pi * (double) (i - a) / (double) (b - a))) * std::polar (1.0, -2 * pi * f * (double) i / rate);

        return std::abs (sum);
    };
    double best = lo, bestSize = -1;

    for (int k = 0; k <= 200; ++k)
        if (const double f = lo + (hi - lo) * k / 200, m = size (f); m > bestSize)
        {
            bestSize = m;
            best = f;
        }

    double l = best - (hi - lo) / 200, h = best + (hi - lo) / 200;
    const double golden = (std::sqrt (5.0) - 1) / 2;

    for (int k = 0; k < 40; ++k)
    {
        const double c = h - golden * (h - l), d = l + golden * (h - l);

        if (size (c) > size (d))
            h = d;
        else
            l = c;
    }

    return 0.5 * (l + h);
}

// The amplitude at f in y[from..to) s (Hann window).
double levelAt (const std::vector<double>& y, double f, double from, double to, double rate)
{
    const auto a = (size_t) (from * rate), b = std::min (y.size(), (size_t) (to * rate));
    std::complex<double> sum;
    double weights = 0;

    for (size_t i = a; i < b; ++i)
    {
        const double w = 0.5 - 0.5 * std::cos (2 * pi * (double) (i - a) / (double) (b - a));
        sum += y[i] * w * std::polar (1.0, -2 * pi * f * (double) i / rate);
        weights += w;
    }

    return 2 * std::abs (sum) / weights;
}

double cents (double f, double reference) { return 1200 * std::log2 (f / reference); }

// A plucked note: harmonics with these amplitudes, dying away in `decay` s (to 1/e).
std::vector<double> plucked (double f, std::initializer_list<double> harmonics, double seconds, double decay, double rate, double peak = 0.3)
{
    std::vector<double> x ((size_t) (seconds * rate));

    for (size_t i = 0; i < x.size(); ++i)
    {
        const double t = (double) i / rate;
        double v = 0;
        int k = 1;

        for (double h : harmonics)
            v += h * std::sin (2 * pi * k++ * f * t);

        x[i] = peak * std::exp (-t / decay) * v;
    }

    return x;
}

void restringChecks()
{
    std::cout << "Restring (a virtual string played by the DI)\n";
    const double rate = 96000.0;
    const auto run = [rate] (const juce::String& settings, const std::vector<double>& x)
    {
        return runJson ("[ { \"type\": \"restring\"" + settings + " } ]", x, rate);
    };
    const auto start = [] (const std::vector<double>& y)
    {
        size_t i = 0;

        while (i < y.size() && std::abs (y[i]) < 1e-9)
            ++i;

        return i;
    };

    // Plucked at 110 Hz: the new string starts some ms after the pick and rings at the note, its harmonics in tune.
    const std::initializer_list<double> rich { 1, 0.5, 0.33, 0.25, 0.2, 0.17, 0.14, 0.12, 0.11, 0.1 };
    const auto note = plucked (110, rich, 3.0, 1.0, rate);
    const auto same = run ("", note);
    const double latency = 1000.0 * (double) start (same) / rate;
    const double f1 = peakHz (same, 0.5, 2.5, 105, 115, rate);
    double worst = 0;

    for (int n = 2; n <= 10; ++n)
        worst = std::max (worst, std::abs (cents (peakHz (same, 0.5, 2.5, n * f1 - 5, n * f1 + 5, rate), n * f1)));

    juce::String latencies;
    double slowest = 0;

    for (double f : { 82.41, 110.0, 329.63 })
    {
        const double ms = f == 110.0 ? latency : 1000.0 * (double) start (run ("", plucked (f, rich, 0.3, 1.0, rate))) / rate;
        slowest = std::max (slowest, ms);
        latencies << juce::String (ms, 1) << " ";
    }

    check (slowest < 35, "a pick attack plucks the new string after " + latencies + "ms (82, 110, 330 Hz: the tracker finds the note twice)");
    check (std::abs (cents (f1, 110)) < 0.5 && worst < 2, "it rings at " + juce::String (f1, 3) + " Hz (110), harmonics 2-10 within "
                                                              + juce::String (worst, 2) + " cents of n x the note");

    for (auto [semis, hz] : { std::pair { 7.0, 164.8138 }, std::pair { -12.0, 55.0 }, std::pair { 24.0, 440.0 } })
    {
        const auto y = run (", \"transpose\": " + juce::String (semis), note);
        const double f = peakHz (y, 0.5, 2.5, hz * 0.97, hz * 1.03, rate);
        check (std::abs (cents (f, hz)) < 0.5, "transpose " + juce::String (semis, 0) + ": " + juce::String (f, 3) + " Hz (" + juce::String (hz, 2) + ")");
    }

    // Stiff: the 10th harmonic stretch_cents sharp, the others as a stiff string's sqrt(1 + B n^2).
    for (double c : { 5.0, 20.0, 50.0 })
    {
        const auto y = run (", \"stretch_cents\": " + juce::String (c), note);
        const double r2 = std::pow (2.0, 2 * c / 1200), B = (r2 - 1) / (100 - r2);
        const double g1 = peakHz (y, 0.5, 2.5, 105, 115, rate);
        double off = 0, tenth = 0;

        for (int n = 2; n <= 10; ++n)
        {
            const double stretch = std::sqrt ((1 + B * n * n) / (1 + B)), want = n * g1 * stretch;
            const double got = peakHz (y, 0.5, 2.5, want - 6, want + 6, rate);
            off = std::max (off, std::abs (cents (got, want)));
            tenth = n == 10 ? cents (got, 10 * g1) : tenth;
        }

        check (std::abs (cents (g1, 110)) < 0.5 && off < 0.3 * c + 1,
               "stretch " + juce::String (c, 0) + " cents: note " + juce::String (g1, 3) + " Hz, 10th harmonic " + juce::String (tenth, 1)
                   + " cents sharp, harmonics 2-10 within " + juce::String (off, 2) + " cents of a stiff string's (B = " + juce::String (B, 6) + ")");
    }

    // Sustain 20 s: the note falls 1.5 dB in 0.5 s while the guitar's note fades much faster (4.3 dB); harmonics at
    // string_tone_hz (here the 10th, 1100 Hz) fall 10 times as fast (15 dB).
    {
        const auto y = run (", \"sustain_s\": 20, \"string_tone_hz\": 1100", note);
        const double fall1 = 20 * std::log10 (levelAt (y, 110, 1.0, 1.1, rate) / levelAt (y, 110, 0.5, 0.6, rate));
        const double fall10 = 20 * std::log10 (levelAt (y, 1100, 1.0, 1.1, rate) / levelAt (y, 1100, 0.5, 0.6, rate));
        check (std::abs (fall1 + 1.5) < 0.2 && std::abs (fall10 + 15) < 1,
               "sustain 20 s: the note falls " + juce::String (fall1, 2) + " dB in 0.5 s (-1.5), its 10th harmonic at string_tone_hz "
                   + juce::String (fall10, 1) + " dB (-15)");
    }

    // The pluck bend: 1 semitone gliding back in 0.5 s: 100 e^(-t / 0.5) cents sharp t after the pluck.
    {
        const auto y = run (", \"bend_semitones\": 1, \"bend_s\": 0.5, \"sustain_s\": 20", note);
        const double t0 = (double) start (y) / rate;
        juce::String got;
        double off = 0;

        for (double t : { 0.25, 0.5, 1.0 })
        {
            const double c = cents (peakHz (y, t0 + t - 0.03, t0 + t + 0.03, 100, 125, rate), 110);
            off = std::max (off, std::abs (c - 100 * std::exp (-t / 0.5)));
            got << juce::String (c, 1) << " ";
        }

        check (off < 3, "pluck bend 1 semitone, 0.5 s: " + got + "cents sharp after 0.25 / 0.5 / 1 s (60.7 36.8 13.5)");
    }

    // It follows the note while it rings: 110 Hz going on into 123.47 Hz (B2) with no new attack (a hammer-on).
    {
        std::vector<double> x ((size_t) (2.0 * rate));
        double phase = 0;

        for (size_t i = 0; i < x.size(); ++i)
        {
            phase += 2 * pi * (i < x.size() / 2 ? 110.0 : 123.4708) / rate;
            x[i] = 0.3 * std::sin (phase);
        }

        const auto y = run ("", x);
        const double before = peakHz (y, 0.5, 0.95, 100, 130, rate), after = peakHz (y, 1.3, 1.95, 100, 130, rate);
        check (std::abs (cents (before, 110)) < 1 && std::abs (cents (after, 123.4708)) < 1,
               "a hammer-on: it follows from " + juce::String (before, 2) + " to " + juce::String (after, 2) + " Hz (110, 123.47)");
    }

    // A new pick attack, a new note: a new string at D3, and the old one is stopped by the pick.
    {
        auto x = plucked (110, rich, 2.0, 1.0, rate, 0.1);
        const auto second = plucked (146.8324, rich, 1.0, 1.0, rate, 0.3);
        std::copy (second.begin(), second.end(), x.begin() + (int) rate);
        const auto y = run (", \"sustain_s\": 20", x);
        const double f = peakHz (y, 1.3, 1.95, 140, 155, rate);
        const double old = 20 * std::log10 (levelAt (y, 110, 1.4, 1.9, rate) / levelAt (y, 110, 0.4, 0.9, rate));
        check (std::abs (cents (f, 146.8324)) < 0.5 && old < -60,
               "a new pluck at D3: " + juce::String (f, 3) + " Hz (146.83), the old string " + juce::String (old, 0) + " dB");
    }

    // A hand on the strings stops it: the input stops dead, the string is gone 0.35 s later. A note that only
    // fades rings on (see sustain).
    {
        auto x = plucked (110, rich, 1.0, 1.0, rate);
        std::fill (x.begin() + (int) (0.5 * rate), x.end(), 0.0);
        const auto y = run (", \"sustain_s\": 20", x);
        double rest = 0;

        for (size_t i = (size_t) (0.85 * rate); i < y.size(); ++i)
            rest = std::max (rest, std::abs (y[i]));

        const double ringing = levelAt (y, 110, 0.3, 0.45, rate);
        check (rest < 1e-12 * ringing, "a hand on the strings stops it: " + juce::String (20 * std::log10 (std::max (rest, 1e-300) / ringing), 0)
                                           + " dB 0.35 s after");
    }

    // The pluck carries the DI's own harmonics over (where it was picked): a DI without a 3rd harmonic gives a string
    // without one, 5 semitones up; the 2nd stays 6 dB under the note.
    {
        const auto x = plucked (110, { 1, 0.5, 0, 0.25, 0.2 }, 1.5, 1.0, rate);
        const auto y = run (", \"transpose\": 5, \"sustain_s\": 60, \"string_tone_hz\": 10000", x);
        const double t0 = (double) start (y) / rate, f = 110 * std::pow (2.0, 5 / 12.0);
        const auto h = [&] (int k) { return 20 * std::log10 (levelAt (y, k * f, t0 + 0.02, t0 + 0.2, rate) / levelAt (y, f, t0 + 0.02, t0 + 0.2, rate)); };
        check (std::abs (h (2) + 6.02) < 0.5 && h (3) < -40 && std::abs (h (4) + 12.04) < 0.5,
               "the DI's own harmonics carry over: 2nd " + juce::String (h (2), 2) + " dB (-6.02), 3rd " + juce::String (h (3), 0)
                   + " dB (none), 4th " + juce::String (h (4), 2) + " dB (-12.04)");
    }

    // string_tone_hz under about 3 x the note asks for more than a 1-pole can do without gaining energy at 0 Hz (it
    // did: the loop grew without end). Then sustain_s wins: a 60 s string falls 1.0 dB per second and nothing grows.
    {
        const auto y = run (", \"sustain_s\": 60, \"string_tone_hz\": 200", note);
        const double fall = 20 * std::log10 (levelAt (y, 110, 2.0, 2.5, rate) / levelAt (y, 110, 1.0, 1.5, rate));
        double early = 0, late = 0;

        for (size_t i = (size_t) (0.5 * rate); i < y.size(); ++i)
            (i < (size_t) (1.0 * rate) ? early : late) = std::max (i < (size_t) (1.0 * rate) ? early : late, std::abs (y[i]));

        check (std::abs (fall + 1.0) < 0.1 && late < early, "string_tone_hz 200 at a 110 Hz note: the note falls " + juce::String (fall, 2)
                                                                + " dB per s (sustain 60 s: -1.0), the last 2 s peak " + juce::String (late / early, 3) + " x the 0.5-1 s peak");
    }
}

// Runs one bow block on x and gives its output; `counts` gets its stick/slip counters over the second half.
std::vector<double> bowRun (const juce::String& settings, std::vector<double> x, double rate, std::array<long long, 3>* counts = nullptr)
{
    amp::BuildContext ctx;
    ctx.rate = ctx.checkRate = rate;
    amp::Bow b (amp::Cfg (juce::JSON::parse ("{ \"type\": \"bow\"" + settings + " }"), "bow"), ctx);
    b.prepare (4096);
    b.reset();
    const size_t half = x.size() / 2;
    std::array<long long, 3> at {};

    for (size_t i = 0; i < x.size(); i += 4096)
    {
        const size_t n = std::min ((size_t) 4096, x.size() - i);

        if (i <= half && half < i + n)
        {
            b.process (x.data() + i, (int) (half - i));
            at = { b.samples, b.stuckSamples, b.slips };
            b.process (x.data() + half, (int) (i + n - half));
            continue;
        }

        b.process (x.data() + i, (int) n);
    }

    if (counts != nullptr)
        *counts = { b.samples - at[0], b.stuckSamples - at[1], b.slips - at[2] };

    return x;
}

void bowChecks()
{
    std::cout << "Bow (a virtual string bowed by the DI)" << std::endl;
    const double rate = 96000.0;
    const auto steady = [rate] (double f, double volts, double seconds)
    {
        std::vector<double> x ((size_t) (seconds * rate));

        for (size_t i = 0; i < x.size(); ++i)
            x[i] = volts * std::sin (2 * pi * f * (double) i / rate);

        return x;
    };
    const auto note = steady (110, 0.5, 2.0);
    const auto motion = [&] (const juce::String& settings, double& stuck)
    {
        std::array<long long, 3> c {};
        bowRun (settings, note, rate, &c);
        stuck = (double) c[1] / (double) c[0];
        return (double) c[2] / ((double) c[0] / rate * 110);  // slips per period
    };

    // Schelleng (1973): one clean slip per period (Helmholtz motion) only between a least and a most bow force. The
    // most: 2 Z v / (bow position (mu_s - mu_d)) = 2 x 0.3 x 0.2 / (0.5 x bow position): 2.0 N at 0.12, 1.2 N at 0.2;
    // the least grows with the losses. In the middle of that window it is stuck for 1 - bow position of each period
    // (the corner needs bow position x the period to go from the bow to the bridge and back).
    double highest = 0;

    for (double beta : { 0.12, 0.2 })
    {
        const double most = 2 * 0.3 * 0.2 / (beta * 0.5);
        juce::String window;
        double lowest = 0, top = 0;

        for (double force = 0.1; force < 2 * most; force += 0.1)
        {
            double stuck = 0;

            if (std::abs (motion (", \"bow_position\": " + juce::String (beta) + ", \"bow_force_n\": " + juce::String (force, 1), stuck) - 1) < 0.02)
            {
                lowest = lowest == 0 ? force : lowest;
                top = force;
                window << juce::String (force, 1) << " ";
            }
        }

        double stuck = 0;
        const double middle = std::round ((lowest + top) / 2 * 10) / 10;
        const double slips = motion (", \"bow_position\": " + juce::String (beta) + ", \"bow_force_n\": " + juce::String (middle, 1), stuck);
        check (top < 1.2 * most && top > 0.7 * most && std::abs (slips - 1) < 0.02 && std::abs (stuck - (1 - beta)) < 0.01,
               "bow at " + juce::String (beta) + ": clean Helmholtz motion at " + window + "N (Schelleng: under " + juce::String (most, 1)
                   + " N); at " + juce::String (middle, 1) + " N stuck " + juce::String (stuck, 3) + " of each period (1 - bow position = "
                   + juce::String (1 - beta, 2) + ")");
        highest = beta == 0.12 ? top : highest;
    }

    // Too little force: the bow can not hold the string through a whole period and slips again (Schelleng's least
    // force: the "surface sound", whistling at the harmonics).
    double weakStuck = 0;
    const double weak = motion (", \"bow_force_n\": 0.5", weakStuck);
    check (weak > 1.5, "too little force (0.5 N): " + juce::String (weak, 2) + " slips per period (the surface sound)");

    // At the note; a little flat at high force (the friction holds on to the string a little too long each period).
    {
        const auto at = [&] (double force) { return cents (peakHz (bowRun (", \"bow_force_n\": " + juce::String (force), note, rate), 1.0, 2.0, 100, 120, rate), 110); };
        const double normal = at (1.5), hard = at (highest);
        check (std::abs (normal) < 3 && hard < normal, "it plays the note: " + juce::String (normal, 2) + " cents at 1.5 N, "
                                                           + juce::String (hard, 2) + " cents at " + juce::String (highest, 1) + " N (flatter when pressed harder)");
    }

    // The bow follows your level: half the level = half the bow speed and force = half the sound (Helmholtz: the
    // string's speed jumps by bow speed / bow position).
    {
        const auto loud = bowRun ("", note, rate), soft = bowRun ("", steady (110, 0.25, 2.0), rate);
        const double drop = 20 * std::log10 (levelAt (soft, 110, 1.0, 2.0, rate) / levelAt (loud, 110, 1.0, 2.0, rate));
        check (std::abs (drop + 6.02) < 0.3, "half the playing level: " + juce::String (drop, 2) + " dB (-6.02)");
    }

    // A plucked note that fades fast keeps being bowed: the bow slows down in release_s (2 s): -2.17 dB in 0.5 s.
    {
        auto x = plucked (110, { 1, 0.5, 0.3 }, 2.0, 0.1, rate, 0.5);
        const auto y = bowRun ("", x, rate);
        const double fall = 20 * std::log10 (levelAt (y, 110, 1.5, 1.6, rate) / levelAt (y, 110, 1.0, 1.1, rate));
        const double guitar = 20 * std::log10 (std::exp (-0.5 / 0.1));
        check (std::abs (fall + 2.17) < 0.3, "after a pluck it goes on: " + juce::String (fall, 2) + " dB in 0.5 s (bow release 2 s: -2.17; the guitar's note "
                                                 + juce::String (guitar, 1) + " dB)");
    }

    // A hand on the strings lifts the bow and stops the string.
    {
        auto x = note;
        std::fill (x.begin() + (int) (1.0 * rate), x.end(), 0.0);
        const auto y = bowRun ("", x, rate);
        double rest = 0;

        for (size_t i = (size_t) (1.35 * rate); i < y.size(); ++i)
            rest = std::max (rest, std::abs (y[i]));

        check (rest < 1e-12 * levelAt (y, 110, 0.5, 0.9, rate), "a hand on the strings stops it: "
                                                                    + juce::String (20 * std::log10 (std::max (rest, 1e-300) / levelAt (y, 110, 0.5, 0.9, rate)), 0) + " dB 0.35 s after");
    }

    // It follows a hammer-on (no new attack), and transpose moves the string.
    {
        std::vector<double> x ((size_t) (2.0 * rate));
        double phase = 0;

        for (size_t i = 0; i < x.size(); ++i)
        {
            phase += 2 * pi * (i < x.size() / 2 ? 110.0 : 123.4708) / rate;
            x[i] = 0.5 * std::sin (phase);
        }

        const auto y = bowRun ("", x, rate), low = bowRun (", \"transpose\": -12", note, rate);
        const double before = peakHz (y, 0.5, 0.95, 100, 130, rate), after = peakHz (y, 1.3, 1.95, 100, 130, rate), octave = peakHz (low, 1.0, 2.0, 50, 60, rate);
        check (std::abs (cents (before, 110)) < 3 && std::abs (cents (after, 123.4708)) < 3 && std::abs (cents (octave, 55)) < 1,
               "a hammer-on: " + juce::String (before, 2) + " -> " + juce::String (after, 2) + " Hz (110, 123.47); transpose -12: " + juce::String (octave, 2) + " Hz (55)");
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

    // Two pairs into half the load: each tube works as in one pair, so twice the power and the same sound; the supply
    // feeds twice the current, so it sags about twice as far (a bit less: a lower supply lowers the current too).
    {
        const auto one = amp::PushPull::make ("EL34", 450, 450, -38, 3400, 0.0, 1), two = amp::PushPull::make ("EL34", 450, 450, -38, 1700, 0.0, 2);
        const double fast = 96000.0;
        const auto run = [fast] (const juce::String& power, double& lowestSupply)
        {
            auto* preset = new juce::DynamicObject();
            preset->setProperty ("chain", juce::JSON::parse ("[ " + power + " ]"));
            amp::Amp a (juce::var (preset), fast, fast, juce::File(), juce::File ("C:/no/such/folder/ir.wav"));
            a.prepare (4096, 4096);
            auto x = sine (110, 30.0, 0.5, fast);
            lowestSupply = 1.0;
            amp::Power* pa = nullptr;

            for (auto* b : amp::flatten (a.core))
                pa = pa != nullptr ? pa : dynamic_cast<amp::Power*> (b);

            for (size_t i = 0; i < x.size(); i += 96)
            {
                a.processCore (x.data() + i, 96);
                lowestSupply = std::min (lowestSupply, pa->supplyRatio());
            }

            return x;
        };
        double unused = 0, dropOne = 0, dropTwo = 0;
        const auto yOne = run (R"({ "type": "power", "tube": "EL34", "plate_v": 450, "bias_v": -38, "raa_ohms": 3400 })", unused);
        const auto yTwo = run (R"({ "type": "power", "tube": "EL34", "plate_v": 450, "bias_v": -38, "raa_ohms": 1700, "pairs": 2 })", unused);
        double worst = 0.0, peak = 0.0;

        for (size_t i = 0; i < yOne.size(); ++i)
        {
            worst = std::max (worst, std::abs (yOne[i] - yTwo[i]));
            peak = std::max (peak, std::abs (yOne[i]));
        }

        run (R"({ "type": "power", "tube": "EL34", "plate_v": 450, "bias_v": -38, "raa_ohms": 3400, "supply_ohms": 170 })", dropOne);
        run (R"({ "type": "power", "tube": "EL34", "plate_v": 450, "bias_v": -38, "raa_ohms": 1700, "pairs": 2, "supply_ohms": 170 })", dropTwo);
        const double ratio = (1 - dropTwo) / (1 - dropOne);
        check (std::abs (two.maxWatts / one.maxWatts - 2.0) < 1e-9 && worst < 1e-9 * peak && ratio > 1.6 && ratio <= 2.0,
               "two EL34 pairs into 1.7k: " + juce::String (two.maxWatts, 1) + " W = " + juce::String (two.maxWatts / one.maxWatts, 4)
                   + " x one pair into 3.4k, the same sound (" + juce::String (worst / peak, 12) + " apart), and a 170 ohm supply sags "
                   + juce::String (ratio, 2) + " x as far (B+ " + juce::String (juce::roundToInt (450 * dropOne)) + " V vs "
                   + juce::String (juce::roundToInt (450 * dropTwo)) + " V)");
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
        if (args.size() >= 1 && args[0] == "bow")  // only the bow checks (quicker while working on it)
        {
            bowChecks();
            return failures == 0 ? 0 : 1;
        }

        if (args.size() >= 1 && args[0] == "check")
        {
            toneStackChecks();
            triodeChecks();
            feedbackChecks();
            couplingChecks();
            powerChecks();
            preampSupplyChecks();
            transformerChecks();
            biasShiftChecks();
            linearTwinChecks();
            stringsChecks();
            restringChecks();
            bowChecks();
            roomChecks (root);
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
