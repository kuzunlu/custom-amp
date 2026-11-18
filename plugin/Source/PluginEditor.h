#pragma once

#include <juce_audio_utils/juce_audio_utils.h>

#include "PluginProcessor.h"

// Help text for every setting, from shared/settings.json (built into the plugin).
class SettingsHelp
{
public:
    SettingsHelp();

    juce::String blockTip (const juce::String& type) const;
    juce::String settingTip (const juce::String& type, const juce::String& key) const;
    juce::String sectionTip (const juce::String& section) const;                 // "split_path", "band", ...
    juce::String sectionSettingTip (const juce::String& section, const juce::String& key) const;

private:
    static juce::String entryTip (const juce::var& entry);
    juce::var doc;
};

// The blocks of the preset, one row each, with every setting that is set. Hover for help.
class ChainView final : public juce::Component
{
public:
    explicit ChainView (const SettingsHelp& h) : help (h) {}

    void setPreset (const juce::var& preset);
    void layout (int width);  // sets its own height to fit

private:
    struct Item
    {
        std::unique_ptr<juce::Label> label;
        int indent = 0;
        bool startsRow = false, isHeader = false;
    };

    void addBlocks (const juce::Array<juce::var>& blocks, int indent);
    void addItem (const juce::String& text, const juce::String& tip, int indent, bool startsRow, bool isHeader);
    static juce::String formatValue (const juce::String& key, const juce::var& value);

    const SettingsHelp& help;
    std::vector<Item> items;
};

class CustomAmpEditor final : public juce::AudioProcessorEditor,
                              private juce::ChangeListener
{
public:
    explicit CustomAmpEditor (CustomAmpProcessor&);
    ~CustomAmpEditor() override;

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    void changeListenerCallback (juce::ChangeBroadcaster*) override;
    void refresh();
    void refreshPresetBox();

    CustomAmpProcessor& processor;
    SettingsHelp help;
    juce::TooltipWindow tooltips { this, 400 };

    juce::ComboBox presetBox;
    juce::TextButton openButton { "Open file..." }, reloadButton { "Reload" };
    juce::TextButton irButton { "Load IR..." }, clearIrButton { "Built-in IR" };
    juce::Slider inputKnob, outputKnob;
    juce::Label inputLabel, outputLabel, qualityLabel, chainLabel;
    juce::ComboBox qualityBox;
    juce::TextEditor statusBox;
    juce::Viewport chainViewport;
    ChainView chainView { help };

    juce::AudioProcessorValueTreeState::SliderAttachment inputAttachment, outputAttachment;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ComboBoxAttachment> qualityAttachment;  // made after the items exist
    std::unique_ptr<juce::FileChooser> chooser;
    juce::Array<CustomAmpProcessor::BuiltInPreset> builtIns;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (CustomAmpEditor)
};
