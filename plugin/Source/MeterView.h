#pragma once

#include "PluginProcessor.h"

// The header's meters: input and output level with a clip light each, a tuner (the note at the amp's input and how
// many cents off it is), and how much of each audio block's time the plugin needs (CPU). Reads the processor's
// Meters 30 times a second; the tracker for the tuner only runs while this is on screen.
class MeterView final : public juce::Component,
                        public juce::SettableTooltipClient,
                        private juce::Timer
{
public:
    explicit MeterView (CustomAmpProcessor& p);
    ~MeterView() override;

    void paint (juce::Graphics&) override;
    void mouseDown (const juce::MouseEvent&) override;  // a click clears the clip lights

    // The note nearest to hz ("A2") and how far off it is (cents, -50..50). Also used by the test program.
    static juce::String noteName (double hz, double& cents);

    // What it shows now (for the test program).
    float shownIn() const { return inDb; }
    float shownOut() const { return outDb; }
    double shownHz() const { return hz; }

private:
    void timerCallback() override;
    void visibilityChanged() override;

    CustomAmpProcessor& processor;
    float inDb = -100, outDb = -100, cpu = 0;
    double hz = 0;
    juce::uint32 inClipUntil = 0, outClipUntil = 0;  // clip lights stay on 2 s
};
