#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

#include "dsp/Engine.h"

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

    juce::String getPresetSource() const   { return presetSource; }
    juce::File getPresetFile() const;
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
    static std::unique_ptr<amp::Engine> buildEngine (const BuildRequest& r, juce::String& message);

    BuildRequest makeRequest();
    void requestBuild();
    void engineBuilt (std::unique_ptr<amp::Engine> built, const BuildRequest& r, const juce::String& message);
    void setPresetText (const juce::String& source, const juce::String& text);
    int factorLog2ForRate (double rate) const;
    void timerCallback() override;

    std::atomic<float>* inputParam = nullptr;
    std::atomic<float>* outputParam = nullptr;
    std::atomic<float>* qualityParam = nullptr;

    // The engine belongs to the audio thread. A new one arrives in `pendingEngine`; the old one is
    // handed back in `retiredEngine` and deleted on the message thread (never free memory on the audio thread).
    std::unique_ptr<amp::Engine> engine;
    std::atomic<amp::Engine*> pendingEngine { nullptr }, retiredEngine { nullptr };
    juce::CriticalSection postLock;
    int latestGeneration = 0;

    std::vector<double> mono;
    juce::SmoothedValue<double, juce::ValueSmoothingTypes::Multiplicative> inputGain { 1.0 }, outputGain { 1.0 };
    double currentRate = 0.0;
    int currentBlock = 512;

    juce::String presetSource, presetText;  // source: "builtin:<file name>" or "file:<full path>"
    juce::File irFile;
    juce::Time presetFileTime;
    int builtQuality = -1;

    mutable juce::CriticalSection statusLock;
    Status status;
    std::atomic<bool> statusChanged { false };
    std::atomic<int> newLatency { -1 };

    Builder builder { *this };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (CustomAmpProcessor)
};
