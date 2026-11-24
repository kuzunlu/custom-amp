#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include "ChainView.h"
#include "PluginProcessor.h"

#include <mutex>

// Shows what every block does to a test signal: its own copy of the amp plays the signal (on a background
// thread, never through the sound you hear) and records the output of each block. One strip per block in
// signal order, as a waveform (time) or as a spectrum (frequency). For sag blocks the strip is the headroom,
// for a gate with apply_after its gain.
class SignalView final : public juce::Component, private juce::AsyncUpdater
{
public:
    explicit SignalView (CustomAmpProcessor& p);
    ~SignalView() override;

    // The preset to show (the editor's copy, and the same as text). Plays the test signal through it again.
    void setPreset (const juce::var& preset, const juce::String& text);
    void setSelection (const Selection& s);
    std::function<void (const Selection&)> onSelect;

    enum class Signal { sine, pluck, chord, loudSoft, playing };
    void setSignal (Signal s, double freq, double peakV);  // also used by the test program
    void setFrequencyView (bool frequency);
    void setTimeSpanMs (int ms);

    // One recorded signal: the input, or the output of one block.
    struct Trace
    {
        juce::String name, summary;
        Selection target;  // nothing for the input
        int indent = 0;
        bool isUnit = false;  // headroom or gain (0 to 1), not volts
        double rate = 48000.0;
        std::vector<float> samples;
        float peak = 0.0f;
        std::vector<float> spectrumDb;  // dB of a sine's peak volts, per FFT bin
        double binHz = 1.0;
    };

    const std::vector<Trace>& getTraces() const { return traces; }
    juce::String getProblem() const { return problem; }
    bool isWaiting() const;  // a run is still going (for the test program)

    void resized() override;
    void paint (juce::Graphics&) override;
    void visibilityChanged() override;

private:
    struct Request
    {
        juce::String text;  // parsed again on the runner thread: the editor's juce::var is not safe to share
        amp::Engine::Options options;
        Signal signal = Signal::sine;
        double freq = 110.0, peakV = 0.5, spanSeconds = 0.02;
        std::vector<double> playing;
        int generation = 0;
    };

    struct Result
    {
        int generation = 0;
        juce::String text, problem;
        std::vector<Trace> traces;
    };

    class Runner final : public juce::Thread
    {
    public:
        explicit Runner (SignalView& v) : juce::Thread ("Custom Amp signal view"), owner (v) {}
        void request (Request r);
        void run() override;

    private:
        SignalView& owner;
        std::mutex lock;
        std::optional<Request> next;
        juce::WaitableEvent wakeUp;
    };

    class Strip;
    class Strips;

    static Result compute (const Request& r);
    void requestRun();
    void handleAsyncUpdate() override;
    void rebuildStrips();
    void signalChanged();

    CustomAmpProcessor& processor;
    juce::var preset;
    juce::String presetText;
    Selection selection;

    juce::ComboBox signalBox, spanBox;
    juce::TextButton timeButton { "Time" }, frequencyButton { "Frequency" };
    juce::Slider freqSlider, levelSlider;
    juce::Label freqLabel, levelLabel, infoLabel;
    juce::Viewport viewport;
    std::unique_ptr<Strips> strips;

    bool frequencyView = false, dirty = false;
    int generation = 0, shownGeneration = 0;
    std::vector<Trace> traces;
    juce::String problem;

    std::mutex resultLock;
    std::optional<Result> result;
    Runner runner { *this };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SignalView)
};
