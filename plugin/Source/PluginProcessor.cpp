#include "PluginProcessor.h"

#include "BinaryData.h"
#include "PluginEditor.h"

static const juce::String defaultPreset = "brit_800.json";
static constexpr double fadeSeconds = 0.03;    // old and new engine cross-fade this long
static constexpr double warmUpSeconds = 0.25;  // a new engine hears this much of the recent input first
static constexpr size_t maxUndoSteps = 200;
static constexpr double knobTolerance = 0.01;  // a knob (0-10) this close to its preset value has not moved
static constexpr juce::uint32 mergeKnobMs = 800;  // moves of the same knob closer than this are one undo step

CustomAmpProcessor::CustomAmpProcessor()
    : AudioProcessor (BusesProperties().withInput ("Input", juce::AudioChannelSet::stereo(), true)
                                       .withOutput ("Output", juce::AudioChannelSet::stereo(), true)),
      parameters (*this, nullptr, "PARAMETERS", createParameterLayout())
{
    inputParam = parameters.getRawParameterValue ("input_db");
    outputParam = parameters.getRawParameterValue ("output_db");
    qualityParam = parameters.getRawParameterValue ("quality");

    for (int i = 0; i < panel::maxKnobs; ++i)
        knobParams[(size_t) i] = parameters.getRawParameterValue ("knob_" + juce::String (i + 1));

    for (auto& p : getBuiltInPresets())
        if (p.fileName == defaultPreset)
            loadBuiltInPreset (p.fileName);

    builder.startThread();
    twinBuilder.startThread();
    startTimer (100);
}

CustomAmpProcessor::~CustomAmpProcessor()
{
    stopTimer();
    builder.signalThreadShouldExit();
    builder.request ({});  // wakes it up
    builder.stopThread (10000);
    twinBuilder.signalThreadShouldExit();
    twinBuilder.request ({});
    twinBuilder.stopThread (10000);
    delete pendingEngine.exchange (nullptr);
    delete retiredEngine.exchange (nullptr);
    delete pendingTwin.exchange (nullptr);
    delete retiredTwin.exchange (nullptr);
}

//==============================================================================
juce::String CustomAmpProcessor::twinPreset (const juce::String& text, const juce::StringArray& blockPath)
{
    auto preset = juce::JSON::parse (text);

    if (! preset.isObject() || blockPath.isEmpty())
        return {};

    // The steps lead to the list; the last one is the block's place in it (like a panel knob's path).
    auto list = preset;

    for (int i = 0; i + 1 < blockPath.size(); ++i)
        list = list.isArray() ? list[blockPath[i].getIntValue()] : list.getProperty (juce::Identifier (blockPath[i]), {});

    const int index = blockPath[blockPath.size() - 1].getIntValue();
    auto* array = list.getArray();

    if (array == nullptr || ! juce::isPositiveAndBelow (index, array->size()))
        return {};

    auto block = (*array)[index];
    const auto type = block.getProperty ("type", {}).toString();
    auto* obj = block.getDynamicObject();

    if (obj == nullptr)
        return {};

    if (type == "gain" || type == "power")
        obj->setProperty ("linear", true);  // the same stage without its non-linear parts
    else if (type == "sag" || type == "feedback")
        obj->setProperty ("amount", 0.0);
    else if (type == "eq" || type == "tonestack" || type == "gate" || type == "ir" || type == "room" || type == "strings" || type == "restring" || type == "bow" || type == "mouth" || type == "body")
        array->remove (index);
    else
        return {};  // a split: no single "off"

    if (auto* root = preset.getDynamicObject())
        root->removeProperty ("panel");  // it does not change the sound, and it may name a block that is gone

    return juce::JSON::toString (preset, true);
}

void CustomAmpProcessor::xray (const juce::StringArray& blockPath, Listen mode)
{
    if (twinPreset (presetText, blockPath).isEmpty())
        return;

    auto list = getPresetData();

    for (int i = 0; i + 1 < blockPath.size(); ++i)
        list = list.isArray() ? list[blockPath[i].getIntValue()] : list.getProperty (juce::Identifier (blockPath[i]), {});

    const auto block = list[blockPath[blockPath.size() - 1].getIntValue()];
    const bool sameBlock = blockPath == xrayPath;
    xrayPath = blockPath;
    xrayName = block.getProperty ("id", block.getProperty ("type", "block")).toString();
    listenMode = (int) mode;
    twinWanted = true;

    if (! sameBlock || twin == nullptr)
        requestTwinBuild();

    statusChanged = true;
}

void CustomAmpProcessor::stopXray()
{
    {
        const juce::ScopedLock sl (postLock);
        ++latestTwinGeneration;  // twins still being built are thrown away
    }

    xrayPath.clear();
    xrayName = {};
    listenMode = (int) Listen::normal;
    twinWanted = false;
    statusChanged = true;
}

void CustomAmpProcessor::requestTwinBuild()
{
    if (xrayPath.isEmpty() || currentRate <= 0.0)
        return;

    const auto text = twinPreset (presetText, xrayPath);

    if (text.isEmpty())  // the block is gone (an edit or another preset)
    {
        stopXray();
        return;
    }

    const juce::ScopedLock sl (postLock);
    auto r = makeRequest (false);  // the amp's own output level, so amp minus twin is exact
    r.presetText = text;
    r.twin = true;
    r.warmUp = true;
    r.generation = ++latestTwinGeneration;
    twinBuilder.request (r);
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

    // The front panel's knobs. Their meaning changes with the preset, so the DAW only knows them by number.
    for (int i = 1; i <= panel::maxKnobs; ++i)
        layout.add (std::make_unique<juce::AudioParameterFloat> (juce::ParameterID { "knob_" + juce::String (i), 1 },
                                                                 "Knob " + juce::String (i),
                                                                 juce::NormalisableRange<float> (0.0f, 10.0f), 5.0f));
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

void CustomAmpProcessor::setPresetText (const juce::String& source, const juce::String& text, bool measure, bool newStep)
{
    if (newStep && presetText.isNotEmpty() && (source != presetSource || text != presetText))
    {
        undoSteps.push_back ({ presetSource, presetText, currentLevel.load() });

        if (undoSteps.size() > maxUndoSteps)
            undoSteps.erase (undoSteps.begin());
    }

    redoSteps.clear();
    presetSource = source;
    presetText = text;

    if (! applyingKnob)
        lastKnobEdited = -1;

    updatePanel();
    requestBuild (measure);
    requestTwinBuild();  // X-ray follows the edits
}

void CustomAmpProcessor::updatePanel()
{
    const auto preset = getPresetData();
    panelKnobs.clear();
    knobPositions.clear();

    try
    {
        panelKnobs = panel::read (preset, help, panelGuessed);
    }
    catch (const std::exception&)
    {
        // a mistake in the panel: the build shows it, and there are no knobs meanwhile
    }

    // The knobs show the preset's values (the DAW sees the knob parameters move).
    for (size_t i = 0; i < panelKnobs.size(); ++i)
    {
        const double position = panelKnobs[i].toPosition (panel::getValue (preset, panelKnobs[i]));
        knobPositions.push_back (position);

        if (std::abs (knobParams[i]->load() - position) > knobTolerance)
            if (auto* param = parameters.getParameter ("knob_" + juce::String ((int) i + 1)))
                param->setValueNotifyingHost (param->convertTo0to1 ((float) position));
    }
}

void CustomAmpProcessor::applyKnobChanges()
{
    // A knob that is not where the preset puts it was moved (in the window or by the DAW): write it into the preset.
    juce::var preset;
    int moved = 0, which = -1;

    for (size_t i = 0; i < panelKnobs.size(); ++i)
    {
        const double position = knobParams[i]->load();

        if (std::abs (position - knobPositions[i]) <= knobTolerance)
            continue;

        if (moved == 0)
            preset = getPresetData();

        if (! preset.isObject())
            return;

        panel::setValue (preset, panelKnobs[i], panelKnobs[i].toValue (position));
        knobPositions[i] = position;
        which = (int) i;
        ++moved;
    }

    if (moved == 0)
        return;

    const auto now = juce::Time::getMillisecondCounter();
    const bool merge = moved == 1 && which == lastKnobEdited && now - lastKnobTime < mergeKnobMs;
    applyingKnob = true;
    setEditedPreset (juce::JSON::toString (preset, true), merge);
    applyingKnob = false;
    lastKnobEdited = moved == 1 ? which : -1;
    lastKnobTime = now;
    statusChanged = true;  // the window shows the new value in the chain
}

void CustomAmpProcessor::undo()
{
    if (undoSteps.empty())
        return;

    redoSteps.push_back ({ presetSource, presetText, currentLevel.load() });
    const auto step = undoSteps.back();
    undoSteps.pop_back();
    restore (step);
}

void CustomAmpProcessor::redo()
{
    if (redoSteps.empty())
        return;

    undoSteps.push_back ({ presetSource, presetText, currentLevel.load() });
    const auto step = redoSteps.back();
    redoSteps.pop_back();
    restore (step);
}

void CustomAmpProcessor::restore (const Step& step)
{
    presetSource = step.source;
    presetText = step.text;

    // A file that says something else by now: this text is no longer the file, so it counts as edited
    // (otherwise the file watcher would replace it). If it still is the file, the watcher starts from now.
    if (auto file = getPresetFile(); ! isEdited() && file != juce::File())
    {
        if (file.loadFileAsString() == step.text)
            presetFileTime = file.getLastModificationTime();
        else
            presetSource = "edited:" + presetSource;
    }

    if (step.level > 0.0)
        currentLevel = step.level;  // the level it had then

    lastKnobEdited = -1;
    updatePanel();
    requestBuild (step.level <= 0.0);
    statusChanged = true;
}

void CustomAmpProcessor::loadBuiltInPreset (const juce::String& fileName)
{
    for (int i = 0; i < BinaryData::namedResourceListSize; ++i)
    {
        if (fileName != BinaryData::originalFilenames[i])
            continue;

        int size = 0;
        const char* data = BinaryData::getNamedResource (BinaryData::namedResourceList[i], size);
        setPresetText ("builtin:" + fileName, juce::String::fromUTF8 (data, size), true);
        return;
    }
}

juce::File CustomAmpProcessor::getPresetFile() const
{
    const auto source = isEdited() ? presetSource.fromFirstOccurrenceOf ("edited:", false, false) : presetSource;
    return source.startsWith ("file:") ? juce::File (source.fromFirstOccurrenceOf ("file:", false, false)) : juce::File();
}

void CustomAmpProcessor::readPresetFile (const juce::File& file, bool measure)
{
    presetFileTime = file.getLastModificationTime();
    setPresetText ("file:" + file.getFullPathName(), file.loadFileAsString(), measure);
}

void CustomAmpProcessor::loadPresetFile (const juce::File& file)
{
    readPresetFile (file, true);
}

void CustomAmpProcessor::reloadPreset()
{
    const auto source = isEdited() ? presetSource.fromFirstOccurrenceOf ("edited:", false, false) : presetSource;

    if (getPresetFile().existsAsFile())
        loadPresetFile (getPresetFile());
    else if (source.startsWith ("builtin:"))
        loadBuiltInPreset (source.fromFirstOccurrenceOf ("builtin:", false, false));
    else
        requestBuild (true);
}

void CustomAmpProcessor::setIrFile (const juce::File& file)
{
    irFile = file;
    requestBuild (true);  // cabs differ a lot in loudness
}

void CustomAmpProcessor::setEditedPreset (const juce::String& text, bool mergeWithLast)
{
    setPresetText (isEdited() ? presetSource : "edited:" + presetSource, text, false, ! mergeWithLast);
}

bool CustomAmpProcessor::saveEditedPreset (const juce::File& file)
{
    auto data = getPresetData();
    const auto text = data.isObject() ? juce::JSON::toString (data) : presetText;

    if (! file.replaceWithText (text + "\n"))
        return false;

    presetFileTime = file.getLastModificationTime();
    presetSource = "file:" + file.getFullPathName();
    presetText = text;
    updatePanel();
    statusChanged = true;
    return true;
}

void CustomAmpProcessor::measureLevel()
{
    requestBuild (true);
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
CustomAmpProcessor::BuildRequest CustomAmpProcessor::makeRequest (bool measure)
{
    BuildRequest r;
    r.presetText = presetText;
    r.presetFolder = getPresetFile().getParentDirectory();
    r.irFile = irFile;
    r.sampleRate = currentRate;
    r.maxBlock = currentBlock;
    r.factorLog2 = factorLog2ForRate (currentRate);
    r.fixedLevel = measure ? 0.0 : currentLevel.load();
    builtQuality = (int) qualityParam->load();
    return r;
}

amp::Engine::Options CustomAmpProcessor::getSignalOptions() const
{
    amp::Engine::Options options;
    options.presetFolder = getPresetFile().getParentDirectory();
    options.irOverride = irFile;
    options.sampleRate = currentRate > 0.0 ? currentRate : 48000.0;
    options.factorLog2 = factorLog2ForRate (options.sampleRate);
    options.fixedLevel = 1.0;
    return options;
}

std::vector<double> CustomAmpProcessor::getRecentInput (double seconds) const
{
    const int size = (int) history.size();
    const int count = juce::jlimit (1, size - 1, (int) (seconds * (currentRate > 0.0 ? currentRate : 48000.0)));
    const int end = historyWrite.load();
    std::vector<double> x ((size_t) count);

    for (int i = 0; i < count; ++i)
        x[(size_t) i] = history[(size_t) ((end - count + i + size) % size)].load (std::memory_order_relaxed);

    return x;
}

void CustomAmpProcessor::requestBuild (bool measure)
{
    if (currentRate <= 0.0)
        return;  // prepareToPlay builds the first engine

    const juce::ScopedLock sl (postLock);
    auto r = makeRequest (measure);
    r.generation = ++latestGeneration;
    r.warmUp = true;
    builder.request (r);
}

std::unique_ptr<amp::Engine> CustomAmpProcessor::buildEngine (const BuildRequest& r, juce::String& message) const
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
        options.fixedLevel = r.fixedLevel;
        auto built = std::make_unique<amp::Engine> (options);
        bool guessed = false;
        panel::read (preset, help, guessed);  // a mistake in the panel stops the preset like any other

        if (r.warmUp)
        {
            // Play the last moments of input through it, so its filters and tubes are already in the right
            // state when it takes over. The input keeps coming meanwhile, so catch up until less than one
            // block is missing; the audio thread plays it that last bit just before the swap.
            const int size = (int) history.size();
            int from = historyWrite.load() - juce::jmin (size / 2, (int) (warmUpSeconds * r.sampleRate));
            std::vector<double> chunk ((size_t) r.maxBlock);

            for (int round = 0; round < 8; ++round)
            {
                const int to = historyWrite.load();
                int count = (to - from + 2 * size) % size;

                if (round > 0 && count <= r.maxBlock)
                    break;

                for (; count > 0; count -= r.maxBlock)
                {
                    const int n = juce::jmin (r.maxBlock, count);

                    for (int i = 0; i < n; ++i)
                        chunk[(size_t) i] = history[(size_t) ((from + i + 2 * size) % size)].load (std::memory_order_relaxed);

                    built->process (chunk.data(), n);
                    from = (from + n + size) % size;
                }
            }

            built->heardUpTo = (from + size) % size;
        }

        juce::StringArray lines;
        lines.add (built->amp.name.isNotEmpty() ? built->amp.name : juce::String ("(no name)"));
        lines.add ("Runs at " + juce::String (built->coreRate() / 1000.0, 1) + " kHz, latency "
                   + juce::String (built->latencySamples()) + " samples ("
                   + juce::String (1000.0 * built->latencySamples() / r.sampleRate, 2) + " ms), "
                   + (! built->amp.autoLevel ? "auto level off: " : r.fixedLevel > 0.0 ? "level kept at " : "auto level ")
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

    if (r.twin)
    {
        const juce::ScopedLock sl (postLock);

        if (r.generation == latestTwinGeneration && built != nullptr)
            delete pendingTwin.exchange (built.release());

        return;
    }

    {
        const juce::ScopedLock sl (postLock);

        if (r.generation != latestGeneration)
            return;  // something newer was asked for (or the sample rate changed) meanwhile

        if (built != nullptr)
        {
            newLatency = built->latencySamples();

            if (built->makeup != currentLevel.load())
                twinStale = true;  // the twin must play at the amp's new level

            currentLevel = built->makeup;
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
        auto built = owner.buildEngine (*r, message);
        owner.engineBuilt (std::move (built), *r, message);
    }
}

void CustomAmpProcessor::timerCallback()
{
    delete retiredEngine.exchange (nullptr);
    delete retiredTwin.exchange (nullptr);

    if (twinStale.exchange (false))
        requestTwinBuild();
    applyKnobChanges();

    if (auto latency = newLatency.exchange (-1); latency >= 0 && latency != getLatencySamples())
        setLatencySamples (latency);

    if (statusChanged.exchange (false))
        sendChangeMessage();

    if ((int) qualityParam->load() != builtQuality)
    {
        requestBuild (false);
        requestTwinBuild();
    }

    // A preset loaded from a file is loaded again when the file changes (edit it in any text editor).
    // This counts as an edit: the level is kept. Not after edits in the plugin window, which would be lost.
    auto file = getPresetFile();

    if (! isEdited() && file.existsAsFile() && file.getLastModificationTime() != presetFileTime)
    {
        readPresetFile (file, false);
        statusChanged = true;
    }
}

//==============================================================================
void CustomAmpProcessor::prepareToPlay (double sampleRate, int samplesPerBlock)
{
    const juce::ScopedLock sl (postLock);
    ++latestGeneration;  // engines still being built for the old settings are thrown away
    delete pendingEngine.exchange (nullptr);
    delete retiredEngine.exchange (nullptr);
    fadingEngine.reset();
    ++latestTwinGeneration;
    delete pendingTwin.exchange (nullptr);
    delete retiredTwin.exchange (nullptr);
    twin.reset();
    twinStale = ! xrayPath.isEmpty();  // the timer builds it again for the new rate

    currentRate = sampleRate;
    currentBlock = juce::jmax (32, samplesPerBlock);
    mono.assign ((size_t) currentBlock, 0.0);
    monoOld.assign ((size_t) currentBlock, 0.0);
    twinBuffer.assign ((size_t) currentBlock, 0.0);
    fadeLength = juce::jmax (1, (int) (fadeSeconds * sampleRate));
    inputGain.reset (sampleRate, 0.02);
    outputGain.reset (sampleRate, 0.02);
    tuner.prepare (sampleRate, 0.001);  // the room's tracker (steadier than restring's quick one); quieter = no note

    juce::String message;
    engine = buildEngine (makeRequest (currentLevel.load() <= 0.0), message);

    if (engine != nullptr)
    {
        setLatencySamples (engine->latencySamples());
        currentLevel = engine->makeup;
    }

    const juce::ScopedLock sl2 (statusLock);
    status = engine == nullptr ? Status { true, message } : Status { false, message };
    statusChanged = true;
}

void CustomAmpProcessor::catchUp (amp::Engine& fresh) noexcept
{
    // Play it the few samples that arrived after its warm-up, so it has heard exactly what the old one heard.
    const int size = (int) history.size();
    const int now = historyWrite.load (std::memory_order_relaxed);
    int missing = fresh.heardUpTo >= 0 ? (now - fresh.heardUpTo + size) % size : 0;

    if (missing <= 4 * fresh.maxBlock())
        for (int from = fresh.heardUpTo; missing > 0;)
        {
            const int n = juce::jmin (missing, fresh.maxBlock());

            for (int i = 0; i < n; ++i)
                monoOld[(size_t) i] = history[(size_t) ((from + i) % size)].load (std::memory_order_relaxed);

            fresh.process (monoOld.data(), n);
            from = (from + n) % size;
            missing -= n;
        }
}

void CustomAmpProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    juce::ScopedNoDenormals noDenormals;
    const auto started = juce::Time::getHighResolutionTicks();

    // An old engine that has faded out goes back to the message thread to be deleted.
    if (fadingEngine != nullptr && fadePos >= fadeLength && retiredEngine.load() == nullptr)
        retiredEngine = fadingEngine.release();

    // Take a new engine if one is waiting; the current one fades out under it.
    if (fadingEngine == nullptr)
        if (auto* fresh = pendingEngine.exchange (nullptr))
        {
            catchUp (*fresh);
            fadingEngine = std::move (engine);
            engine.reset (fresh);
            fadePos = fadingEngine != nullptr ? 0 : fadeLength;
        }

    // X-ray: drop the twin when it is off; take a new one when it is ready (the old one goes back to be deleted).
    if (! twinWanted.load() && twin != nullptr && retiredTwin.load() == nullptr)
        retiredTwin = twin.release();

    if (twinWanted.load() && retiredTwin.load() == nullptr)
        if (auto* fresh = pendingTwin.exchange (nullptr))
        {
            catchUp (*fresh);
            retiredTwin = twin.release();
            twin.reset (fresh);
            twinFade = 0;
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
    const int size = (int) history.size();
    const bool tunerOn = meters.tunerOn.load (std::memory_order_relaxed);
    float rawPeak = 0, inPeak = 0, outPeak = 0;

    for (int start = 0; start < numSamples; start += engine->maxBlock())
    {
        const int n = juce::jmin (engine->maxBlock(), numSamples - start);
        int write = historyWrite.load (std::memory_order_relaxed);

        // The amp is mono: the average of the input channels.
        for (int i = 0; i < n; ++i)
        {
            double sum = 0.0;

            for (int ch = 0; ch < numIn; ++ch)
            {
                const float s = buffer.getSample (ch, start + i);
                rawPeak = juce::jmax (rawPeak, std::abs (s));
                sum += (double) s;
            }

            mono[(size_t) i] = sum / numIn * inputGain.getNextValue();
            inPeak = juce::jmax (inPeak, (float) std::abs (mono[(size_t) i]));

            if (tunerOn && tuner.hear (mono[(size_t) i]))
                meters.noteHz.store ((float) (currentRate / tuner.period()), std::memory_order_relaxed);
            history[(size_t) write].store ((float) mono[(size_t) i], std::memory_order_relaxed);
            write = (write + 1) % size;
        }

        historyWrite.store (write);
        const bool fading = fadingEngine != nullptr && fadePos < fadeLength;

        if (twin != nullptr)
            std::copy (mono.begin(), mono.begin() + n, twinBuffer.begin());

        if (fading)
            std::copy (mono.begin(), mono.begin() + n, monoOld.begin());

        engine->process (mono.data(), n);

        if (fading)
        {
            fadingEngine->process (monoOld.data(), n);

            for (int i = 0; i < n; ++i)
            {
                const double g = juce::jmin (1.0, (double) fadePos++ / fadeLength);
                mono[(size_t) i] = g * mono[(size_t) i] + (1.0 - g) * monoOld[(size_t) i];
            }
        }

        bool finite = true;

        for (int i = 0; i < n; ++i)
            finite = finite && std::isfinite (mono[(size_t) i]);

        if (! finite)  // should never happen; if it does, start clean instead of making a loud noise
        {
            engine->reset();
            fadePos = fadeLength;
            std::fill (mono.begin(), mono.begin() + n, 0.0);
        }

        // X-ray: the twin heard the same input; play only what the block adds (amp minus twin), or the twin.
        if (twin != nullptr)
        {
            twin->process (twinBuffer.data(), n);
            const auto mode = (Listen) listenMode.load();

            for (int i = 0; i < n; ++i)
            {
                const double tw = std::isfinite (twinBuffer[(size_t) i]) ? twinBuffer[(size_t) i] : 0.0;
                const double heard = mode == Listen::onlyAdds ? mono[(size_t) i] - tw : mode == Listen::without ? tw : mono[(size_t) i];
                const double g = (double) twinFade / fadeLength;  // a new twin fades in, so switching does not click

                if (twinFade < fadeLength)
                    ++twinFade;

                mono[(size_t) i] += g * (heard - mono[(size_t) i]);
            }
        }

        for (int i = 0; i < n; ++i)
        {
            const auto v = (float) (mono[(size_t) i] * outputGain.getNextValue());
            outPeak = juce::jmax (outPeak, std::abs (v));

            for (int ch = 0; ch < numOut; ++ch)
                buffer.setSample (ch, start + i, v);
        }
    }

    // The meters: peaks go up to the highest since the editor last read them.
    const auto raise = [] (std::atomic<float>& a, float v)
    {
        for (float old = a.load (std::memory_order_relaxed); v > old && ! a.compare_exchange_weak (old, v);) {}
    };
    raise (meters.inPeak, inPeak);
    raise (meters.outPeak, outPeak);

    if (rawPeak >= 0.999f)
        meters.inClip = true;

    if (outPeak >= 0.999f)
        meters.outClip = true;

    if (tunerOn && tuner.isMuted())
        meters.noteHz.store (0.0f, std::memory_order_relaxed);

    const double used = juce::Time::highResolutionTicksToSeconds (juce::Time::getHighResolutionTicks() - started) / (numSamples / currentRate);
    meters.cpu.store ((float) (0.9 * meters.cpu.load (std::memory_order_relaxed) + 0.1 * used), std::memory_order_relaxed);
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
    state.setProperty ("level", currentLevel.load(), nullptr);
    state.appendChild (parameters.copyState(), nullptr);

    if (auto xml = state.createXml())
        copyXmlToBinary (*xml, destData);
}

void CustomAmpProcessor::setStateInformation (const void* data, int sizeInBytes)
{
    stopXray();  // a project starts with X-ray off
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
    currentLevel = (double) state.getProperty ("level", 0.0);

    // A preset file that still exists is read again (it may have been edited since); otherwise the saved
    // copy is used. Edits made in the plugin window are only in the saved copy. The saved level is kept,
    // so the project sounds exactly as it did.
    if (! isEdited() && getPresetFile().existsAsFile())
        readPresetFile (getPresetFile(), currentLevel.load() <= 0.0);
    else
        requestBuild (currentLevel.load() <= 0.0);

    undoSteps.clear();  // a project was loaded: nothing before it to go back to
    redoSteps.clear();
    updatePanel();
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new CustomAmpProcessor();
}
