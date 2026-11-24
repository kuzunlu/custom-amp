#include "PluginEditor.h"

#include "Theme.h"

namespace
{
const int fileItemId = 1000;
const int editedItemId = 1001;
const juce::uint32 mergeEditsMs = 800;  // small edits to the same block closer than this are one undo step

// Where a list sits in the preset, as property names and list places, so the same place can be found
// in a new copy of the preset (after undo, or a file reload).
bool findList (const juce::var& node, const juce::Array<juce::var>* target, juce::StringArray& path)
{
    if (node.getArray() == target)
        return true;

    if (auto* array = node.getArray())
    {
        for (int i = 0; i < array->size(); ++i)
        {
            path.add (juce::String (i));

            if (findList (array->getReference (i), target, path))
                return true;

            path.remove (path.size() - 1);
        }
    }
    else if (auto* object = node.getDynamicObject())
    {
        for (auto& property : object->getProperties())
        {
            path.add (property.name.toString());

            if (findList (property.value, target, path))
                return true;

            path.remove (path.size() - 1);
        }
    }

    return false;
}

juce::var followPath (juce::var node, const juce::StringArray& path)
{
    for (auto& step : path)
        node = node.isArray() ? node[step.getIntValue()] : node.getProperty (juce::Identifier (step), {});

    return node;
}
}

CustomAmpEditor::CustomAmpEditor (CustomAmpProcessor& p)
    : AudioProcessorEditor (&p),
      processor (p),
      inputAttachment (p.parameters, "input_db", inputKnob),
      outputAttachment (p.parameters, "output_db", outputKnob)
{
    builtIns = CustomAmpProcessor::getBuiltInPresets();

    addAndMakeVisible (presetBox);
    presetBox.setTooltip (help.sectionTip ("preset"));
    presetBox.onChange = [this]
    {
        const int id = presetBox.getSelectedId();

        if (id >= 1 && id <= builtIns.size())
            processor.loadBuiltInPreset (builtIns[id - 1].fileName);
    };

    addAndMakeVisible (openButton);
    openButton.setTooltip ("Open a preset .json file. The plugin watches the file: edit it in any text editor, "
                           "save, and the amp reloads by itself (until you change something in this window).");
    openButton.onClick = [this]
    {
        // The installer puts editable copies of the presets here.
        auto start = processor.getPresetFile();
        const auto installed = juce::File::getSpecialLocation (juce::File::commonDocumentsDirectory).getChildFile ("Custom Amp/Presets");

        if (start == juce::File() && installed.isDirectory())
            start = installed;

        chooser = std::make_unique<juce::FileChooser> ("Open a preset", start, "*.json");
        chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                              [this] (const juce::FileChooser& fc)
                              {
                                  if (fc.getResult().existsAsFile())
                                      processor.loadPresetFile (fc.getResult());
                              });
    };

    addAndMakeVisible (reloadButton);
    reloadButton.setTooltip ("Load the preset again from where it came from. Throws away changes made in this window.");
    reloadButton.onClick = [this] { processor.reloadPreset(); };

    addAndMakeVisible (saveButton);
    saveButton.setTooltip ("Save the preset, with your changes, as a .json file. The plugin then watches that file.");
    saveButton.onClick = [this]
    {
        auto start = processor.getPresetFile();

        if (start == juce::File())
            start = juce::File::getSpecialLocation (juce::File::userDocumentsDirectory).getChildFile ("my_amp.json");

        chooser = std::make_unique<juce::FileChooser> ("Save the preset", start, "*.json");
        chooser->launchAsync (juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::canSelectFiles
                                  | juce::FileBrowserComponent::warnAboutOverwriting,
                              [this] (const juce::FileChooser& fc)
                              {
                                  auto file = fc.getResult();

                                  if (file == juce::File())
                                      return;

                                  if (! processor.saveEditedPreset (file.withFileExtension ("json")))
                                      statusBox.setText ("Could not write " + file.getFullPathName());
                                  else
                                      sentText = processor.getPresetText();

                                  refreshPresetBox();
                              });
    };

    addAndMakeVisible (irButton);
    irButton.setTooltip (help.blockTip ("ir"));
    irButton.onClick = [this]
    {
        chooser = std::make_unique<juce::FileChooser> ("Load a cab IR", processor.getIrFile(), "*.wav;*.aif;*.aiff;*.flac");
        chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                              [this] (const juce::FileChooser& fc)
                              {
                                  if (fc.getResult().existsAsFile())
                                      processor.setIrFile (fc.getResult());
                              });
    };

    addAndMakeVisible (clearIrButton);
    clearIrButton.setTooltip ("Forget the loaded IR file: each ir block uses its own file again "
                              "(or the built-in test cab if that file is not found).");
    clearIrButton.onClick = [this] { processor.setIrFile ({}); };

    addAndMakeVisible (levelButton);
    levelButton.setTooltip ("Measure the output level again, so this amp comes out about as loud as the others "
                            "(a short test riff averages -20 dBFS). Done by itself when a preset is loaded, "
                            "but not while you edit, so you can hear what each change does to the volume.");
    levelButton.onClick = [this] { processor.measureLevel(); };

    for (auto [knob, label, key] : { std::tuple { &inputKnob, &inputLabel, "input_db" },
                                     std::tuple { &outputKnob, &outputLabel, "output_db" } })
    {
        knob->setSliderStyle (juce::Slider::RotaryHorizontalVerticalDrag);
        knob->setTextBoxStyle (juce::Slider::TextBoxBelow, false, 70, 20);
        knob->setTextValueSuffix (" dB");
        knob->setTooltip (help.sectionSettingTip ("plugin", key));
        addAndMakeVisible (*knob);
        label->setText (label == &inputLabel ? "Input" : "Output", juce::dontSendNotification);
        label->setJustificationType (juce::Justification::centred);
        addAndMakeVisible (*label);
    }

    qualityBox.addItemList ({ "2x", "4x", "8x" }, 1);
    qualityBox.setTooltip (help.sectionSettingTip ("plugin", "quality"));
    qualityLabel.setText ("Quality", juce::dontSendNotification);
    qualityLabel.setJustificationType (juce::Justification::centred);
    addAndMakeVisible (qualityBox);
    addAndMakeVisible (qualityLabel);
    qualityAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ComboBoxAttachment> (p.parameters, "quality", qualityBox);

    statusBox.setMultiLine (true);
    statusBox.setReadOnly (true);
    statusBox.setScrollbarsShown (true);
    statusBox.setColour (juce::TextEditor::backgroundColourId, theme::panel);
    addAndMakeVisible (statusBox);

    addAndMakeVisible (chainLabel);

    addAndMakeVisible (addBlockButton);
    addBlockButton.setTooltip ("Add a block at the end of the chain (before the cab IR: only eq and ir blocks can come after it).");
    addBlockButton.onClick = [this]
    {
        if (! preset.isObject())
            return;

        if (! preset["chain"].isArray())
            if (auto* obj = preset.getDynamicObject())
                obj->setProperty ("chain", juce::Array<juce::var>());

        juce::PopupMenu menu;
        const auto types = help.blockTypes();

        for (int i = 0; i < types.size(); ++i)
            menu.addItem (i + 1, types[i] + "  -  " + help.blockInfo (types[i])["short"].toString());

        menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&addBlockButton), [this, types] (int result)
        {
            if (result <= 0)
                return;

            auto chain = preset["chain"];
            const auto block = makeBlock (types[result - 1]);
            const int at = endInsertIndex (chain, block);
            chain.getArray()->insert (at, block);
            presetEdited (true);
            select ({ chain, at, false });
        });
    };

    addAndMakeVisible (undoButton);
    undoButton.setTooltip ("Take back the last change (Ctrl+Z): an edit, a loaded preset or a reload. "
                           "A loaded preset comes back with the level it had.");
    undoButton.onClick = [this] { processor.undo(); refresh(); };

    addAndMakeVisible (redoButton);
    redoButton.setTooltip ("Do the change you took back again (Ctrl+Y or Ctrl+Shift+Z).");
    redoButton.onClick = [this] { processor.redo(); refresh(); };

    chainView.onSelect = [this] (const Selection& s) { select (s); };
    chainViewport.setViewedComponent (&chainView, false);
    chainViewport.setScrollBarsShown (true, false);
    addAndMakeVisible (chainViewport);

    inspector.onChanged = [this] (bool rebuild) { presetEdited (rebuild); };
    inspector.onSelect = [this] (const Selection& s) { select (s); };
    inspector.onPutOnPanel = [this] (const juce::var& block, const juce::String& setting) { putOnPanel (block, setting); };
    inspectorViewport.setViewedComponent (&inspector, false);
    inspectorViewport.setScrollBarsShown (true, false);
    addAndMakeVisible (inspectorViewport);

    signalView.onSelect = [this] (const Selection& s) { select (s); };
    addChildComponent (signalView);
    addAndMakeVisible (signalsButton);
    signalsButton.onClick = [this] { showSignals (! processor.showSignals); };

    addChildComponent (panelView);
    panelView.onRemove = [this] (int index)
    {
        panel::removeKnob (preset, index, help);
        presetEdited (true);
    };
    panelView.onRename = [this] (int index, const juce::String& label)
    {
        panel::renameKnob (preset, index, label, help);
        presetEdited (true);
    };
    panelView.onShowInChain = [this] (int index)
    {
        // The knob's block: the last step of its path is its place in a list, the steps before lead to the list.
        const auto& path = processor.getPanel()[(size_t) index].blockPath;
        auto list = preset;

        for (int i = 0; i + 1 < path.size(); ++i)
            list = list.isArray() ? list[path[i].getIntValue()] : list.getProperty (juce::Identifier (path[i]), {});

        showView (false);
        select ({ list, path[path.size() - 1].getIntValue(), false });
    };

    addAndMakeVisible (viewButton);
    viewButton.onClick = [this] { showView (! processor.showPanelView); };

    processor.addChangeListener (this);
    setWantsKeyboardFocus (true);  // for Ctrl+Z (keys a text field does not use come here too)
    setResizable (true, true);
    setResizeLimits (760, 480, 2400, 1600);
    setSize (1120, 720);
    refresh();
    showView (processor.showPanelView);
}

void CustomAmpEditor::showView (bool showPanel)
{
    processor.showPanelView = showPanel;
    panelView.setVisible (showPanel);

    for (auto* c : { (juce::Component*) &inspectorViewport, (juce::Component*) &addBlockButton, (juce::Component*) &signalsButton })
        c->setVisible (! showPanel);

    chainViewport.setVisible (! showPanel && ! processor.showSignals);
    signalView.setVisible (! showPanel && processor.showSignals);
    signalsButton.setButtonText (processor.showSignals ? "Blocks" : "Signals");
    signalsButton.setTooltip (processor.showSignals ? "Back to the list of blocks and their settings."
                                                    : "See what every block does to a test signal: its waveform or its spectrum.");

    viewButton.setButtonText (showPanel ? juce::String::fromUTF8 ("Edit chain \xe2\x96\xb8") : juce::String::fromUTF8 ("\xe2\x97\x82 Panel"));
    viewButton.setTooltip (showPanel ? "Show the blocks of the amp, to change any setting, add blocks or put more knobs on the panel."
                                     : "Back to the front panel.");
    showMessage ({}, false);
    resized();
}

void CustomAmpEditor::showSignals (bool signals)
{
    processor.showSignals = signals;
    showView (false);
}

void CustomAmpEditor::showMessage (const juce::String& text, bool isProblem)
{
    // Empty text: the normal hint for the view.
    const auto hint = processor.showPanelView ? "Front panel: turn the knobs. Your DAW can automate them as Knob 1 to Knob 8."
                                              : "Chain: click a block to edit it, hover for help. Right-click a slider to put it on the front panel.";
    chainLabel.setText (text.isNotEmpty() ? text : juce::String (hint), juce::dontSendNotification);
    chainLabel.setColour (juce::Label::textColourId,
                          text.isEmpty() ? juce::Colours::white : isProblem ? theme::error : theme::accent);
}

void CustomAmpEditor::putOnPanel (const juce::var& block, const juce::String& setting)
{
    const auto problem = panel::addKnob (preset, block, setting, help);

    if (problem.isNotEmpty())
    {
        showMessage (problem, true);
        return;
    }

    presetEdited (true);
    showMessage ("On the front panel now, as knob " + juce::String ((int) processor.getPanel().size())
                     + ". Rename it there: double-click its name.", false);
}

void CustomAmpEditor::refreshPanel()
{
    panelView.setPanel (processor.getPanel(), processor.isPanelGuessed(), preset.getProperty ("name", "").toString());
}

CustomAmpEditor::~CustomAmpEditor()
{
    processor.removeChangeListener (this);
}

void CustomAmpEditor::changeListenerCallback (juce::ChangeBroadcaster*)
{
    // Our own edits come back as the same text: then only the status changes.
    if (processor.getPresetText() != sentText)
        refresh();
    else
        refreshStatus();

    refreshPresetBox();
}

void CustomAmpEditor::select (const Selection& s)
{
    selection = s;
    needChainView = needInspector = true;
    triggerAsyncUpdate();  // not now: the click that called this may come from a component that gets rebuilt
}

void CustomAmpEditor::presetEdited (bool rebuildInspector)
{
    // A slider drag sends many small changes: one undo step for all of them. Changes that rebuild the
    // inspector (add, remove, a new curve) are always a step of their own.
    const auto now = juce::Time::getMillisecondCounter();
    const bool merge = ! rebuildInspector && lastEditWasSmall && selection == lastEditSelection
                       && now - lastEditTime < mergeEditsMs;
    lastEditSelection = selection;
    lastEditTime = now;
    lastEditWasSmall = ! rebuildInspector;

    sentText = juce::JSON::toString (preset, true);
    processor.setEditedPreset (sentText, merge);
    signalView.setPreset (preset, sentText);
    refreshPanel();
    needChainView = true;
    needInspector = needInspector || rebuildInspector;
    triggerAsyncUpdate();
    refreshPresetBox();
}

void CustomAmpEditor::handleAsyncUpdate()
{
    if (needChainView)
        chainView.setPreset (preset, selection);

    if (needInspector)
        inspector.show (selection, preset);

    signalView.setSelection (selection);
    needChainView = needInspector = false;
    layoutViews();
}

void CustomAmpEditor::refreshPresetBox()
{
    presetBox.clear (juce::dontSendNotification);
    const auto source = processor.getPresetSource();
    const auto file = processor.getPresetFile();

    if (processor.isEdited())
        presetBox.addItem ("Edited: " + preset.getProperty ("name", "preset").toString() + " (not saved)", editedItemId);
    else if (file != juce::File())
        presetBox.addItem ("File: " + file.getFileName(), fileItemId);

    for (int i = 0; i < builtIns.size(); ++i)
        presetBox.addItem (builtIns[i].name, i + 1);

    if (processor.isEdited())
        presetBox.setSelectedId (editedItemId, juce::dontSendNotification);
    else if (file != juce::File())
        presetBox.setSelectedId (fileItemId, juce::dontSendNotification);

    for (int i = 0; i < builtIns.size(); ++i)
        if (source == "builtin:" + builtIns[i].fileName)
            presetBox.setSelectedId (i + 1, juce::dontSendNotification);

    undoButton.setEnabled (processor.canUndo());
    redoButton.setEnabled (processor.canRedo());
}

void CustomAmpEditor::refreshStatus()
{
    auto status = processor.getStatus();
    juce::String text = status.text;
    const auto ir = processor.getIrFile();

    if (ir != juce::File())
        text << "\nIR file (replaces the preset's): " << ir.getFullPathName();

    text << "\n\nCustom Amp " << CUSTOM_AMP_VERSION;  // the git tag of a release, "dev" otherwise

    const auto colour = status.isError ? theme::error : juce::Colours::white;
    statusBox.setColour (juce::TextEditor::textColourId, colour);
    statusBox.setText (text, false);
    statusBox.applyColourToAllText (colour);
}

void CustomAmpEditor::refresh()
{
    // A new preset from outside (menu, file, reload, undo, DAW). The same block stays selected if the new
    // preset has a block of the same type in the same place.
    juce::StringArray path;
    const bool found = selection.isValid() && findList (preset, selection.list.getArray(), path);
    const auto oldType = selection.item()["type"];

    preset = processor.getPresetData();
    sentText = processor.getPresetText();
    const Selection kept { followPath (preset, path), selection.index, selection.isPath };
    selection = found && kept.isValid() && kept.item()["type"] == oldType ? kept : Selection {};
    lastEditTime = 0;
    lastEditWasSmall = false;
    refreshPresetBox();
    refreshStatus();
    chainView.setPreset (preset, selection);
    inspector.show (selection, preset);
    signalView.setPreset (preset, sentText);
    signalView.setSelection (selection);
    refreshPanel();
    layoutViews();
}

void CustomAmpEditor::paint (juce::Graphics& g)
{
    g.fillAll (theme::background);
}

bool CustomAmpEditor::keyPressed (const juce::KeyPress& key)
{
    const auto command = juce::ModifierKeys::commandModifier, shift = juce::ModifierKeys::shiftModifier;

    // A text field that has the focus gets these keys first (Ctrl+C there copies its text).
    if (key == juce::KeyPress ('z', command, 0))
        undoButton.triggerClick();
    else if (key == juce::KeyPress ('y', command, 0) || key == juce::KeyPress ('z', command | shift, 0))
        redoButton.triggerClick();
    else if (processor.showPanelView)
        return false;  // the block keys only work where the blocks are shown
    else if (key == juce::KeyPress ('c', command, 0))
        return inspector.copySelected();
    else if (key == juce::KeyPress ('x', command, 0))
        return inspector.cutSelected();
    else if (key == juce::KeyPress ('v', command, 0))
        return inspector.pasteAfterSelected();
    else if (key == juce::KeyPress ('d', command, 0))
        return inspector.duplicateSelected();
    else if (key == juce::KeyPress (juce::KeyPress::deleteKey) && inspector.getSelection().isValid())
        inspector.removeSelected();
    else
        return false;

    return true;
}

void CustomAmpEditor::layoutViews()
{
    chainView.layout (juce::jmax (100, chainViewport.getWidth() - chainViewport.getScrollBarThickness() - 4));
    inspector.layout (juce::jmax (100, inspectorViewport.getWidth() - inspectorViewport.getScrollBarThickness() - 8));
}

void CustomAmpEditor::resized()
{
    auto area = getLocalBounds().reduced (10);

    auto top = area.removeFromTop (28);
    presetBox.setBounds (top.removeFromLeft (juce::jmin (330, top.getWidth() / 3)));
    top.removeFromLeft (8);
    const int buttonWidth = juce::jmin (96, (top.getWidth() - 30) / 6);

    for (auto* b : { &openButton, &reloadButton, &saveButton, &irButton, &clearIrButton, &levelButton })
    {
        b->setBounds (top.removeFromLeft (buttonWidth));
        top.removeFromLeft (5);
    }

    area.removeFromTop (8);
    auto controls = area.removeFromTop (110);
    auto knobArea = controls.removeFromLeft (280);

    for (auto [knob, label] : { std::pair { &inputKnob, &inputLabel }, std::pair { &outputKnob, &outputLabel } })
    {
        auto column = knobArea.removeFromLeft (90);
        label->setBounds (column.removeFromTop (18));
        knob->setBounds (column);
    }

    auto qualityColumn = knobArea.removeFromLeft (90);
    qualityLabel.setBounds (qualityColumn.removeFromTop (18));
    qualityBox.setBounds (qualityColumn.removeFromTop (26).reduced (4, 0));

    controls.removeFromLeft (8);
    statusBox.setBounds (controls);

    area.removeFromTop (8);
    auto header = area.removeFromTop (26);
    viewButton.setBounds (header.removeFromRight (110));
    header.removeFromRight (5);

    if (! processor.showPanelView)
    {
        addBlockButton.setBounds (header.removeFromRight (110));
        header.removeFromRight (5);
        signalsButton.setBounds (header.removeFromRight (80));
        header.removeFromRight (5);
    }

    redoButton.setBounds (header.removeFromRight (60));
    header.removeFromRight (5);
    undoButton.setBounds (header.removeFromRight (60));
    chainLabel.setBounds (header);
    area.removeFromTop (4);

    panelView.setBounds (area);
    auto left = area.removeFromLeft (area.getWidth() * 55 / 100);
    chainViewport.setBounds (left);
    signalView.setBounds (left);
    area.removeFromLeft (12);
    inspectorViewport.setBounds (area);
    layoutViews();
}
