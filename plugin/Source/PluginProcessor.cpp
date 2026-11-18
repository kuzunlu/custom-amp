#include "PluginProcessor.h"

#include "BinaryData.h"
#include "PluginEditor.h"

static const juce::String defaultPreset = "brit_800.json";

CustomAmpProcessor::CustomAmpProcessor()
    : AudioProcessor (BusesProperties().withInput ("Input", juce::AudioChannelSet::stereo(), true)
                                       .withOutput ("Output", juce::AudioChannelSet::stereo(), true)),
      parameters (*this, nullptr, "PARAMETERS", createParameterLayout())
{
    inputParam = parameters.getRawParameterValue ("input_db");
    outputParam = parameters.getRawParameterValue ("output_db");
    qualityParam = parameters.getRawParameterValue ("quality");

    for (auto& p : getBuiltInPresets())
        if (p.fileName == defaultPreset)
            loadBuiltInPreset (p.fileName);

    builder.startThread();
    startTimer (100);
}

CustomAmpProcessor::~CustomAmpProcessor()
{
    stopTimer();
    builder.signalThreadShouldExit();
    builder.request ({});  // wakes it up
    builder.stopThread (10000);
    delete pendingEngine.exchange (nullptr);
    delete retiredEngine.exchange (nullptr);
}

juce::AudioProcessorValueTreeState::ParameterLayout CustomAmpProcessor::createParameterLayout()
{
    // Names, ranges and defaults match the "plugin" section of shared/settings.json.
    juce::AudioProcessorValueTreeState::ParameterLayout layout;
    layout.add (std::make_unique<juce::AudioParameterFloat> (juce::ParameterID { "input_db", 1 }, "Input",
                                                             juce::NormalisableRange<float> (-24.0f, 24.0f, 0.1f), 0.0f,
                                                             juce::AudioParameterFloatAttributes().withLabel ("dB")));
    layout.add (std::make_unique<juce::AudioParameterFloat> (juce::ParameterID { "output_db", 1 }, "Output",
                                                             juce::NormalisableRange<float> (-36.0f, 12.0f, 0.1f), 0.0f,
                                                             juce::AudioParameterFloatAttributes().withLabel ("dB")));
    layout.add (std::make_unique<juce::AudioParameterChoice> (juce::ParameterID { "quality", 1 }, "Quality",
                                                              juce::StringArray { "2x", "4x", "8x" }, 2));
    return layout;
}

bool CustomAmpProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    auto ok = [] (const juce::AudioChannelSet& set)
    {
        return set == juce::AudioChannelSet::mono() || set == juce::AudioChannelSet::stereo();
    };

    return ok (layouts.getMainInputChannelSet()) && ok (layouts.getMainOutputChannelSet());
}

int CustomAmpProcessor::factorLog2ForRate (double rate) const
{
    // The quality knob is the oversampling at 44.1/48 kHz. Higher session rates need less of it,
    // so the amp always runs at about the same rate and sounds the same.
    int factor = (int) qualityParam->load() + 1;

    if (rate >= 88200.0)
        --factor;

    if (rate >= 176400.0)
        --factor;

    return juce::jmax (0, factor);
}

//==============================================================================
juce::Array<CustomAmpProcessor::BuiltInPreset> CustomAmpProcessor::getBuiltInPresets()
{
    juce::Array<BuiltInPreset> list;

    for (int i = 0; i < BinaryData::namedResourceListSize; ++i)
    {
        const juce::String fileName (BinaryData::originalFilenames[i]);

        if (! fileName.endsWith (".json") || fileName == "settings.json")
            continue;

        int size = 0;
        const char* data = BinaryData::getNamedResource (BinaryData::namedResourceList[i], size);
        auto preset = juce::JSON::parse (juce::String::fromUTF8 (data, size));
        list.add ({ fileName, preset.getProperty ("name", fileName).toString() });
    }

    std::sort (list.begin(), list.end(), [] (auto& a, auto& b) { return a.name < b.name; });
    return list;
}

void CustomAmpProcessor::setPresetText (const juce::String& source, const juce::String& text)
{
    presetSource = source;
    presetText = text;
    requestBuild();
}

void CustomAmpProcessor::loadBuiltInPreset (const juce::String& fileName)
{
    for (int i = 0; i < BinaryData::namedResourceListSize; ++i)
    {
        if (fileName != BinaryData::originalFilenames[i])
            continue;

        int size = 0;
        const char* data = BinaryData::getNamedResource (BinaryData::namedResourceList[i], size);
        setPresetText ("builtin:" + fileName, juce::String::fromUTF8 (data, size));
        return;
    }
}

juce::File CustomAmpProcessor::getPresetFile() const
{
    return presetSource.startsWith ("file:") ? juce::File (presetSource.fromFirstOccurrenceOf ("file:", false, false))
                                             : juce::File();
}

void CustomAmpProcessor::loadPresetFile (const juce::File& file)
{
    presetFileTime = file.getLastModificationTime();
    setPresetText ("file:" + file.getFullPathName(), file.loadFileAsString());
}

void CustomAmpProcessor::reloadPreset()
{
    if (getPresetFile().existsAsFile())
        loadPresetFile (getPresetFile());
    else if (presetSource.startsWith ("builtin:"))
        loadBuiltInPreset (presetSource.fromFirstOccurrenceOf ("builtin:", false, false));
    else
        requestBuild();
}

void CustomAmpProcessor::setIrFile (const juce::File& file)
{
    irFile = file;
    requestBuild();
}

juce::var CustomAmpProcessor::getPresetData() const
{
    juce::var preset;
    return juce::JSON::parse (presetText, preset).wasOk() ? preset : juce::var();
}

CustomAmpProcessor::Status CustomAmpProcessor::getStatus() const
{
    const juce::ScopedLock sl (statusLock);
    return status;
}

//==============================================================================
CustomAmpProcessor::BuildRequest CustomAmpProcessor::makeRequest()
{
    BuildRequest r;
    r.presetText = presetText;
    r.presetFolder = getPresetFile().getParentDirectory();
    r.irFile = irFile;
    r.sampleRate = currentRate;
    r.maxBlock = currentBlock;
    r.factorLog2 = factorLog2ForRate (currentRate);
    builtQuality = (int) qualityParam->load();
    return r;
}

void CustomAmpProcessor::requestBuild()
{
    if (currentRate <= 0.0)
        return;  // prepareToPlay builds the first engine

    const juce::ScopedLock sl (postLock);
    auto r = makeRequest();
    r.generation = ++latestGeneration;
    builder.request (r);
}

std::unique_ptr<amp::Engine> CustomAmpProcessor::buildEngine (const BuildRequest& r, juce::String& message)
{
    try
    {
        juce::var preset;
        auto parsed = juce::JSON::parse (r.presetText, preset);

        if (parsed.failed())
            throw amp::PresetError ("the preset is not valid JSON: " + parsed.getErrorMessage());

        amp::Engine::Options options;
        options.preset = preset;
        options.presetFolder = r.presetFolder;
        options.irOverride = r.irFile;
        options.sampleRate = r.sampleRate;
        options.maxBlock = r.maxBlock;
        options.factorLog2 = r.factorLog2;
        auto built = std::make_unique<amp::Engine> (options);

        juce::StringArray lines;
        lines.add (built->amp.name.isNotEmpty() ? built->amp.name : juce::String ("(no name)"));
        lines.add ("Runs at " + juce::String (built->coreRate() / 1000.0, 1) + " kHz, latency "
                   + juce::String (built->latencySamples()) + " samples ("
                   + juce::String (1000.0 * built->latencySamples() / r.sampleRate, 2) + " ms), auto level "
                   + juce::String (juce::Decibels::gainToDecibels (built->makeup), 1) + " dB");
        lines.addArray (built->amp.report);
        message = lines.joinIntoString ("\n");
        return built;
    }
    catch (const std::exception& e)
    {
        message = e.what();
        return {};
    }
}

void CustomAmpProcessor::engineBuilt (std::unique_ptr<amp::Engine> built, const BuildRequest& r, const juce::String& message)
{
    const bool ok = built != nullptr;

    {
        const juce::ScopedLock sl (postLock);

        if (r.generation != latestGeneration)
            return;  // something newer was asked for (or the sample rate changed) meanwhile

        if (built != nullptr)
        {
            newLatency = built->latencySamples();
            delete pendingEngine.exchange (built.release());  // an engine the audio thread never took
        }
    }

    const juce::ScopedLock sl (statusLock);
    status = ok ? Status { false, message } : Status { true, message + "\n\nStill playing the last preset that worked." };
    statusChanged = true;
}

void CustomAmpProcessor::Builder::request (BuildRequest r)
{
    {
        const juce::ScopedLock sl (lock);
        next = std::move (r);
    }

    wakeUp.signal();
}

void CustomAmpProcessor::Builder::run()
{
    while (! threadShouldExit())
    {
        wakeUp.wait (-1);
        std::optional<BuildRequest> r;

        {
            const juce::ScopedLock sl (lock);
            std::swap (r, next);
        }

        if (! r.has_value() || threadShouldExit() || r->presetText.isEmpty())
            continue;

        juce::String message;
        auto built = buildEngine (*r, message);
        owner.engineBuilt (std::move (built), *r, message);
    }
}

void CustomAmpProcessor::timerCallback()
{
    delete retiredEngine.exchange (nullptr);

    if (auto latency = newLatency.exchange (-1); latency >= 0 && latency != getLatencySamples())
        setLatencySamples (latency);

    if (statusChanged.exchange (false))
        sendChangeMessage();

    if ((int) qualityParam->load() != builtQuality)
        requestBuild();

    // A preset loaded from a file is loaded again when the file changes (edit it in any text editor).
    auto file = getPresetFile();

    if (file.existsAsFile() && file.getLastModificationTime() != presetFileTime)
        loadPresetFile (file);
}

//==============================================================================
void CustomAmpProcessor::prepareToPlay (double sampleRate, int samplesPerBlock)
{
    const juce::ScopedLock sl (postLock);
    ++latestGeneration;  // engines still being built for the old settings are thrown away
    delete pendingEngine.exchange (nullptr);
    delete retiredEngine.exchange (nullptr);

    currentRate = sampleRate;
    currentBlock = juce::jmax (32, samplesPerBlock);
    mono.assign ((size_t) currentBlock, 0.0);
    inputGain.reset (sampleRate, 0.02);
    outputGain.reset (sampleRate, 0.02);

    juce::String message;
    engine = buildEngine (makeRequest(), message);

    if (engine != nullptr)
        setLatencySamples (engine->latencySamples());

    const juce::ScopedLock sl2 (statusLock);
    status = engine == nullptr ? Status { true, message } : Status { false, message };
    statusChanged = true;
}

void CustomAmpProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    juce::ScopedNoDenormals noDenormals;

    // Take a new engine if one is waiting (and the message thread has deleted the last old one).
    if (retiredEngine.load() == nullptr)
        if (auto* fresh = pendingEngine.exchange (nullptr))
        {
            retiredEngine = engine.release();
            engine.reset (fresh);
        }

    const int numIn = getTotalNumInputChannels(), numOut = getTotalNumOutputChannels();
    const int numSamples = buffer.getNumSamples();

    if (engine == nullptr || numIn == 0)
    {
        buffer.clear();
        return;
    }

    inputGain.setTargetValue (juce::Decibels::decibelsToGain ((double) inputParam->load()));
    outputGain.setTargetValue (juce::Decibels::decibelsToGain ((double) outputParam->load()));

    for (int start = 0; start < numSamples; start += engine->maxBlock())
    {
        const int n = juce::jmin (engine->maxBlock(), numSamples - start);

        // The amp is mono: the average of the input channels.
        for (int i = 0; i < n; ++i)
        {
            double sum = 0.0;

            for (int ch = 0; ch < numIn; ++ch)
                sum += (double) buffer.getSample (ch, start + i);

            mono[(size_t) i] = sum / numIn * inputGain.getNextValue();
        }

        engine->process (mono.data(), n);
        bool finite = true;

        for (int i = 0; i < n; ++i)
            finite = finite && std::isfinite (mono[(size_t) i]);

        if (! finite)  // should never happen; if it does, start clean instead of making a loud noise
        {
            engine->reset();
            std::fill (mono.begin(), mono.begin() + n, 0.0);
        }

        for (int i = 0; i < n; ++i)
        {
            const auto v = (float) (mono[(size_t) i] * outputGain.getNextValue());

            for (int ch = 0; ch < numOut; ++ch)
                buffer.setSample (ch, start + i, v);
        }
    }
}

//==============================================================================
juce::AudioProcessorEditor* CustomAmpProcessor::createEditor()
{
    return new CustomAmpEditor (*this);
}

void CustomAmpProcessor::getStateInformation (juce::MemoryBlock& destData)
{
    juce::ValueTree state ("CustomAmp");
    state.setProperty ("presetSource", presetSource, nullptr);
    state.setProperty ("presetText", presetText, nullptr);
    state.setProperty ("irFile", irFile.getFullPathName(), nullptr);
    state.appendChild (parameters.copyState(), nullptr);

    if (auto xml = state.createXml())
        copyXmlToBinary (*xml, destData);
}

void CustomAmpProcessor::setStateInformation (const void* data, int sizeInBytes)
{
    auto xml = getXmlFromBinary (data, sizeInBytes);

    if (xml == nullptr)
        return;

    auto state = juce::ValueTree::fromXml (*xml);

    if (! state.hasType ("CustomAmp"))
        return;

    if (auto params = state.getChildWithName (parameters.state.getType()); params.isValid())
        parameters.replaceState (params);

    const auto path = state.getProperty ("irFile").toString();
    irFile = path.isNotEmpty() ? juce::File (path) : juce::File();
    presetSource = state.getProperty ("presetSource").toString();
    presetText = state.getProperty ("presetText").toString();

    // A preset file that still exists is read again (it may have been edited since); otherwise the saved copy is used.
    if (getPresetFile().existsAsFile())
        loadPresetFile (getPresetFile());
    else
        requestBuild();
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new CustomAmpProcessor();
}
