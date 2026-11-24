#pragma once

#include "MenuSlider.h"
#include "PluginProcessor.h"

// The front panel: up to 8 knobs that read 0 to 10, each a handle on one setting in the chain (see Panel.h).
// The knobs are attached to the knob_1..knob_8 parameters; the processor writes their moves into the preset.
class PanelView final : public juce::Component
{
public:
    explicit PanelView (CustomAmpProcessor& p);
    ~PanelView() override;

    void setPanel (const std::vector<PanelKnob>& knobs, bool guessed, const juce::String& presetName);
    int getNumKnobsShown() const { return shown; }

    std::function<void (int index)> onRemove, onShowInChain;
    std::function<void (int index, const juce::String& label)> onRename;

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    struct Knob
    {
        MenuSlider slider;
        juce::Label name, value;
        std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> attachment;
    };

    void showValue (int index);
    void showMenu (int index);

    class Look;
    std::unique_ptr<Look> look;  // before the knobs: it must outlive them
    std::vector<std::unique_ptr<Knob>> knobs;
    std::vector<PanelKnob> panel;
    juce::String presetName;
    juce::Label note;
    juce::Rectangle<int> plateArea;
    int shown = 0;
};
