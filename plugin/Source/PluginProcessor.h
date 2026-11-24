#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

#include "Help.h"
#include "Panel.h"
#include "dsp/Engine.h"

#include <array>
#include <optional>

class CustomAmpProcessor final : public juce::AudioProcessor,
                                 public juce::ChangeBroadcaster,
                                 private juce::Timer
{
public:
    CustomAmpProcessor();
    ~CustomAmpProcessor() override;

    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override {}
    bool isBusesLayoutSupported (const BusesLayout& layouts) const override;

    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    using AudioProcessor::processBlock;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override                                 { return true; }

    const juce::String getName() const override                     { return JucePlugin_Name; }
    bool acceptsMidi() const override                               { return false; }
    bool producesMidi() const override                              { return false; }
    double getTailLengthSeconds() const override                    { return 0.0; }

    int getNumPrograms() override                                   { return 1; }
    int getCurrentProgram() override                                { return 0; }
    void setCurrentProgram (int) override                           {}
    const juce::String getProgramName (int) override                { return {}; }
    void changeProgramName (int, const juce::String&) override      {}

    void getStateInformation (juce::MemoryBlock& destData) override;
    void setStateInformation (const void* data, int sizeInBytes) override;

    // Message thread only. ---------------------------------------------------
    struct BuiltInPreset { juce::String fileName, name; };
    static juce::Array<BuiltInPreset> getBuiltInPresets();

    void loadBuiltInPreset (const juce::String& fileName);
    void loadPresetFile (const juce::File& file);
    void reloadPreset();
    void setIrFile (const juce::File& file);  // juce::File() = use the preset's own IR

    // An edit made in the plugin window. Keeps the output level, so you hear what the edit does.
    // mergeWithLast = part of the last undo step (a slider being dragged), not a new one.
    void setEditedPreset (const juce::String& text, bool mergeWithLast = false);
    bool saveEditedPreset (const juce::File& file);
    void measureLevel();

    // Every preset change (load, reload, file change, edit) can be undone, with the level it had.
    void undo();
    void redo();
    bool canUndo() const                   { return ! undoSteps.empty(); }
    bool canRedo() const                   { return ! redoSteps.empty(); }

    // The front panel of the current preset. Its knobs are the parameters knob_1 to knob_8: moving one (in the
    // window or from the DAW) changes the preset, like an edit in the window.
    const std::vector<PanelKnob>& getPanel() const { return panelKnobs; }
    bool isPanelGuessed() const            { return panelGuessed; }
    bool showPanelView = true;             // what the window shows (kept while the window is closed)
    bool showSignals = false;              // the chain editor shows the signal view instead of the block list

    // For the signal view: how a preset is built here (folder, IR, rates), with the output level fixed at 1.
    amp::Engine::Options getSignalOptions() const;
    // The last `seconds` of input (after the Input knob), for the signal view.
    std::vector<double> getRecentInput (double seconds) const;

    juce::String getPresetSource() const   { return presetSource; }
    juce::String getPresetText() const     { return presetText; }
    bool isEdited() const                  { return presetSource.startsWith ("edited:"); }
    juce::File getPresetFile() const;      // the file the preset came from (also when edited since)
    juce::File getIrFile() const           { return irFile; }
    juce::var getPresetData() const;       // the preset as parsed JSON (empty if it does not parse)

    struct Status
    {
        bool isError = false;
        juce::String text;
    };
    Status getStatus() const;

    juce::AudioProcessorValueTreeState parameters;

private:
    struct BuildRequest
    {
        juce::String presetText;
        juce::File presetFolder, irFile;
        double sampleRate = 48000.0;
        int maxBlock = 512, factorLog2 = 3, generation = 0;
        double fixedLevel = 0.0;  // 0 = measure the level
        bool warmUp = false;      // play the last bit of input through it first
    };

    // Builds engines on its own thread, so the sound keeps going while a new preset is loaded.
    class Builder final : public juce::Thread
    {
    public:
        explicit Builder (CustomAmpProcessor& p) : juce::Thread ("Custom Amp builder"), owner (p) {}
        void request (BuildRequest r);
        void run() override;

    private:
        CustomAmpProcessor& owner;
        juce::CriticalSection lock;
        std::optional<BuildRequest> next;
        juce::WaitableEvent wakeUp;
    };

    static juce::AudioProcessorValueTreeState::ParameterLayout createParameterLayout();
    std::unique_ptr<amp::Engine> buildEngine (const BuildRequest& r, juce::String& message) const;

    BuildRequest makeRequest (bool measure);
    void requestBuild (bool measure);
    void engineBuilt (std::unique_ptr<amp::Engine> built, const BuildRequest& r, const juce::String& message);
    struct Step
    {
        juce::String source, text;
        double level = 0.0;
    };

    void setPresetText (const juce::String& source, const juce::String& text, bool measure, bool newStep = true);
    void restore (const Step& step);
    void updatePanel();
    void applyKnobChanges();
    void readPresetFile (const juce::File& file, bool measure);
    int factorLog2ForRate (double rate) const;
    void timerCallback() override;

    std::atomic<float>* inputParam = nullptr;
    std::atomic<float>* outputParam = nullptr;
    std::atomic<float>* qualityParam = nullptr;

    // The engine belongs to the audio thread. A new one arrives in `pendingEngine`; the old one fades out
    // in `fadingEngine`, then goes back through `retiredEngine` and is deleted on the message thread
    // (never free memory on the audio thread).
    std::unique_ptr<amp::Engine> engine, fadingEngine;
    std::atomic<amp::Engine*> pendingEngine { nullptr }, retiredEngine { nullptr };
    int fadePos = 0, fadeLength = 1;
    juce::CriticalSection postLock;
    int latestGeneration = 0;

    // The last moments of input, so a new engine can be warmed up with it before it takes over.
    std::vector<std::atomic<float>> history = std::vector<std::atomic<float>> (1 << 15);
    std::atomic<int> historyWrite { 0 };

    std::vector<double> mono, monoOld;
    juce::SmoothedValue<double, juce::ValueSmoothingTypes::Multiplicative> inputGain { 1.0 }, outputGain { 1.0 };
    double currentRate = 0.0;
    int currentBlock = 512;

    juce::String presetSource, presetText;  // source: "builtin:<file>", "file:<path>", or "edited:" + one of those
    juce::File irFile;
    juce::Time presetFileTime;
    int builtQuality = -1;
    std::atomic<double> currentLevel { 0.0 };
    std::vector<Step> undoSteps, redoSteps;

    SettingsHelp help;  // for the panel: setting names, ranges and units from settings.json
    std::vector<PanelKnob> panelKnobs;
    std::vector<double> knobPositions;  // where the preset puts each knob (0-10)
    std::array<std::atomic<float>*, panel::maxKnobs> knobParams {};
    bool panelGuessed = false, applyingKnob = false;
    int lastKnobEdited = -1;  // so a moving knob is one undo step
    juce::uint32 lastKnobTime = 0;

    mutable juce::CriticalSection statusLock;
    Status status;
    std::atomic<bool> statusChanged { false };
    std::atomic<int> newLatency { -1 };

    Builder builder { *this };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (CustomAmpProcessor)
};
