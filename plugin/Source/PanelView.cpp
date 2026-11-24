#include "PanelView.h"

#include "Theme.h"

namespace
{
const float startAngle = juce::MathConstants<float>::pi * 1.25f;  // 0 on the knob: down-left, like an amp knob
const float endAngle = juce::MathConstants<float>::pi * 2.75f;    // 10: down-right
const int knobWidth = 108, knobHeight = 134;
}

// Amp knobs: the numbers 0 to 10 around a dark skirt, a pointer line on the cap.
class PanelView::Look final : public juce::LookAndFeel_V4
{
public:
    void drawRotarySlider (juce::Graphics& g, int x, int y, int width, int height, float position, float start, float end,
                           juce::Slider& slider) override
    {
        const auto area = juce::Rectangle<float> ((float) x, (float) y, (float) width, (float) height);
        const auto centre = area.getCentre();
        const float radius = juce::jmin (area.getWidth(), area.getHeight()) / 2.0f - 13.0f;
        auto at = [centre] (float angle, float r) { return centre + juce::Point<float> (r * std::sin (angle), -r * std::cos (angle)); };

        g.setColour (theme::muted);
        g.setFont (juce::FontOptions (11.0f));

        for (int i = 0; i <= 10; ++i)
        {
            const auto p = at (start + (end - start) * (float) i / 10.0f, radius + 8.0f);
            g.drawText (juce::String (i), juce::Rectangle<float> (16.0f, 12.0f).withCentre (p), juce::Justification::centred);
        }

        g.setColour (juce::Colour (0xff15171a));
        g.fillEllipse (juce::Rectangle<float> (radius * 2.0f, radius * 2.0f).withCentre (centre));
        g.setColour (juce::Colour (0xff4a5058));
        g.drawEllipse (juce::Rectangle<float> (radius * 2.0f, radius * 2.0f).withCentre (centre), 1.0f);
        g.setColour (juce::Colour (0xff3d434a));
        g.fillEllipse (juce::Rectangle<float> (radius * 1.24f, radius * 1.24f).withCentre (centre));

        const float angle = start + position * (end - start);
        g.setColour (slider.isMouseOverOrDragging() || slider.hasKeyboardFocus (false) ? theme::accent : juce::Colour (0xfff2efe8));
        g.drawLine (juce::Line<float> (at (angle, radius * 0.25f), at (angle, radius * 0.92f)), 3.0f);
    }
};

PanelView::PanelView (CustomAmpProcessor& p) : look (std::make_unique<Look>())
{
    for (int i = 0; i < panel::maxKnobs; ++i)
    {
        auto knob = std::make_unique<Knob>();
        auto& slider = knob->slider;
        slider.setLookAndFeel (look.get());
        slider.setSliderStyle (juce::Slider::RotaryHorizontalVerticalDrag);
        slider.setTextBoxStyle (juce::Slider::NoTextBox, true, 0, 0);
        slider.setRotaryParameters (startAngle, endAngle, true);
        slider.setComponentID ("panel_knob_" + juce::String (i + 1));
        slider.onValueChange = [this, i] { showValue (i); };
        slider.onMenu = [this, i] { showMenu (i); };
        knob->attachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment> (p.parameters, "knob_" + juce::String (i + 1), slider);

        knob->name.setJustificationType (juce::Justification::centred);
        knob->name.setFont (juce::FontOptions (15.0f, juce::Font::bold));
        knob->name.setEditable (false, true, false);  // double-click to rename
        knob->name.onTextChange = [this, i]
        {
            if (onRename != nullptr)
                onRename (i, knobs[(size_t) i]->name.getText());
        };

        knob->value.setJustificationType (juce::Justification::centred);
        knob->value.setFont (juce::FontOptions (12.0f));
        knob->value.setColour (juce::Label::textColourId, theme::muted);

        addChildComponent (slider);
        addChildComponent (knob->name);
        addChildComponent (knob->value);
        knobs.push_back (std::move (knob));
    }

    note.setColour (juce::Label::textColourId, theme::muted);
    note.setFont (juce::FontOptions (13.0f));
    note.setJustificationType (juce::Justification::centred);
    addAndMakeVisible (note);
}

PanelView::~PanelView()
{
    for (auto& k : knobs)
        k->slider.setLookAndFeel (nullptr);
}

void PanelView::setPanel (const std::vector<PanelKnob>& newPanel, bool guessed, const juce::String& name)
{
    panel = newPanel;
    presetName = name;
    shown = juce::jmin ((int) panel.size(), panel::maxKnobs);

    for (int i = 0; i < panel::maxKnobs; ++i)
    {
        auto& k = *knobs[(size_t) i];
        const bool visible = i < shown;

        for (auto* c : { (juce::Component*) &k.slider, (juce::Component*) &k.name, (juce::Component*) &k.value })
            c->setVisible (visible);

        if (! visible)
            continue;

        const auto& knob = panel[(size_t) i];
        k.name.setText (knob.label, juce::dontSendNotification);
        const auto unit = knob.unit.isNotEmpty() ? " " + knob.unit : juce::String();
        const auto tip = knob.label + ": " + knob.settingPath.joinIntoString (".") + " of "
                         + (knob.blockId.isNotEmpty() ? "block '" + knob.blockId + "'" : juce::String ("the block"))
                         + "\n0 on the knob = " + juce::String (knob.atZero) + unit + ", 10 = " + juce::String (knob.atTen) + unit
                         + "\nYour DAW calls it Knob " + juce::String (i + 1) + ". Right-click to rename or remove it.";
        k.slider.setTooltip (tip);
        k.name.setTooltip ("Double-click to rename.");
        showValue (i);
    }

    if (guessed)
        note.setText (shown > 0 ? "This preset has no front panel of its own, so these knobs are a guess. To add a knob: Edit chain, "
                                  "right-click a slider, Put on the front panel."
                                : "This preset has no front panel yet. To add a knob: Edit chain, right-click a slider, "
                                  "Put on the front panel.",
                      juce::dontSendNotification);
    else
        note.setText (shown > 0 ? "Drag a knob up or down. Right-click a knob to rename or remove it."
                                : "This preset's front panel has no knobs. To add one: Edit chain, right-click a slider, "
                                  "Put on the front panel.",
                      juce::dontSendNotification);

    resized();
    repaint();
}

void PanelView::showValue (int index)
{
    if (index >= shown)
        return;

    const auto& knob = panel[(size_t) index];
    const double v = knob.toValue (knobs[(size_t) index]->slider.getValue());
    const double span = std::abs (knob.atTen - knob.atZero);
    const int decimals = span >= 100.0 ? 0 : span >= 10.0 ? 1 : 2;
    knobs[(size_t) index]->value.setText (juce::String (v, decimals) + (knob.unit.isNotEmpty() ? " " + knob.unit : juce::String()),
                                          juce::dontSendNotification);
}

void PanelView::showMenu (int index)
{
    juce::PopupMenu menu;
    menu.addItem (1, "Rename...");
    menu.addItem (2, "Remove from the front panel");
    menu.addItem (3, "Show in the chain editor");
    juce::Component::SafePointer<PanelView> safe (this);

    menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&knobs[(size_t) index]->slider), [safe, index] (int result)
    {
        if (safe == nullptr || index >= safe->shown)
            return;

        if (result == 1)
            safe->knobs[(size_t) index]->name.showEditor();
        else if (result == 2 && safe->onRemove != nullptr)
            safe->onRemove (index);
        else if (result == 3 && safe->onShowInChain != nullptr)
            safe->onShowInChain (index);
    });
}

void PanelView::paint (juce::Graphics& g)
{
    const auto face = getLocalBounds().toFloat().reduced (1.0f);
    g.setGradientFill (juce::ColourGradient::vertical (juce::Colour (0xff31363c), face.getY(), juce::Colour (0xff25292d), face.getBottom()));
    g.fillRoundedRectangle (face, 10.0f);
    g.setColour (juce::Colour (0xff454b53));
    g.drawRoundedRectangle (face, 10.0f, 1.0f);

    // The name plate: the plugin's name on the left, the preset's on the right.
    auto plate = plateArea;
    g.setColour (theme::accent);
    g.setFont (juce::FontOptions (22.0f, juce::Font::bold).withKerningFactor (0.18f));
    g.drawText ("CUSTOM AMP", plate.removeFromLeft (220), juce::Justification::centredLeft);
    g.setColour (theme::muted);
    g.setFont (juce::FontOptions (16.0f));
    g.drawText (presetName, plate, juce::Justification::centredRight, true);
    g.setColour (juce::Colour (0xff454b53));
    g.drawHorizontalLine (plateArea.getBottom() + 6, (float) plateArea.getX(), (float) plateArea.getRight());
}

void PanelView::resized()
{
    auto area = getLocalBounds().reduced (20, 14);
    plateArea = area.removeFromTop (34);
    area.removeFromTop (16);
    note.setBounds (area.removeFromBottom (40));

    // Bigger knobs when there is room (up to 1.4 times), as many in a row as fit, rows centred.
    const int labels = knobHeight - knobWidth;
    const int width = juce::jlimit (knobWidth, knobWidth * 14 / 10,
                                    juce::jmin (area.getWidth() / juce::jmax (1, shown), area.getHeight() - labels));
    const int height = width + labels;
    const int perRow = juce::jmax (1, area.getWidth() / width);
    const int rows = (shown + perRow - 1) / perRow;
    int y = area.getY() + juce::jmax (0, (area.getHeight() - rows * height) / 2);

    for (int row = 0, i = 0; row < rows; ++row, y += height)
    {
        const int inRow = juce::jmin (perRow, shown - i);
        int x = area.getCentreX() - inRow * width / 2;

        for (int k = 0; k < inRow; ++k, ++i, x += width)
        {
            auto& knob = *knobs[(size_t) i];
            knob.slider.setBounds (x + 4, y, width - 8, width - 8);
            knob.name.setBounds (x, y + width - 8, width, 20);
            knob.value.setBounds (x, y + width + 12, width, 16);
        }
    }
}
