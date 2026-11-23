#include "Engine.h"

#include <functional>
#include <map>

namespace amp
{

std::vector<Block*> flatten (const Chain& blocks)
{
    std::vector<Block*> flat;

    std::function<void (Block*)> add = [&] (Block* block)
    {
        flat.push_back (block);

        for (auto* child : block->children())
            add (child);
    };

    for (auto& b : blocks)
        add (b.get());

    return flat;
}

int tailStart (const juce::Array<juce::var>& chain)
{
    auto typeOf = [&] (int i) { return chain[i].getProperty ("type", {}).toString(); };
    int start = chain.size();

    while (start > 0 && (typeOf (start - 1) == "ir" || typeOf (start - 1) == "eq"))
        --start;

    while (start < chain.size() && typeOf (start) == "eq")
        ++start;  // eq blocks before the first ir stay in the oversampled part

    return start;
}

static juce::Array<juce::var> slice (const juce::Array<juce::var>& chain, int from, int to)
{
    juce::Array<juce::var> part;

    for (int i = from; i < to; ++i)
        part.add (chain[i]);

    return part;
}

Amp::Amp (const juce::var& preset, double baseRate, double coreRate, const juce::File& presetFolder,
          const juce::File& irOverride)
{
    Cfg cfg (preset, "preset");
    cfg.checkKeys ({ "name", "notes", "chain" });

    if (! cfg.has ("chain"))
        throw PresetError ("preset: needs a 'chain' (the list of blocks)");

    name = cfg.text ("name", {});
    const auto chain = cfg.list ("chain");
    const int split = tailStart (chain);

    BuildContext ctx;
    ctx.rate = coreRate;
    ctx.checkRate = baseRate;
    ctx.presetFolder = presetFolder;
    ctx.irOverride = irOverride;
    core = buildChain (slice (chain, 0, split), ctx);

    ctx.rate = baseRate;
    ctx.allowIr = true;
    tail = buildChain (slice (chain, split, chain.size()), ctx);

    link();

    for (auto& part : { &core, &tail })
        for (auto* b : flatten (*part))
        {
            const auto text = b->describe();

            if (text.isNotEmpty())
                report.add ((b->id.isNotEmpty() ? b->id : b->type) + ": " + text);
        }
}

void Amp::link()
{
    // Connect sag blocks to their targets and gate blocks to their apply_after block (link_blocks in chain.py).
    auto blocks = flatten (core);
    const auto numCore = blocks.size();

    for (auto* b : flatten (tail))
        blocks.push_back (b);

    std::set<Block*> inLoops;

    for (auto* b : blocks)
        if (dynamic_cast<Feedback*> (b) != nullptr)
            for (auto* child : b->children())
                inLoops.insert (child);

    std::map<juce::String, size_t> position;

    for (size_t i = 0; i < blocks.size(); ++i)
    {
        const auto& id = blocks[i]->id;

        if (id.isEmpty())
            continue;

        if (position.count (id) != 0)
            throw PresetError ("two blocks use the id '" + id + "'");

        position[id] = i;
    }

    auto find = [&] (const juce::String& kind, size_t i, const juce::String& targetId) -> size_t
    {
        if (position.count (targetId) == 0)
            throw PresetError (kind + ": no block has the id '" + targetId + "'");

        const auto j = position[targetId];

        if (j < i)
            throw PresetError (kind + ": '" + targetId + "' must come after the " + kind + " block in the chain");

        return j;
    };

    for (size_t i = 0; i < blocks.size(); ++i)
    {
        if (auto* sag = dynamic_cast<Sag*> (blocks[i]))
        {
            for (auto& targetId : sag->targetIds)
            {
                auto* target = dynamic_cast<SagTarget*> (blocks[find ("sag", i, targetId)]);

                if (target == nullptr)
                    throw PresetError ("sag: '" + targetId + "' is not a gain or power block");

                sagLinks.emplace_back (sag, target);
            }
        }
        else if (auto* gate = dynamic_cast<Gate*> (blocks[i]); gate != nullptr && gate->applyAfter.isNotEmpty())
        {
            const auto j = find ("gate", i, gate->applyAfter);

            if (inLoops.count (blocks[j]) != 0)
                throw PresetError ("gate: '" + gate->applyAfter + "' is inside a feedback loop; use the feedback block's id");

            if (j >= numCore)
                throw PresetError ("gate: in the plugin, apply_after can not name the cab IR or a block after it");

            gate->hasTarget = true;
            gateLinks.emplace_back (gate, blocks[j]);
        }
    }
}

void Amp::prepare (int maxBaseSamples, int maxCoreSamples)
{
    for (auto* b : flatten (core))
        b->prepare (maxCoreSamples);

    for (auto* b : flatten (tail))
        b->prepare (maxBaseSamples);

    // The buffers exist now, so the links can point at them. A later sag on the same target wins, like in Python.
    for (auto& [sag, target] : sagLinks)
        target->headroom = sag->headroom.data();

    for (auto& [gate, target] : gateLinks)
        target->afterGain = gate->gains.data();

    reset();
}

void Amp::reset() noexcept
{
    for (auto& part : { &core, &tail })
        for (auto* b : flatten (*part))
            b->reset();
}

//==============================================================================
std::vector<double> makeLevelRiff (double rate)
{
    // Four plucked-like notes (E2, A2, D3, G3), a quarter second each, with harmonics that die away.
    const double freqs[] { 82.41, 110.0, 146.83, 196.0 };
    const int noteLength = (int) (0.25 * rate);
    std::vector<double> riff ((size_t) (4 * noteLength), 0.0);
    const double twoPi = juce::MathConstants<double>::twoPi;

    for (int note = 0; note < 4; ++note)
        for (int i = 0; i < noteLength; ++i)
        {
            const double t = i / rate;
            double v = 0.0;

            for (int k = 1; k <= 12; ++k)
                v += std::sin (twoPi * freqs[note] * k * t) / k * std::exp (-t * (4.0 + 1.5 * k));

            riff[(size_t) (note * noteLength + i)] = v;
        }

    double peak = 0.0;

    for (auto v : riff)
        peak = std::max (peak, std::abs (v));

    for (auto& v : riff)
        v *= 0.5 / peak;

    return riff;
}

Engine::Engine (const Options& options)
    : amp (options.preset, options.sampleRate, options.sampleRate * (double) (1 << options.factorLog2),
           options.presetFolder, options.irOverride),
      rate (options.sampleRate),
      blockSize (options.maxBlock),
      factorLog2 (options.factorLog2),
      oversampling (1, (size_t) options.factorLog2, juce::dsp::Oversampling<double>::filterHalfBandPolyphaseIIR, true, true)
{
    oversampling.initProcessing ((size_t) blockSize);
    amp.prepare (blockSize, blockSize << factorLog2);

    if (options.fixedLevel > 0.0)
    {
        makeup = options.fixedLevel;
    }
    else if (options.autoLevel)
    {
        // Play a short riff through the amp, then set the output so it averages -20 dBFS.
        auto riff = makeLevelRiff (rate);
        double sumSquares = 0.0;

        for (size_t start = 0; start < riff.size(); start += (size_t) blockSize)
        {
            const int n = (int) std::min ((size_t) blockSize, riff.size() - start);
            process (riff.data() + start, n);

            for (int i = 0; i < n; ++i)
                sumSquares += riff[start + (size_t) i] * riff[start + (size_t) i];
        }

        const double rms = std::sqrt (sumSquares / (double) riff.size());
        makeup = rms > 1e-9 && std::isfinite (rms) ? dbToGain (-20.0) / rms : 1.0;
    }

    reset();
}

void Engine::process (double* x, int n) noexcept
{
    double* channels[] { x };
    juce::dsp::AudioBlock<double> block (channels, 1, (size_t) n);
    auto up = oversampling.processSamplesUp (block);
    amp.processCore (up.getChannelPointer (0), (int) up.getNumSamples());
    oversampling.processSamplesDown (block);
    amp.processTail (x, n);

    for (int i = 0; i < n; ++i)
        x[i] *= makeup;
}

void Engine::reset() noexcept
{
    oversampling.reset();
    amp.reset();
}

} // namespace amp
