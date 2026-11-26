#pragma once

#include <juce_audio_utils/juce_audio_utils.h>

#include "ChainView.h"
#include "Help.h"
#include "Inspector.h"
#include "MeterView.h"
#include "PanelView.h"
#include "PluginProcessor.h"
#include "SignalView.h"

class CustomAmpEditor final : public juce::AudioProcessorEditor,
                              private juce::ChangeListener,
                              private juce::AsyncUpdater
{
public:
    explicit CustomAmpEditor (CustomAmpProcessor&);
    ~CustomAmpEditor() override;

    void paint (juce::Graphics&) override;
    void resized() override;
    bool keyPressed (const juce::KeyPress& key) override;

    // Selects a block (or split path) for the inspector. Also used by the test program.
    void select (const Selection& selection);
    const Selection& getSelection() const { return selection; }
    juce::var getEditedPreset() const { return preset; }

    // The front panel (shown first) or the chain editor.
    void showView (bool panel);
    bool isShowingPanel() const { return processor.showPanelView; }
    const PanelView& getPanelView() const { return panelView; }

    // In the chain editor: the signal view (each block's waveform or spectrum) instead of the block list.
    void showSignals (bool signals);
    SignalView& getSignalView() { return signalView; }
    const MeterView& getMeterView() const { return meterView; }

    // What "Put on the front panel" does (right-click a slider in the chain editor).
    void putOnPanel (const juce::var& block, const juce::String& setting);

private:
    void changeListenerCallback (juce::ChangeBroadcaster*) override;
    void handleAsyncUpdate() override;
    void refresh();
    void refreshPresetBox();
    void refreshStatus();
    void layoutViews();
    void presetEdited (bool rebuildInspector);
    void refreshPanel();
    void showMessage (const juce::String& text, bool isProblem);
    void showXrayMenu (const juce::StringArray& blockPath, juce::Component* near);
    void refreshXray();

    CustomAmpProcessor& processor;
    SettingsHelp help;
    juce::TooltipWindow tooltips { this, 400 };

    // The preset being shown and edited. Edits change it in place, then it is sent to the processor as text.
    juce::var preset;
    juce::String sentText;
    Selection selection;
    bool needChainView = false, needInspector = false;

    // The last edit, to make quick small changes to the same block (a slider drag) one undo step.
    Selection lastEditSelection;
    juce::uint32 lastEditTime = 0;
    bool lastEditWasSmall = false;

    juce::ComboBox presetBox;
    juce::TextButton openButton { "Open..." }, reloadButton { "Reload" }, saveButton { "Save as..." };
    juce::TextButton irButton { "Load IR..." }, clearIrButton { "Built-in IR" }, levelButton { "Level" };
    juce::Slider inputKnob, outputKnob;
    juce::Label inputLabel, outputLabel, qualityLabel, chainLabel;
    juce::ComboBox qualityBox;
    juce::TextEditor statusBox;
    MeterView meterView { processor };
    juce::TextButton addBlockButton { "Add block..." }, undoButton { "Undo" }, redoButton { "Redo" }, viewButton;
    PanelView panelView { processor };
    juce::Viewport chainViewport, inspectorViewport;
    ChainView chainView { help };
    Inspector inspector { help };
    SignalView signalView { processor };
    juce::TextButton signalsButton, xrayButton;

    juce::AudioProcessorValueTreeState::SliderAttachment inputAttachment, outputAttachment;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ComboBoxAttachment> qualityAttachment;  // made after the items exist
    std::unique_ptr<juce::FileChooser> chooser;
    juce::Array<CustomAmpProcessor::BuiltInPreset> builtIns;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (CustomAmpEditor)
};
