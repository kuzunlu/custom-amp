#pragma once

#include "Blocks.h"
#include "Room.h"

#include <juce_dsp/juce_dsp.h>

namespace amp
{

// The blocks of one preset, linked (sag targets, gate apply_after).
// The chain is cut in two: the "core" (everything up to the cab IR) runs at the oversampled rate,
// the "tail" (the cab IR and any eq blocks after it) runs at the normal rate.
class Amp
{
public:
    Amp (const juce::var& preset, double baseRate, double coreRate, const juce::File& presetFolder,
         const juce::File& irOverride);

    void prepare (int maxBaseSamples, int maxCoreSamples);
    void processCore (double* x, int n) noexcept { runBlocks (core, x, n); }
    void processTail (double* x, int n) noexcept { runBlocks (tail, x, n); }
    void reset() noexcept;

    juce::String name;
    juce::StringArray report;  // one line per block that has something to say (tube operating points, IR file)
    Chain core, tail;
    std::unique_ptr<Room> room;  // a last "room" block: the Engine runs the acoustic feedback loop around the amp

private:
    void link();

    std::vector<std::pair<Sag*, SagTarget*>> sagLinks;
    std::vector<std::pair<Gate*, Block*>> gateLinks;
};

// All blocks in the order they run. Blocks inside a feedback loop or split come right after it.
std::vector<Block*> flatten (const Chain& blocks);

// Where the tail starts: the first ir block of the run of ir and eq blocks at the end of the chain.
int tailStart (const juce::Array<juce::var>& chain);

// The whole amp for the plugin: oversampling around the core, then the tail, then an output level
// that makes every preset about equally loud.
class Engine
{
public:
    struct Options
    {
        juce::var preset;
        juce::File presetFolder, irOverride;
        double sampleRate = 48000.0;
        int maxBlock = 512;
        int factorLog2 = 3;  // 3 = 8x
        bool autoLevel = true;
        double fixedLevel = 0.0;  // above 0: use this output gain instead of measuring one
    };

    explicit Engine (const Options& options);  // throws PresetError

    void process (double* x, int n) noexcept;  // mono, n <= maxBlock; with a room block, the acoustic feedback loop too
    void reset() noexcept;

    int latencySamples() const { return juce::roundToInt (oversampling.getLatencyInSamples()); }
    int maxBlock() const { return blockSize; }
    double coreRate() const { return rate * (double) (1 << factorLog2); }

    Amp amp;
    double makeup = 1.0;
    int heardUpTo = -1;  // for the plugin: where in its input history this engine's warm-up stopped

private:
    void processAmp (double* x, int n) noexcept;  // the amp itself: oversampled core, tail, output level

    double rate;
    int blockSize, factorLog2;
    juce::dsp::Oversampling<double> oversampling;
};

// A short fake riff (0.5 V peak, like the test DI) used to set the output level.
std::vector<double> makeLevelRiff (double rate);

} // namespace amp
