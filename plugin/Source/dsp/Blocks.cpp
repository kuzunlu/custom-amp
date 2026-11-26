#include "Blocks.h"
#include "Bow.h"
#include "Restring.h"
#include "Strings.h"

#include "Circuit.h"

#include <juce_audio_formats/juce_audio_formats.h>

#include <map>

namespace amp
{

static std::set<juce::String> withCommon (std::set<juce::String> settings)
{
    settings.insert ("type");
    settings.insert ("id");
    return settings;
}

static juce::String blockWhere (const Cfg& cfg)
{
    return (cfg.text ("type", {}) + " block " + cfg.get ("id").toString()).trim();
}

Block::Block (const Cfg& cfg, const std::set<juce::String>& settings)
{
    amp::checkKeys (cfg.value(), withCommon (settings), blockWhere (cfg));
    type = cfg.text ("type", {});
    id = cfg.has ("id") ? cfg.get ("id").toString() : juce::String();
}

void runBlocks (const Chain& blocks, double* x, int n) noexcept
{
    for (auto& block : blocks)
    {
        if (block->probeIn != nullptr)
            for (int i = 0; i < n; ++i)
                block->probeIn->push_back ((float) x[i]);

        block->process (x, n);

        if (block->afterGain != nullptr)
            for (int i = 0; i < n; ++i)
                x[i] *= block->afterGain[i];

        if (block->probe != nullptr)
        {
            const double* v = block->probeValues (x);

            for (int i = 0; i < n; ++i)
                block->probe->push_back ((float) v[i]);
        }
    }
}

static double shiftCoefficient (double ms, double rate)
{
    return 1 - std::exp (-1 / (ms * 0.001 * rate));
}

// The coupling capacitor into this stage's grid resistor (coupling_nf into grid_ohms): a 1st-order high-pass at
// 1 / (2 pi R C) at the very start of the stage. When the grid draws current (bias shift), it charges this same
// capacitor, which then drains through R: so the shift recovers with C x R, unless shift_release_ms says otherwise.
// 0 nF = DC-coupled: no high-pass and no capacitor to charge (shift_release_ms, default 50 ms, is used then).
// Returns the release coefficient for the bias shift.
static double inputCoupling (const Cfg& cfg, const BuildContext& ctx, FilterList& pre, double defaultGridOhms)
{
    const double nf = cfg.number ("coupling_nf", 22.0), ohms = cfg.number ("grid_ohms", defaultGridOhms);

    if (nf < 0 || ohms <= 0)
        throw PresetError ("coupling_nf must be 0 or more and grid_ohms above 0");

    if (nf > 0)
        pre.filters.insert (pre.filters.begin(), Biquad::make ("highpass1", 1 / (2 * juce::MathConstants<double>::pi * ohms * nf * 1e-9),
                                                               ctx.rate, 0.707, 0.0, ctx.checkRate));

    const double releaseMs = cfg.has ("shift_release_ms") || nf == 0 ? cfg.number ("shift_release_ms", 50) : nf * 1e-9 * ohms * 1000;
    return shiftCoefficient (releaseMs, ctx.rate);
}

//==============================================================================
EqBlock::EqBlock (const Cfg& cfg, const BuildContext& ctx) : LoopBlock (cfg, { "bands", "level_db" })
{
    filters = makeFilters (cfg.list ("bands"), ctx);
    level = dbToGain (cfg.number ("level_db", 0.0));
}

void EqBlock::process (double* x, int n) noexcept
{
    for (int i = 0; i < n; ++i)
        x[i] = filters.tick (x[i]) * level;
}

//==============================================================================
Tonestack::Tonestack (const Cfg& cfg, const BuildContext& ctx)
    : LoopBlock (cfg, { "style", "parts", "treble", "mid", "bass", "treble_taper", "mid_taper", "bass_taper",
                        "source_ohms", "load_ohms", "level_db" })
{
    struct Style
    {
        juce::String circuit;
        std::map<juce::String, double> parts, fixed;
        std::map<juce::String, juce::String> tapers;
    };
    static const std::map<juce::String, std::vector<juce::String>> circuits {
        { "fmv", { "treble", "mid", "bass" } },
        { "james", { "treble", "bass" } },
    };
    static const std::map<juce::String, Style> styles {
        { "british", { "fmv", { { "C1", 470e-12 }, { "C2", 22e-9 }, { "C3", 22e-9 }, { "R1", 220e3 }, { "R2", 1e6 }, { "R3", 25e3 }, { "R4", 33e3 } }, {}, {} } },
        { "american", { "fmv", { { "C1", 250e-12 }, { "C2", 20e-9 }, { "C3", 20e-9 }, { "R1", 250e3 }, { "R2", 1e6 }, { "R3", 25e3 }, { "R4", 56e3 } }, {}, {} } },
        { "top_boost", { "fmv", { { "C1", 50e-12 }, { "C2", 22e-9 }, { "C3", 22e-9 }, { "R1", 1e6 }, { "R2", 1e6 }, { "R3", 10e3 }, { "R4", 100e3 } },
                         { { "mid", 10.0 } }, { { "treble", "log" }, { "bass", "log" } } } },
        { "james", { "james", { { "C1", 220e-12 }, { "C2", 2.2e-9 }, { "C3", 4.7e-9 }, { "C4", 47e-9 },
                                { "R1", 1e6 }, { "R2", 1e6 }, { "R3", 100e3 }, { "R4", 10e3 }, { "R5", 100e3 } },
                     {}, { { "treble", "log" }, { "bass", "log" } } } },
    };
    const auto styleName = cfg.text ("style", "british");

    if (styles.count (styleName) == 0)
        throw PresetError ("tonestack: unknown style '" + styleName + "'. Use one of: british, american, top_boost, james");

    const auto& style = styles.at (styleName);
    auto parts = style.parts;
    const auto partsCfg = cfg.get ("parts");

    if (! partsCfg.isVoid())
    {
        Cfg custom (partsCfg, "tonestack parts");
        std::set<juce::String> names;

        for (auto& p : parts)
            names.insert (p.first);

        custom.checkKeys (names);

        for (auto& [name, value] : parts)
            value = custom.number (name.toRawUTF8(), value);
    }

    std::map<juce::String, double> knobs;
    const std::map<juce::String, juce::String> tapers { { "treble", "linear" }, { "mid", "linear" }, { "bass", "log" } };
    const auto& knobNames = circuits.at (style.circuit);

    for (auto* knob : { "treble", "mid", "bass" })
    {
        const auto tapeKey = juce::String (knob) + "_taper";

        if (std::find (knobNames.begin(), knobNames.end(), knob) == knobNames.end() || style.fixed.count (knob) > 0)
        {
            if (cfg.has (knob) || cfg.has (tapeKey.toRawUTF8()))
                throw PresetError ("tonestack: the " + styleName + " style has no " + knob + " knob");

            knobs[knob] = (style.fixed.count (knob) > 0 ? style.fixed.at (knob) : 0.0) / 10;
            continue;
        }

        const double value = cfg.number (knob, 5.0);

        if (value < 0 || value > 10)
            throw PresetError (juce::String ("tonestack: ") + knob + " must be from 0 to 10, got " + juce::String (value));

        const auto taper = cfg.text (tapeKey.toRawUTF8(), style.tapers.count (knob) > 0 ? style.tapers.at (knob) : tapers.at (knob));

        if (taper != "log" && taper != "linear")
            throw PresetError (juce::String ("tonestack: ") + knob + "_taper must be 'log' or 'linear'");

        // Audio ("log") taper: half way round gives 10% of the resistance, like real tone pots.
        knobs[knob] = taper == "log" ? (std::pow (81.0, value / 10) - 1) / 80 : value / 10;
    }

    const double t = knobs["treble"], m = knobs["mid"], l = knobs["bass"];
    const double r = 1.0;  // a pot turned fully to one end still has a tiny resistance
    const std::vector<Part> netlist = style.circuit == "fmv"
        ? std::vector<Part> {
              { 'C', parts["C1"], "in", "top" },
              { 'R', std::max (parts["R1"] * (1 - t), r), "top", "out" },
              { 'R', std::max (parts["R1"] * t, r), "out", "x" },
              { 'R', std::max (parts["R2"] * l, r), "x", "y" },
              { 'R', std::max (parts["R3"] * (1 - m), r), "y", "w" },
              { 'R', std::max (parts["R3"] * m, r), "w", "gnd" },
              { 'R', parts["R4"], "in", "z" },
              { 'C', parts["C2"], "z", "x" },
              { 'C', parts["C3"], "z", "w" },
          }
        : std::vector<Part> {
              { 'R', parts["R3"], "in", "a" },
              { 'R', std::max (parts["R2"] * (1 - l), r), "a", "m" },
              { 'R', std::max (parts["R2"] * l, r), "m", "b" },
              { 'R', parts["R4"], "b", "gnd" },
              { 'C', parts["C3"], "a", "m" },
              { 'C', parts["C4"], "m", "b" },
              { 'R', parts["R5"], "m", "out" },
              { 'C', parts["C1"], "in", "t" },
              { 'R', std::max (parts["R1"] * (1 - t), r), "t", "out" },
              { 'R', std::max (parts["R1"] * t, r), "out", "u" },
              { 'C', parts["C2"], "u", "gnd" },
          };

    std::vector<double> b, a;
    digitalFilter (netlist, ctx.rate, cfg.number ("source_ohms", 1000.0), cfg.number ("load_ohms", 1e6), b, a);
    filter = Iir (b, a);
    level = dbToGain (cfg.number ("level_db", 0.0));
}

void Tonestack::process (double* x, int n) noexcept
{
    for (int i = 0; i < n; ++i)
        x[i] = filter.tick (x[i]) * level;
}

//==============================================================================
static const std::map<juce::String, std::set<juce::String>> curveSettings {
    { "custom", { "top", "top_softness", "bottom", "bottom_softness" } },
    { "triode", { "tube", "plate_v", "plate_ohms", "bias_v", "plate_volts" } },
};

static std::set<juce::String> gainSettings()
{
    std::set<juce::String> s { "drive_db", "curve", "bias", "level_db", "pre", "post", "oversample",
                               "bias_shift", "shift_attack_ms", "shift_release_ms", "coupling_nf", "grid_ohms", "linear" };

    for (auto& [curve, names] : curveSettings)
        s.insert (names.begin(), names.end());

    return s;
}

Gain::Gain (const Cfg& cfg, const BuildContext& ctx) : LoopBlock (cfg, gainSettings())
{
    static const juce::StringArray curveNames { "hard", "soft", "tube", "diode", "custom", "triode" };
    const auto name = cfg.text ("curve", "soft");

    if (! curveNames.contains (name))
        throw PresetError ("unknown curve '" + name + "'. Use one of: " + curveNames.joinIntoString (", "));

    for (auto& [other, names] : curveSettings)
    {
        if (other == name)
            continue;

        juce::StringArray wrong;

        for (auto& setting : names)
            if (cfg.has (setting.toRawUTF8()))
                wrong.add (setting);

        if (! wrong.isEmpty())
        {
            wrong.sort (false);
            throw PresetError ("gain: the setting(s) " + wrong.joinIntoString (", ") + " only work with curve '" + other + "'");
        }
    }

    top = 1.0;  // where the top of the curve starts to clip (bias shift starts there)
    level = dbToGain (cfg.number ("level_db", 0.0));

    if (name == "custom")
    {
        curve = makeCustomCurve (cfg.number ("top", 1.0), cfg.number ("top_softness", 0.5),
                                 cfg.number ("bottom", 1.0), cfg.number ("bottom_softness", 0.5));
        top = cfg.number ("top", 1.0);
    }
    else if (name == "triode")
    {
        const double biasV = cfg.number ("bias_v", -1.5);
        curve = makeTriodeCurve (cfg.text ("tube", "12AX7"), cfg.number ("plate_v", 300.0),
                                 cfg.number ("plate_ohms", 100e3), biasV, tubeInfo);
        top = -biasV;
        isTriode = true;

        if (cfg.flag ("plate_volts", false))
            level *= tubeInfo.gain;  // put the stage's real gain back: output = plate swing in volts
    }
    else
    {
        curve.kind = name == "hard" ? Curve::Kind::hard
                   : name == "tube" ? Curve::Kind::tube
                   : name == "diode" ? Curve::Kind::diode
                                     : Curve::Kind::soft;
    }

    drive = dbToGain (cfg.number ("drive_db", 0.0));
    bias = cfg.number ("bias", 0.0);
    offset = curve (bias);  // keeps silence at zero when bias is set
    pre = makeFilters (cfg.list ("pre"), ctx);
    post = makeFilters (cfg.list ("post"), ctx);
    shiftRelease = inputCoupling (cfg, ctx, pre, 1e6);  // a preamp grid resistor: usually 1 Mohm
    shiftAmount = cfg.number ("bias_shift", 0.0);

    if (shiftAmount < 0 || shiftAmount > 1)
        throw PresetError ("bias_shift must be from 0 to 1, got " + juce::String (shiftAmount));

    shiftAttack = shiftCoefficient (cfg.number ("shift_attack_ms", 1), ctx.rate);

    // linear: the same stage without clipping (X-ray listening subtracts it to hear only what the clipping adds). Its
    // curve becomes the straight line through its working point with the same slope, so quiet playing is unchanged;
    // no bias shift (that needs clipping), and sag changes nothing (it only lowers the clipping ceiling).
    if (cfg.flag ("linear", false))
    {
        const double e = 1e-4;
        Curve line;
        line.kind = Curve::Kind::line;
        line.slope = (curve (bias + e) - curve (bias - e)) / (2 * e);
        curve = line;
        offset = curve (bias);
        shiftAmount = 0.0;
    }
}

void Gain::reset() noexcept
{
    pre.reset();
    post.reset();
    shift = 0.0;
    h = 1.0;
}

void Gain::process (double* x, int n) noexcept
{
    if (headroom == nullptr && supplyHeadroom == nullptr && shiftAmount == 0)
    {
        for (int i = 0; i < n; ++i)
            x[i] = post.tick (curve (pre.tick (x[i]) * drive + bias) - offset) * level;

        return;
    }

    // The curve's ceiling is scaled down to the headroom (quiet signals are not changed),
    // and the bias shift moves the working point.
    for (int i = 0; i < n; ++i)
    {
        const double hh = (headroom != nullptr ? headroom[i] : 1.0) * (supplyHeadroom != nullptr ? *supplyHeadroom : 1.0);
        const double g = pre.tick (x[i]) * drive / hh + bias;
        const double y = hh * (curve (g + shift) - offset);

        if (shiftAmount != 0)
            shift = nextShift (shift, g);

        x[i] = post.tick (y) * level;
    }
}

double Gain::peek (double v) const noexcept
{
    const double g = pre.peek (v) * driveOverH + bias;
    triedG = g;
    return post.peek (h * (curve (g + shift) - offset)) * level;
}

void Gain::commitTried() noexcept
{
    pre.commitTried();

    if (shiftAmount != 0)
        shift = nextShift (shift, triedG);

    post.commitTried();
}

juce::String Gain::describe() const
{
    if (! isTriode)
        return {};

    return "plate " + juce::String (juce::roundToInt (tubeInfo.plateV)) + " V, " + juce::String (tubeInfo.plateMa, 2)
           + " mA, gain " + juce::String (juce::roundToInt (tubeInfo.gain));
}

//==============================================================================
// pairs: a whole number (1.5 pairs of tubes is a mistake, not something to round).
static int wholePairs (double pairs)
{
    if (pairs != std::floor (pairs))
        throw PresetError ("power pairs must be a whole number (1 to 4), got " + juce::String (pairs));

    return (int) pairs;
}

Power::Power (const Cfg& cfg, const BuildContext& ctx)
    : LoopBlock (cfg, { "tube", "plate_v", "screen_v", "bias_v", "raa_ohms", "mismatch", "drive_db", "level_db",
                        "pre", "post", "oversample", "bias_shift", "shift_attack_ms", "shift_release_ms",
                        "supply_ohms", "supply_uf", "preamp_targets", "preamp_filter_ms", "transformer_low_hz",
                        "transformer_sat_hz", "pairs", "coupling_nf", "grid_ohms", "linear" })
{
    const double plateV = cfg.number ("plate_v", 450.0);
    const double screenV = cfg.number ("screen_v", 0.0);
    tubeName = cfg.text ("tube", "EL34");
    tubes = PushPull::make (tubeName, plateV, screenV != 0 ? screenV : plateV, cfg.number ("bias_v", -45.0),
                            cfg.number ("raa_ohms", 3400.0), cfg.number ("mismatch", 0.0), wholePairs (cfg.number ("pairs", 1)));
    supply = tubes.supply (1.0);
    double idleDraw = 0.0;
    offset = tubes.shape (0.0, supply, 0.0, 0.0, idleDraw);  // mismatched tubes leave a step at rest

    // C++ only: the power supply as a filter capacitor charged through the rectifier and transformer resistance.
    // plate_v stays the voltage at idle, so the supply with no load at all is a little higher.
    supplyOhms = cfg.number ("supply_ohms", 0.0);
    supplyUf = cfg.number ("supply_uf", 47.0);

    if (supplyOhms < 0)
        throw PresetError ("power: supply_ohms must be 0 or more, got " + juce::String (supplyOhms));

    if (supplyUf <= 0)
        throw PresetError ("power: supply_uf must be above 0, got " + juce::String (supplyUf));

    supplyIdle = supplyV = plateV;
    supplyNoLoad = plateV + idleDraw * supplyOhms;
    supplyDecay = std::exp (-1.0 / (supplyOhms * supplyUf * 1e-6 * ctx.rate));

    // The preamp is fed from the same supply through decoupling filters (a resistor and a capacitor per node), so
    // its voltage follows the supply slowly. The gain stages in preamp_targets clip lower by that share.
    const auto targets = cfg.get ("preamp_targets");

    if (! targets.isVoid() && ! targets.isArray())
        throw PresetError ("power: preamp_targets must be a list, like [\"v1a\", \"v2a\"]");

    for (auto& t : cfg.list ("preamp_targets"))
        preampTargetIds.add (t.toString());

    const double filterMs = cfg.number ("preamp_filter_ms", 100.0);

    if (filterMs <= 0)
        throw PresetError ("power: preamp_filter_ms must be above 0, got " + juce::String (filterMs));

    preampCoeff = 1 - std::exp (-1.0 / (filterMs * 0.001 * ctx.rate));
    drive = dbToGain (cfg.number ("drive_db", 0.0));
    level = dbToGain (cfg.number ("level_db", 0.0));
    pre = makeFilters (cfg.list ("pre"), ctx);
    post = makeFilters (cfg.list ("post"), ctx);
    shiftRelease = inputCoupling (cfg, ctx, pre, 220e3);  // power tubes' grid (bias feed) resistors: about 220k
    // The output transformer: its winding is an inductor across the load, a 1st-order high-pass at transformer_low_hz
    // (it passes no DC). Its core holds only so much flux (volts x time), so loud LOW notes saturate it: at
    // transformer_sat_hz a full-power note just reaches that flux. Flux lambda' = out, out = y - w m (lambda),
    // m = lambda (1 + (lambda / lambdaSat)^4) (the magnetizing current: twice the linear one at lambdaSat).
    const double lowHz = cfg.number ("transformer_low_hz", 10.0), satHz = cfg.number ("transformer_sat_hz", 0.0);

    if (lowHz <= 0 || satHz < 0)
        throw PresetError ("power: transformer_low_hz must be above 0 and transformer_sat_hz 0 or more");

    otDt = 1.0 / ctx.rate;
    otW = 2 * juce::MathConstants<double>::pi * lowHz;
    otPerSat = satHz > 0 ? 2 * juce::MathConstants<double>::pi * satHz / std::abs (tubes.bias) : 0.0;  // full output = |bias_v|

    shiftAmount = cfg.number ("bias_shift", 0.0);

    if (shiftAmount < 0 || shiftAmount > 1)
        throw PresetError ("bias_shift must be from 0 to 1, got " + juce::String (shiftAmount));

    shiftAttack = shiftCoefficient (cfg.number ("shift_attack_ms", 1), ctx.rate);
    idleCurrent = idleDraw;

    // linear: the pair as a perfect linear amplifier with its small-signal gain (X-ray listening): no clipping, no
    // crossover, no bias shift, no transformer saturation, and a constant current (so its supply never sags).
    if (cfg.flag ("linear", false))
    {
        double unused = 0.0;
        linear = true;
        linSlope = (tubes.shape (1e-3, supply, 0.0, 0.0, unused) - tubes.shape (-1e-3, supply, 0.0, 0.0, unused)) / 2e-3;
        otPerSat = 0.0;
        shiftAmount = 0.0;
    }
}

void Power::reset() noexcept
{
    pre.reset();
    post.reset();
    sa = sb = 0.0;
    h = 1.0;
    supply = tubes.supply (1.0);
    supplyV = supplyIdle;
    preampHeadroom = 1.0;
    otLambda = 0.0;
}

void Power::setHeadroom (double fromSag) noexcept
{
    // The sag block's headroom times what the supply has left (always 1 without supply_ohms).
    const double hh = supplyOhms > 0 ? fromSag * (supplyV / supplyIdle) : fromSag;

    if (hh != h)
    {
        h = hh;
        supply = tubes.supply (h);
    }
}

void Power::stepSupply (double draw) noexcept
{
    // C dV/dt = (V0 - V) / R - I, solved exactly over one sample with the tubes' current I held for it:
    // V moves towards V0 - I R with the time constant R C.
    const double target = supplyNoLoad - draw * supplyOhms;
    supplyV = target + (supplyV - target) * supplyDecay;
    preampHeadroom += preampCoeff * (supplyV / supplyIdle - preampHeadroom);

    if (supplyProbe != nullptr)
        supplyProbe->push_back ((float) (supplyV / supplyIdle));
}

void Power::nextShifts (double v) noexcept
{
    // Each grid charges its coupling capacitor when it goes above 0 V, then drifts back.
    const double ga = tubes.bias + sa + v, gb = tubes.bias + sb - v;

    if (ga > 0)
        sa -= shiftAmount * shiftAttack * ga;

    if (gb > 0)
        sb -= shiftAmount * shiftAttack * gb;

    sa = sa - shiftRelease * sa;
    sb = sb - shiftRelease * sb;
}

void Power::process (double* x, int n) noexcept
{
    for (int i = 0; i < n; ++i)
    {
        const double v = pre.tick (x[i]) * drive;
        setHeadroom (headroom != nullptr ? headroom[i] : 1.0);
        double draw = idleCurrent;
        const double y = linear ? linSlope * v : tubes.shape (v, supply, sa, sb, draw) - offset;

        if (shiftAmount != 0)
            nextShifts (v);

        if (supplyOhms > 0)
            stepSupply (draw);

        const double flux = nextFlux (y), out = (flux - otLambda) / otDt;
        otLambda = flux;
        x[i] = post.tick (out) * level;
    }
}

// The transformer, one step of backward Euler (stable however hard the core saturates): the new flux is
// (lambda + dt y) / (1 + dt w (1 + (lambda / lambdaSat)^4)), and the output is the flux's change per second.
double Power::nextFlux (double y) const noexcept
{
    const double s = otLambda * otPerSat, s2 = s * s;
    return (otLambda + otDt * y) / (1 + otDt * otW * (1 + s2 * s2));
}

double Power::transformer (double y) const noexcept
{
    return (nextFlux (y) - otLambda) / otDt;
}

void Power::beginSample (int n) noexcept
{
    setHeadroom (headroom != nullptr ? headroom[n] : 1.0);
}

double Power::peek (double v) const noexcept
{
    const double u = pre.peek (v) * drive;
    triedU = u;
    triedDraw = idleCurrent;
    triedY = linear ? linSlope * u : tubes.shape (u, supply, sa, sb, triedDraw) - offset;
    return post.peek (transformer (triedY)) * level;
}

void Power::commitTried() noexcept
{
    pre.commitTried();

    if (shiftAmount != 0)
        nextShifts (triedU);

    if (supplyOhms > 0)
        stepSupply (triedDraw);

    otLambda = nextFlux (triedY);
    post.commitTried();
}

juce::String Power::describe() const
{
    return juce::String (2 * tubes.pairs) + "x " + tubeName + ", idle " + juce::String (juce::roundToInt (tubes.idleMa)) + " mA ("
           + juce::String (tubes.idleWatts, 1) + " W each), max about " + juce::String (juce::roundToInt (tubes.maxWatts)) + " W"
           + (supplyOhms > 0 ? ", supply " + juce::String (juce::roundToInt (supplyOhms)) + " ohm and " + juce::String (supplyUf, 0)
                                   + " uF (recovers in about " + juce::String (supplyOhms * supplyUf * 1e-3, 1) + " ms)"
                             : juce::String());
}

//==============================================================================
Sag::Sag (const Cfg& cfg, const BuildContext& ctx) : Block (cfg, { "amount", "sens_db", "attack_ms", "release_ms", "targets" })
{
    amount = cfg.number ("amount", 0.3);

    if (amount < 0 || amount >= 1)
        throw PresetError ("sag amount must be from 0 up to (not including) 1, got " + juce::String (amount));

    sens = dbToGain (cfg.number ("sens_db", 0.0));
    attack = std::exp (-1 / (cfg.number ("attack_ms", 5) * 0.001 * ctx.rate));
    release = std::exp (-1 / (cfg.number ("release_ms", 150) * 0.001 * ctx.rate));

    const auto targets = cfg.get ("targets");

    if (! targets.isVoid() && ! targets.isArray())
        throw PresetError ("sag targets must be a list, like [\"v2\"]");

    for (auto& t : cfg.list ("targets"))
        targetIds.add (t.toString());
}

void Sag::process (double* x, int n) noexcept
{
    // Follow the loudness: rise with the attack time, fall with the release time.
    // tanh: the supply can only drop so far, even when playing very hard.
    for (int i = 0; i < n; ++i)
    {
        const double v = std::abs (x[i]);
        const double c = v > env ? attack : release;
        env = v + c * (env - v);
        headroom[(size_t) i] = 1 - amount * std::tanh (env * sens);
    }
}

//==============================================================================
Gate::Gate (const Cfg& cfg, const BuildContext& ctx)
    : Block (cfg, { "threshold_db", "hysteresis_db", "attack_ms", "hold_ms", "release_ms", "range_db", "apply_after" })
{
    const double threshold = cfg.number ("threshold_db", -50.0);
    openLevel = dbToGain (threshold);
    closeLevel = dbToGain (threshold - cfg.number ("hysteresis_db", 6.0));
    attack = shiftCoefficient (cfg.number ("attack_ms", 0.5), ctx.rate);
    release = shiftCoefficient (cfg.number ("release_ms", 100), ctx.rate);
    hold = (int) (cfg.number ("hold_ms", 50) * 0.001 * ctx.rate);
    floor = dbToGain (cfg.number ("range_db", -80.0));
    // The level is the biggest peak in the last 20 ms: longer than one cycle of the lowest guitar note,
    // so the level does not dip between the peaks of one note.
    window = std::max (1, (int) (0.020 * ctx.rate));
    applyAfter = cfg.get ("apply_after").isVoid() ? juce::String() : cfg.get ("apply_after").toString();
    g = floor;
}

void Gate::prepare (int maxSamples)
{
    gains.assign ((size_t) maxSamples, 1.0);
    queueValue.assign ((size_t) window + 1, 0.0);
    queueTime.assign ((size_t) window + 1, 0);
    reset();
}

void Gate::reset() noexcept
{
    queueFront = queueSize = 0;
    time = 0;
    isOpen = false;
    held = 0;
    g = floor;
}

void Gate::process (double* x, int n) noexcept
{
    const int capacity = (int) queueValue.size();

    for (int i = 0; i < n; ++i)
    {
        // Sliding max: drop smaller values from the back, the oldest from the front once it leaves the window.
        const double v = std::abs (x[i]);

        while (queueSize > 0 && queueValue[(size_t) ((queueFront + queueSize - 1) % capacity)] <= v)
            --queueSize;

        const int back = (queueFront + queueSize) % capacity;
        queueValue[(size_t) back] = v;
        queueTime[(size_t) back] = time;
        ++queueSize;

        if (time - queueTime[(size_t) queueFront] >= window)
        {
            queueFront = (queueFront + 1) % capacity;
            --queueSize;
        }

        ++time;
        const double level = queueValue[(size_t) queueFront];

        if (level > openLevel)
        {
            isOpen = true;
            held = 0;
        }
        else if (isOpen && level < closeLevel)
        {
            if (++held > hold)
                isOpen = false;
        }

        const double target = isOpen ? 1.0 : floor;
        g += (target > g ? attack : release) * (target - g);
        gains[(size_t) i] = g;
    }

    if (! hasTarget)
        for (int i = 0; i < n; ++i)
            x[i] *= gains[(size_t) i];
}

//==============================================================================
// Find y where f(y) = 0, for an f that always goes down as y goes up.
// First walk away from y0 in bigger and bigger steps until f changes sign,
// then close in with the "Illinois" method (a safer form of the secant method).
template <typename Function>
static double solveFalling (Function f, double y0, double tol = 1e-10, int maxSteps = 60) noexcept
{
    double a = y0, fa = f (y0);

    if (fa == 0)
        return a;

    const double direction = fa > 0 ? 1.0 : -1.0;
    double step = std::max (1e-4, 0.01 * std::abs (y0));
    double b = a + direction * step, fb = f (b);

    while ((fb > 0) == (fa > 0))
    {
        a = b;
        fa = fb;
        step *= 2;
        b = a + direction * step;
        fb = f (b);
    }

    double c = b;

    for (int i = 0; i < maxSteps; ++i)
    {
        c = b - fb * (b - a) / (fb - fa);
        const double fc = f (c);

        if (std::abs (fc) < tol || std::abs (b - a) < tol)
            break;

        if ((fc > 0) != (fb > 0))
        {
            a = b;
            fa = fb;
        }
        else
        {
            fa *= 0.5;  // the Illinois trick: stops one end from getting stuck
        }

        b = c;
        fb = fc;
    }

    return c;
}

// The same answer as solveFalling (within tol), usually with far fewer tries of f: from one sample to the next
// (at the oversampled rate) the loop hardly changes, so jump straight to the answer with the slope the last
// sample ended with (Newton's method with an old slope). If two jumps do not land within tol, or the slope
// looks wrong (g must fall), the safe search takes over. Same answer as the plain search within tol.
template <typename Function>
static double solveFallingFast (Function f, double y0, double& slope, double tol = 1e-10) noexcept
{
    double a = y0, fa = f (a);

    if (std::abs (fa) < tol)
        return a;

    for (int i = 0; i < 2 && slope < 0 && std::isfinite (slope); ++i)
    {
        const double b = a - fa / slope, fb = f (b);

        if (b != a)
        {
            const double s = (fb - fa) / (b - a);
            slope = s < 0 && std::isfinite (s) ? s : slope;
        }

        if (std::abs (fb) < tol)
            return b;

        a = b;
        fa = fb;
    }

    return solveFalling (f, a, tol);
}

//==============================================================================
// The positive feedback check.
static constexpr int slopePoints = 200000;        // how finely steepest() samples a curve
static constexpr double gainSlopeRange = 100.0;   // gain curves are sampled on -100..100

// The steepest slope of fn on -halfRange..halfRange (200001 points).
template <typename Fn>
static double steepest (Fn fn, double halfRange)
{
    double worst = 0.0, last = fn (-halfRange + 2.0 * halfRange * 0 / slopePoints);

    for (int i = 1; i <= slopePoints; ++i)
    {
        const double y = fn (-halfRange + 2.0 * halfRange * i / slopePoints);
        worst = std::max (worst, std::abs (y - last));
        last = y;
    }

    return worst / (2.0 * halfRange / slopePoints);
}

// How much of the current sample a list of filters passes at once (the product of their b0).
static double firstGain (const FilterList& list)
{
    double g = 1.0;

    for (auto& f : list.filters)
        g *= std::abs (f.b0);

    return g;
}

double EqBlock::maxSlope() const { return firstGain (filters) * level; }

double Tonestack::maxSlope() const { return std::abs (filter.b[0]) * level; }

double Gain::maxSlope() const
{
    // h (sag) and the bias shift only move along the curve: its steepest point is the limit
    return firstGain (pre) * drive * steepest ([this] (double x) { return curve (x); }, gainSlopeRange) * firstGain (post) * level;
}

double Power::maxSlope() const
{
    const double pair = steepest ([this] (double x) { return tubes.unlimited (x); }, 4 * std::abs (tubes.bias));
    return firstGain (pre) * drive * pair * firstGain (post) * level;
}

Feedback::Feedback (const Cfg& cfg, const BuildContext& ctx) : Block (cfg, { "amount", "path", "chain", "oversample", "level_db" })
{
    amount = cfg.number ("amount", 0.3);
    level = dbToGain (cfg.number ("level_db", 0.0));
    path = makeFilters (cfg.list ("path"), ctx);
    int i = 0;

    for (auto& inner : cfg.list ("chain"))
    {
        Cfg innerCfg (inner, "feedback chain block " + juce::String (++i));
        const auto kind = innerCfg.text ("type", {});

        if (kind != "eq" && kind != "tonestack" && kind != "gain" && kind != "power")
            throw PresetError ("feedback: '" + kind + "' blocks can not go inside a feedback loop. Use: eq, tonestack, gain, power");

        if (innerCfg.has ("oversample"))
            throw PresetError ("feedback: gain blocks inside the loop can not set 'oversample'. Set it on the feedback block");

        if (kind == "eq")
            blocks.push_back (std::make_unique<EqBlock> (innerCfg, ctx));
        else if (kind == "tonestack")
            blocks.push_back (std::make_unique<Tonestack> (innerCfg, ctx));
        else if (kind == "gain")
            blocks.push_back (std::make_unique<Gain> (innerCfg, ctx));
        else
            blocks.push_back (std::make_unique<Power> (innerCfg, ctx));
    }

    tried.assign (blocks.size(), 0.0);
    triedIn.assign (blocks.size(), 0.0);

    if (amount < 0)
    {
        // Positive feedback: the loop equation has exactly one answer only while the loop can never gain 1 or
        // more within one sample. Above that a real circuit latches or oscillates, and the solver can not work.
        double gain = -amount * firstGain (path);

        for (auto& b : blocks)
            gain *= b->maxSlope();

        if (gain >= 1)
            throw PresetError ("feedback: positive feedback (amount below 0) needs a loop gain below 1, but this loop can reach "
                               + juce::String (gain, 2) + ". Use a smaller amount or less gain inside the loop");
    }
}

void Feedback::reset() noexcept
{
    path.reset();
    y1 = y2 = y3 = 0.0;
    slope = -1.0;

    for (auto& b : blocks)
        b->reset();
}

std::vector<Block*> Feedback::children()
{
    std::vector<Block*> list;

    for (auto& b : blocks)
        list.push_back (b.get());

    return list;
}

double Feedback::loopPeek (double xv, double y) const noexcept
{
    // What the chain puts out if the loop output is y.
    double v = xv - amount * path.peek (y);

    for (size_t k = 0; k < blocks.size(); ++k)
    {
        triedIn[k] = v;
        tried[k] = v = blocks[k]->peek (v);
    }

    return v;
}

void Feedback::loopCommitTried() noexcept
{
    // The solver's answer is always the last value it tried, so every filter and tube already holds what it
    // computed for it: move them on without running the loop again (the same as working it out once more).
    path.commitTried();

    for (auto& b : blocks)
        b->commitTried();
}

void Feedback::process (double* x, int n) noexcept
{
    // There is no delay in the loop: each output sample is found by solving the loop equation.
    for (int i = 0; i < n; ++i)
    {
        for (auto& b : blocks)
            b->beginSample (i);

        const double xv = x[i];
        // First guess: the last three answers continued as a smooth curve (the loop runs oversampled).
        const double y = solveFallingFast ([this, xv] (double yy) { return loopPeek (xv, yy) - yy; }, 3 * y1 - 3 * y2 + y3, slope);
        loopCommitTried();

        for (size_t k = 0; k < blocks.size(); ++k)
        {
            if (auto* p = blocks[k]->probe)
                p->push_back ((float) tried[k]);

            if (auto* p = blocks[k]->probeIn)
                p->push_back ((float) triedIn[k]);
        }

        y3 = y2;
        y2 = y1;
        y1 = y;
        x[i] = y * level;
    }
}

//==============================================================================
Split::Split (const Cfg& cfg, const BuildContext& ctx) : Block (cfg, { "paths", "level_db" })
{
    const auto pathCfgs = cfg.list ("paths");

    if (pathCfgs.isEmpty())
        throw PresetError ("split: needs at least one path");

    int i = 0;

    for (auto& p : pathCfgs)
    {
        Cfg pathCfg (p, "split path " + juce::String (++i));
        pathCfg.checkKeys ({ "chain", "level_db", "invert" });
        Path path;
        path.blocks = buildChain (pathCfg.list ("chain"), ctx);
        path.gain = (pathCfg.flag ("invert", false) ? -1.0 : 1.0) * dbToGain (pathCfg.number ("level_db", 0.0));
        paths.push_back (std::move (path));
    }

    level = dbToGain (cfg.number ("level_db", 0.0));
}

void Split::prepare (int maxSamples)
{
    sum.assign ((size_t) maxSamples, 0.0);

    for (auto& p : paths)
        p.buffer.assign ((size_t) maxSamples, 0.0);
}

void Split::process (double* x, int n) noexcept
{
    std::fill (sum.begin(), sum.begin() + n, 0.0);

    for (auto& p : paths)
    {
        std::copy (x, x + n, p.buffer.begin());
        runBlocks (p.blocks, p.buffer.data(), n);

        for (int i = 0; i < n; ++i)
            sum[(size_t) i] += p.gain * p.buffer[(size_t) i];
    }

    for (int i = 0; i < n; ++i)
        x[i] = sum[(size_t) i] * level;
}

std::vector<Block*> Split::children()
{
    std::vector<Block*> list;

    for (auto& p : paths)
        for (auto& b : p.blocks)
            list.push_back (b.get());

    return list;
}

//==============================================================================
std::vector<double> makeFakeCabIr()
{
    // Rough 4x12 shape: low thump, mid dip, presence bump, steep top cut (make_fake_ir).
    const double rate = 48000.0;
    FilterList bands;
    bands.filters = {
        Biquad::make ("highpass", 70, rate, 0.9, 0, rate),
        Biquad::make ("peak", 110, rate, 1.2, 4, rate),
        Biquad::make ("peak", 400, rate, 1.0, -2, rate),
        Biquad::make ("peak", 1200, rate, 2.0, -3, rate),
        Biquad::make ("peak", 2500, rate, 1.5, 3, rate),
        Biquad::make ("lowpass", 5000, rate, 0.7, 0, rate),
        Biquad::make ("lowpass", 6000, rate, 0.7, 0, rate),
    };

    std::vector<double> ir (2048, 0.0);
    ir[0] = 1.0;

    // run_filters runs each filter over the whole signal in turn.
    for (auto& f : bands.filters)
        for (auto& v : ir)
            v = f.tick (v);

    const size_t fade = ir.size() / 4;
    const double step = juce::MathConstants<double>::pi / (double) (fade - 1);
    double peak = 0.0;

    for (size_t k = 0; k < fade; ++k)
    {
        const double angle = k + 1 == fade ? juce::MathConstants<double>::pi : (double) k * step;
        ir[ir.size() - fade + k] *= 0.5 * (1 + std::cos (angle));
    }

    for (auto v : ir)
        peak = std::max (peak, std::abs (v));

    for (auto& v : ir)
        v *= 0.9 / peak;

    return ir;
}

juce::File findIrFile (const juce::String& path, const juce::File& presetFolder)
{
    if (juce::File::isAbsolutePath (path))
        return juce::File (path).existsAsFile() ? juce::File (path) : juce::File();

    juce::Array<juce::File> places;

    if (presetFolder != juce::File())
    {
        places.add (presetFolder.getChildFile (path));
        places.add (presetFolder.getParentDirectory().getChildFile (path));
    }

    places.add (juce::File::getCurrentWorkingDirectory().getChildFile (path));

    for (auto& f : places)
        if (f.existsAsFile())
            return f;

    return {};
}

IrBlock::IrBlock (const Cfg& cfg, const BuildContext& ctx) : Block (cfg, { "file", "mix", "level_db" })
{
    if (! ctx.allowIr)
        throw PresetError ("ir: in the plugin, a cab IR must come at the end of the chain (only eq and ir blocks "
                           "can follow it, and it can not be inside a split or feedback block)");

    if (! cfg.has ("file"))
        throw PresetError ("ir: needs a 'file'");

    mix = cfg.number ("mix", 1.0);
    level = dbToGain (cfg.number ("level_db", 0.0));

    const auto path = cfg.text ("file", {});
    auto file = ctx.irOverride != juce::File() ? ctx.irOverride : findIrFile (path, ctx.presetFolder);
    std::vector<double> ir;
    double irRate = 48000.0;

    if (file.existsAsFile())
    {
        juce::AudioFormatManager formats;
        formats.registerBasicFormats();
        std::unique_ptr<juce::AudioFormatReader> reader (formats.createReaderFor (file));

        if (reader == nullptr || reader->lengthInSamples <= 0)
            throw PresetError ("ir: can not read the file '" + file.getFullPathName() + "'");

        juce::AudioBuffer<float> buffer ((int) reader->numChannels, (int) reader->lengthInSamples);
        reader->read (&buffer, 0, buffer.getNumSamples(), 0, true, true);
        irRate = reader->sampleRate;
        ir.assign ((size_t) buffer.getNumSamples(), 0.0);

        // The amp is mono: use the average of the IR's channels.
        for (int i = 0; i < buffer.getNumSamples(); ++i)
        {
            double sum = 0.0;

            for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
                sum += (double) buffer.getSample (ch, i);

            ir[(size_t) i] = sum / buffer.getNumChannels();
        }

        note = file.getFileName();
    }
    else
    {
        ir = makeFakeCabIr();
        note = "'" + path + "' not found: using the built-in test cab";
    }

    if (irRate != ctx.rate)
    {
        // More samples per second means each sample must count less, or the cab gets louder.
        ir = resampleFft (ir, irRate, ctx.rate);

        for (auto& v : ir)
            v *= irRate / ctx.rate;
    }

    convolver.load (ir);
}

void IrBlock::process (double* x, int n) noexcept
{
    for (int i = 0; i < n; ++i)
        x[i] = ((1 - mix) * x[i] + mix * convolver.tick (x[i])) * level;
}

//==============================================================================
Chain buildChain (const juce::Array<juce::var>& cfgs, const BuildContext& ctx)
{
    Chain blocks;
    int i = 0;

    for (auto& c : cfgs)
    {
        ++i;
        Cfg cfg (c, "block " + juce::String (i));
        const auto kind = cfg.text ("type", {});
        std::unique_ptr<Block> block;

        if (kind == "eq")             block = std::make_unique<EqBlock> (cfg, ctx);
        else if (kind == "tonestack") block = std::make_unique<Tonestack> (cfg, ctx);
        else if (kind == "gain")      block = std::make_unique<Gain> (cfg, ctx);
        else if (kind == "power")     block = std::make_unique<Power> (cfg, ctx);
        else if (kind == "sag")       block = std::make_unique<Sag> (cfg, ctx);
        else if (kind == "feedback")  block = std::make_unique<Feedback> (cfg, ctx);
        else if (kind == "split")     block = std::make_unique<Split> (cfg, ctx);
        else if (kind == "gate")      block = std::make_unique<Gate> (cfg, ctx);
        else if (kind == "ir")        block = std::make_unique<IrBlock> (cfg, ctx);
        else if (kind == "strings")   block = std::make_unique<SympatheticStrings> (cfg, ctx);
        else if (kind == "restring")  block = std::make_unique<Restring> (cfg, ctx);
        else if (kind == "bow")       block = std::make_unique<Bow> (cfg, ctx);
        else if (kind == "room")
            throw PresetError ("room: it must be the last block of the main chain (after the cab)");
        else
            throw PresetError ("block " + juce::String (i) + ": unknown type '" + kind
                               + "'. Use one of: eq, tonestack, gain, power, sag, feedback, split, gate, ir, room, strings, restring, bow");

        blocks.push_back (std::move (block));
    }

    return blocks;
}

} // namespace amp
