#pragma once

#include "Convolver.h"
#include "Curves.h"
#include "Filters.h"

#include <memory>
#include <vector>

namespace amp
{

// A block takes a mono signal and changes it in place.
class Block
{
public:
    Block (const Cfg& cfg, const std::set<juce::String>& settings);
    virtual ~Block() = default;

    virtual void prepare (int maxSamples) { juce::ignoreUnused (maxSamples); }
    virtual void process (double* x, int n) noexcept = 0;
    virtual void reset() noexcept {}

    // Blocks inside this block (feedback loops and split paths have some).
    virtual std::vector<Block*> children() { return {}; }

    // A line for the plugin's status text (tube operating points and so on), or empty.
    virtual juce::String describe() const { return {}; }

    juce::String type, id;  // id is empty when the preset has none

    // Set by a gate block with apply_after: one gain value per sample, applied to this block's output.
    const double* afterGain = nullptr;

    // Set by the plugin's signal view (never on the audio thread): every output sample is added to it.
    std::vector<float>* probe = nullptr;
    std::vector<float>* probeIn = nullptr;  // the same for its input (the signal view's curve: output against input)

    // What the signal view records: the output, or for sag and gate their headroom or gain.
    virtual const double* probeValues (const double* x) const noexcept { return x; }
};

using Chain = std::vector<std::unique_ptr<Block>>;

Chain buildChain (const juce::Array<juce::var>& cfgs, const BuildContext& ctx);
void runBlocks (const Chain& blocks, double* x, int n) noexcept;

// The blocks that can run inside a feedback loop: one sample at a time, and they can try a value first.
class LoopBlock : public Block
{
public:
    using Block::Block;

    virtual void beginSample (int n) noexcept { juce::ignoreUnused (n); }  // n = sample index in this buffer
    virtual double peek (double v) const noexcept = 0;   // the output for v, without moving on
    virtual void commitTried() noexcept = 0;             // move on to the next sample with the v of the last peek()

    // The most the output can move per unit of input within one sample (for the positive feedback check).
    virtual double maxSlope() const = 0;
};

// Gain and power blocks can be the target of a sag block.
struct SagTarget
{
    const double* headroom = nullptr;  // one value per sample, 1 = full headroom, lower = clips earlier
};

//==============================================================================
class EqBlock final : public LoopBlock
{
public:
    EqBlock (const Cfg& cfg, const BuildContext& ctx);

    void process (double* x, int n) noexcept override;
    void reset() noexcept override { filters.reset(); }
    double peek (double v) const noexcept override { return filters.peek (v) * level; }
    void commitTried() noexcept override { filters.commitTried(); }
    double maxSlope() const override;

private:
    FilterList filters;
    double level = 1.0;
};

class Tonestack final : public LoopBlock
{
public:
    Tonestack (const Cfg& cfg, const BuildContext& ctx);

    void process (double* x, int n) noexcept override;
    void reset() noexcept override { filter.reset(); }
    double peek (double v) const noexcept override { return filter.peek (v) * level; }
    void commitTried() noexcept override { filter.commitTried(); }
    double maxSlope() const override;

private:
    Iir filter;
    double level = 1.0;
};

class Gain final : public LoopBlock, public SagTarget
{
public:
    Gain (const Cfg& cfg, const BuildContext& ctx);

    void process (double* x, int n) noexcept override;
    void reset() noexcept override;
    void beginSample (int n) noexcept override
    {
        h = (headroom != nullptr ? headroom[n] : 1.0) * (supplyHeadroom != nullptr ? *supplyHeadroom : 1.0);
        driveOverH = drive / h;  // once per sample, so the tries only multiply
    }

    // Set by a power block that lists this stage in preamp_targets: its supply's share of the voltage (1 = full).
    const double* supplyHeadroom = nullptr;
    double peek (double v) const noexcept override;
    void commitTried() noexcept override;
    double maxSlope() const override;
    juce::String describe() const override;

private:
    double nextShift (double s, double g) const noexcept
    {
        const double over = g + s - top;  // how far past the top of the curve

        if (over > 0)
            s -= shiftAmount * shiftAttack * over;

        return s - shiftRelease * s;
    }

    Curve curve;
    FilterList pre, post;
    double drive = 1, bias = 0, level = 1, offset = 0, top = 1;
    mutable double triedG = 0;  // the grid value of the last peek(), for commitTried()
    double shiftAmount = 0, shiftAttack = 0, shiftRelease = 0, shift = 0;
    double h = 1.0, driveOverH = 1.0;
    bool isTriode = false;
    TriodeInfo tubeInfo;
};

class Power final : public LoopBlock, public SagTarget
{
public:
    Power (const Cfg& cfg, const BuildContext& ctx);

    void process (double* x, int n) noexcept override;
    void reset() noexcept override;
    void beginSample (int n) noexcept override;
    double peek (double v) const noexcept override;
    void commitTried() noexcept override;
    double maxSlope() const override;
    juce::String describe() const override;

    std::vector<float>* supplyProbe = nullptr;  // for the signal view: the supply voltage / plate_v, per sample
    double supplyRatio() const noexcept { return supplyV / supplyIdle; }

    // The gain stages fed from this supply through the decoupling filters, and their share of the voltage (1 = full).
    juce::StringArray preampTargetIds;
    double preampHeadroom = 1.0;

private:
    void nextShifts (double v) noexcept;
    void setHeadroom (double fromSag) noexcept;
    void stepSupply (double draw) noexcept;
    double nextFlux (double y) const noexcept;
    double transformer (double y) const noexcept;

    PushPull tubes;
    FilterList pre, post;
    double drive = 1, level = 1, offset = 0;
    mutable double triedU = 0, triedDraw = 0;  // the drive and current of the last peek(), for commitTried()
    double supplyOhms = 0, supplyUf = 47, supplyIdle = 450, supplyNoLoad = 450, supplyV = 450, supplyDecay = 0;
    double preampCoeff = 0;
    double otDt = 0, otW = 0, otPerSat = 0, otLambda = 0;  // the output transformer: 1 / rate, 2 pi low_hz, 1 / flux at saturation, flux
    mutable double triedY = 0;                             // the pair's output in the last peek()
    double shiftAmount = 0, shiftAttack = 0, shiftRelease = 0, sa = 0, sb = 0;
    double h = 1.0;
    PushPull::Supply supply;  // tubes.supply (h)
    juce::String tubeName;
};

class Sag final : public Block
{
public:
    Sag (const Cfg& cfg, const BuildContext& ctx);

    void prepare (int maxSamples) override { headroom.assign ((size_t) maxSamples, 1.0); }
    void process (double* x, int n) noexcept override;
    void reset() noexcept override { env = 0.0; }
    const double* probeValues (const double*) const noexcept override { return headroom.data(); }

    juce::StringArray targetIds;
    std::vector<double> headroom;

private:
    double amount = 0.3, sens = 1, attack = 0, release = 0, env = 0;
};

class Gate final : public Block
{
public:
    Gate (const Cfg& cfg, const BuildContext& ctx);

    void prepare (int maxSamples) override;
    void process (double* x, int n) noexcept override;
    void reset() noexcept override;
    const double* probeValues (const double* x) const noexcept override { return hasTarget ? gains.data() : x; }

    juce::String applyAfter;
    bool hasTarget = false;
    std::vector<double> gains;

private:
    double openLevel = 0, closeLevel = 0, attack = 0, release = 0, floor = 0;
    int hold = 0, window = 1;

    // The level is the biggest |x| in the last `window` samples: a "sliding max" kept with a queue of
    // falling values (each sample goes in and out once).
    std::vector<double> queueValue;
    std::vector<long long> queueTime;
    int queueFront = 0, queueSize = 0;
    long long time = 0;
    bool isOpen = false;
    int held = 0;
    double g = 0;
};

class Feedback final : public Block
{
public:
    Feedback (const Cfg& cfg, const BuildContext& ctx);

    void process (double* x, int n) noexcept override;
    void reset() noexcept override;
    std::vector<Block*> children() override;

private:
    double loopPeek (double xv, double y) const noexcept;
    void loopCommitTried() noexcept;

    std::vector<std::unique_ptr<LoopBlock>> blocks;
    mutable std::vector<double> tried, triedIn;  // each inner block's output and input in the last loopPeek() (= the answer), for probes
    FilterList path;
    double amount = 0.3, level = 1, y1 = 0, y2 = 0, y3 = 0;
    double slope = -1.0;  // the loop equation's slope at the last answer (-1 = no feedback): the next first guess
};

class Split final : public Block
{
public:
    Split (const Cfg& cfg, const BuildContext& ctx);

    void prepare (int maxSamples) override;
    void process (double* x, int n) noexcept override;
    std::vector<Block*> children() override;

private:
    struct Path
    {
        Chain blocks;
        double gain = 1.0;
        std::vector<double> buffer;
    };

    std::vector<Path> paths;
    std::vector<double> sum;
    double level = 1.0;
};

class IrBlock final : public Block
{
public:
    IrBlock (const Cfg& cfg, const BuildContext& ctx);

    void process (double* x, int n) noexcept override;
    void reset() noexcept override { convolver.reset(); }
    juce::String describe() const override { return note; }

private:
    Convolver convolver;
    double mix = 1.0, level = 1.0;
    juce::String note;
};

// A fake 4x12 cab (48 kHz), used when an IR file is missing (EngineTest make-audio also writes it to audio/).
std::vector<double> makeFakeCabIr();

// Find a block's IR file: absolute, next to the preset, one folder up from it, or in the current folder.
juce::File findIrFile (const juce::String& path, const juce::File& presetFolder);

} // namespace amp
