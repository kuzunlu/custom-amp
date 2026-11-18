#include "PluginEditor.h"

#include "BinaryData.h"

namespace
{
const juce::Colour background { 0xff1e2124 };
const juce::Colour panel { 0xff2a2e33 };
const juce::Colour headerColour { 0xffe0a040 };
const juce::Colour chipColour { 0xff3a4047 };
const int fileItemId = 1000;
}

//==============================================================================
SettingsHelp::SettingsHelp()
{
    doc = juce::JSON::parse (juce::String::fromUTF8 (BinaryData::settings_json, BinaryData::settings_jsonSize));
}

juce::String SettingsHelp::entryTip (const juce::var& entry)
{
    if (! entry.isObject())
        return {};

    juce::String tip = entry.getProperty ("label", "").toString();
    const auto shortText = entry.getProperty ("short", "").toString();

    if (shortText.isNotEmpty())
        tip << ": " << shortText;

    tip << "\n\n" << entry.getProperty ("tooltip", "").toString();

    const auto unit = entry.getProperty ("unit", "").toString();
    juce::StringArray facts;

    if (entry.hasProperty ("min") && entry.hasProperty ("max"))
        facts.add ("Range " + entry["min"].toString() + " to " + entry["max"].toString() + (unit.isNotEmpty() ? " " + unit : ""));

    if (entry.hasProperty ("default") && ! entry["default"].isVoid() && ! entry["default"].isArray())
        facts.add ("default " + entry["default"].toString());

    if (! facts.isEmpty())
        tip << "\n\n" << facts.joinIntoString (", ") << ".";

    return tip.trim();
}

juce::String SettingsHelp::blockTip (const juce::String& type) const
{
    return entryTip (doc["blocks"][type.toRawUTF8()]);
}

juce::String SettingsHelp::settingTip (const juce::String& type, const juce::String& key) const
{
    auto entry = doc["blocks"][type.toRawUTF8()]["settings"][key.toRawUTF8()];

    if (! entry.isObject())
        entry = doc["common"]["settings"][key.toRawUTF8()];

    return entryTip (entry);
}

juce::String SettingsHelp::sectionTip (const juce::String& section) const
{
    return entryTip (doc[section.toRawUTF8()]);
}

juce::String SettingsHelp::sectionSettingTip (const juce::String& section, const juce::String& key) const
{
    return entryTip (doc[section.toRawUTF8()]["settings"][key.toRawUTF8()]);
}

//==============================================================================
static juce::String formatNumber (double v)
{
    if (v == std::floor (v) && std::abs (v) < 1e15)
        return juce::String ((juce::int64) v);

    return juce::String (v, 6).trimCharactersAtEnd ("0");
}

juce::String ChainView::formatValue (const juce::String& key, const juce::var& value)
{
    if (value.isArray())
    {
        juce::StringArray parts;

        for (auto& v : *value.getArray())
        {
            if (v.isObject() && v.hasProperty ("kind"))  // a filter band
            {
                juce::String band = v["kind"].toString() + " " + formatNumber ((double) v["freq"]) + " Hz";

                if (v.hasProperty ("gain_db"))
                    band << " " << ((double) v["gain_db"] >= 0 ? "+" : "") << formatNumber ((double) v["gain_db"]) << " dB";

                if (v.hasProperty ("q"))
                    band << " q " << formatNumber ((double) v["q"]);

                parts.add (band);
            }
            else
            {
                parts.add (v.toString());
            }
        }

        return key + ": " + (parts.isEmpty() ? juce::String ("none") : parts.joinIntoString (", "));
    }

    if (auto* obj = value.getDynamicObject())  // tonestack parts
    {
        juce::StringArray parts;

        for (auto& p : obj->getProperties())
            parts.add (p.name.toString() + " " + formatNumber ((double) p.value));

        return key + ": " + parts.joinIntoString (", ");
    }

    if (value.isBool())
        return key + ": " + juce::String ((bool) value ? "true" : "false");

    if (value.isDouble() || value.isInt() || value.isInt64())
        return key + ": " + formatNumber ((double) value);

    return key + ": " + value.toString();
}

void ChainView::addItem (const juce::String& text, const juce::String& tip, int indent, bool startsRow, bool isHeader)
{
    Item item;
    item.label = std::make_unique<juce::Label> (juce::String(), text);
    item.label->setTooltip (tip);
    item.label->setFont (juce::FontOptions (isHeader ? 15.0f : 13.0f, isHeader ? juce::Font::bold : juce::Font::plain));
    item.label->setColour (juce::Label::textColourId, isHeader ? headerColour : juce::Colours::white);
    item.label->setColour (juce::Label::backgroundColourId, isHeader ? juce::Colours::transparentBlack : chipColour);
    item.label->setBorderSize ({ 1, 6, 1, 6 });
    item.indent = indent;
    item.startsRow = startsRow;
    item.isHeader = isHeader;
    addAndMakeVisible (*item.label);
    items.push_back (std::move (item));
}

void ChainView::addBlocks (const juce::Array<juce::var>& blocks, int indent)
{
    int number = 0;

    for (auto& block : blocks)
    {
        ++number;
        const auto type = block.getProperty ("type", "?").toString();
        const auto id = block.getProperty ("id", "").toString();
        addItem (juce::String (number) + ". " + type + (id.isNotEmpty() ? "  " + id : juce::String()),
                 help.blockTip (type), indent, true, true);

        auto* obj = block.getDynamicObject();

        if (obj == nullptr)
            continue;

        for (auto& p : obj->getProperties())
        {
            const auto key = p.name.toString();

            if (key == "type" || key == "id" || key == "chain" || key == "paths")
                continue;

            addItem (formatValue (key, p.value), help.settingTip (type, key), indent, false, false);
        }

        if (type == "feedback")
            addBlocks (block["chain"].getArray() != nullptr ? *block["chain"].getArray() : juce::Array<juce::var>(), indent + 1);

        if (type == "split" && block["paths"].isArray())
        {
            int pathNumber = 0;

            for (auto& path : *block["paths"].getArray())
            {
                addItem ("path " + juce::String (++pathNumber), help.sectionTip ("split_path"), indent + 1, true, true);

                for (auto* key : { "level_db", "invert" })
                    if (path.hasProperty (key))
                        addItem (formatValue (key, path[key]), help.sectionSettingTip ("split_path", key), indent + 1, false, false);

                addBlocks (path["chain"].getArray() != nullptr ? *path["chain"].getArray() : juce::Array<juce::var>(), indent + 2);
            }
        }
    }
}

void ChainView::setPreset (const juce::var& preset)
{
    items.clear();

    if (! preset.isObject())
    {
        addItem ("The preset is not valid JSON.", {}, 0, true, true);
        return;
    }

    if (auto notes = preset["notes"].toString(); notes.isNotEmpty())
        addItem ("Notes (hover to read)", notes, 0, true, true);

    if (preset["chain"].isArray())
        addBlocks (*preset["chain"].getArray(), 0);
}

void ChainView::layout (int width)
{
    const int rowHeight = 22, gap = 4, indentWidth = 24;
    int x = 0, y = 0;

    for (auto& item : items)
    {
        auto& label = *item.label;
        const int left = item.indent * indentWidth;
        const int textWidth = juce::GlyphArrangement::getStringWidthInt (label.getFont(), label.getText()) + 14;
        const int w = juce::jmin (textWidth, width - left);

        if (item.startsRow)
        {
            if (&item != &items.front())
                y += rowHeight + gap + (item.isHeader ? 6 : 0);

            x = left;
        }
        else if (x + w > width)  // wrap the settings onto the next line, a little further in
        {
            y += rowHeight + gap;
            x = left + 16;
        }

        label.setBounds (x, y, w, rowHeight);
        x += w + gap;
    }

    setSize (width, y + rowHeight + gap);
}

//==============================================================================
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
                           "save, and the amp reloads by itself.");
    openButton.onClick = [this]
    {
        chooser = std::make_unique<juce::FileChooser> ("Open a preset", processor.getPresetFile(), "*.json");
        chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                              [this] (const juce::FileChooser& fc)
                              {
                                  if (fc.getResult().existsAsFile())
                                      processor.loadPresetFile (fc.getResult());
                              });
    };

    addAndMakeVisible (reloadButton);
    reloadButton.setTooltip ("Load the preset again (and its IR file).");
    reloadButton.onClick = [this] { processor.reloadPreset(); };

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
    statusBox.setColour (juce::TextEditor::backgroundColourId, panel);
    addAndMakeVisible (statusBox);

    chainLabel.setText ("Chain (hover over anything for help)", juce::dontSendNotification);
    addAndMakeVisible (chainLabel);
    chainViewport.setViewedComponent (&chainView, false);
    chainViewport.setScrollBarsShown (true, false);
    addAndMakeVisible (chainViewport);

    processor.addChangeListener (this);
    setResizable (true, true);
    setResizeLimits (640, 420, 2000, 1600);
    setSize (860, 640);
    refresh();
}

CustomAmpEditor::~CustomAmpEditor()
{
    processor.removeChangeListener (this);
}

void CustomAmpEditor::changeListenerCallback (juce::ChangeBroadcaster*)
{
    refresh();
}

void CustomAmpEditor::refreshPresetBox()
{
    presetBox.clear (juce::dontSendNotification);
    const auto source = processor.getPresetSource();
    const auto file = processor.getPresetFile();

    if (file != juce::File())
        presetBox.addItem ("File: " + file.getFileName(), fileItemId);

    for (int i = 0; i < builtIns.size(); ++i)
        presetBox.addItem (builtIns[i].name, i + 1);

    if (file != juce::File())
        presetBox.setSelectedId (fileItemId, juce::dontSendNotification);

    for (int i = 0; i < builtIns.size(); ++i)
        if (source == "builtin:" + builtIns[i].fileName)
            presetBox.setSelectedId (i + 1, juce::dontSendNotification);
}

void CustomAmpEditor::refresh()
{
    refreshPresetBox();

    auto status = processor.getStatus();
    juce::String text = status.text;
    const auto ir = processor.getIrFile();

    if (ir != juce::File())
        text << "\nIR file (replaces the preset's): " << ir.getFullPathName();

    statusBox.setColour (juce::TextEditor::textColourId, status.isError ? juce::Colour (0xffff7070) : juce::Colours::white);
    statusBox.setText (text, false);
    statusBox.applyColourToAllText (status.isError ? juce::Colour (0xffff7070) : juce::Colours::white);

    chainView.setPreset (processor.getPresetData());
    chainView.layout (juce::jmax (100, chainViewport.getWidth() - chainViewport.getScrollBarThickness() - 4));
}

void CustomAmpEditor::paint (juce::Graphics& g)
{
    g.fillAll (background);
}

void CustomAmpEditor::resized()
{
    auto area = getLocalBounds().reduced (10);

    auto top = area.removeFromTop (28);
    presetBox.setBounds (top.removeFromLeft (juce::jmin (320, top.getWidth() / 2)));
    top.removeFromLeft (8);

    for (auto* b : { &openButton, &reloadButton, &irButton, &clearIrButton })
    {
        b->setBounds (top.removeFromLeft (juce::jmin (100, (top.getWidth() - 12) / 4)));
        top.removeFromLeft (4);
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

    auto qualityColumn = knobArea.removeFromLeft (90).withTrimmedTop (0);
    qualityLabel.setBounds (qualityColumn.removeFromTop (18));
    qualityBox.setBounds (qualityColumn.removeFromTop (26).reduced (4, 0));

    controls.removeFromLeft (8);
    statusBox.setBounds (controls);

    area.removeFromTop (8);
    chainLabel.setBounds (area.removeFromTop (20));
    chainViewport.setBounds (area);
    chainView.layout (juce::jmax (100, chainViewport.getWidth() - chainViewport.getScrollBarThickness() - 4));
}
