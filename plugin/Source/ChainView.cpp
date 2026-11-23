#include "ChainView.h"

#include "Theme.h"

namespace
{
// A label that reports clicks.
class ClickLabel final : public juce::Label
{
public:
    std::function<void()> onClick;

    void mouseUp (const juce::MouseEvent& e) override
    {
        if (onClick != nullptr && e.mouseWasClicked())
            onClick();
    }
};

juce::String formatNumber (double v)
{
    if (v == std::floor (v) && std::abs (v) < 1e15)
        return juce::String ((juce::int64) v);

    return juce::String (v, 6).trimCharactersAtEnd ("0");
}
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

void ChainView::addItem (const juce::String& text, const juce::String& tip, const Selection& target, int indent,
                         bool startsRow, bool isHeader, bool isSelected)
{
    auto label = std::make_unique<ClickLabel>();
    label->setText (text, juce::dontSendNotification);
    label->setTooltip (tip);
    label->setFont (juce::FontOptions (isHeader ? 15.0f : 13.0f, isHeader ? juce::Font::bold : juce::Font::plain));
    label->setColour (juce::Label::textColourId, isHeader ? (isSelected ? theme::background : theme::accent) : juce::Colours::white);
    label->setColour (juce::Label::backgroundColourId, isHeader ? (isSelected ? theme::accent : juce::Colours::transparentBlack)
                                                                : (isSelected ? theme::chipSelected : theme::chip));
    label->setBorderSize ({ 1, 6, 1, 6 });

    if (target.isValid())
    {
        label->setMouseCursor (juce::MouseCursor::PointingHandCursor);
        label->onClick = [this, target] { if (onSelect != nullptr) onSelect (target); };
    }

    Item item;
    item.target = target;
    item.indent = indent;
    item.startsRow = startsRow;
    item.isHeader = isHeader;
    addAndMakeVisible (*label);
    item.label = std::move (label);
    items.push_back (std::move (item));
}

void ChainView::addBlocks (const juce::var& list, int indent, const Selection& selected)
{
    if (! list.isArray())
        return;

    for (int i = 0; i < list.size(); ++i)
    {
        const Selection here { list, i, false };
        const bool isSelected = here == selected;
        const auto block = list[i];
        const auto type = block.getProperty ("type", "?").toString();
        const auto id = block.getProperty ("id", "").toString();
        addItem (juce::String (i + 1) + ". " + type + (id.isNotEmpty() ? "  " + id : juce::String()),
                 help.blockTip (type), here, indent, true, true, isSelected);

        auto* obj = block.getDynamicObject();

        if (obj == nullptr)
            continue;

        for (auto& p : obj->getProperties())
        {
            const auto key = p.name.toString();

            if (key == "type" || key == "id" || key == "chain" || key == "paths")
                continue;

            addItem (formatValue (key, p.value), help.settingTip (type, key), here, indent, false, false, isSelected);
        }

        if (type == "feedback")
            addBlocks (block["chain"], indent + 1, selected);

        if (type == "split" && block["paths"].isArray())
        {
            const auto paths = block["paths"];

            for (int k = 0; k < paths.size(); ++k)
            {
                const Selection pathHere { paths, k, true };
                const bool pathSelected = pathHere == selected;
                const auto path = paths[k];
                addItem ("path " + juce::String (k + 1), help.sectionTip ("split_path"), pathHere, indent + 1, true, true, pathSelected);

                for (auto* key : { "level_db", "invert" })
                    if (path.hasProperty (key))
                        addItem (formatValue (key, path[key]), help.sectionSettingTip ("split_path", key), pathHere,
                                 indent + 1, false, false, pathSelected);

                addBlocks (path["chain"], indent + 2, selected);
            }
        }
    }
}

void ChainView::setPreset (const juce::var& preset, const Selection& selected)
{
    items.clear();

    if (! preset.isObject())
    {
        addItem ("The preset is not valid JSON.", {}, {}, 0, true, true, false);
        return;
    }

    if (auto notes = preset["notes"].toString(); notes.isNotEmpty())
        addItem ("Notes (hover to read)", notes, {}, 0, true, true, false);

    addBlocks (preset["chain"], 0, selected);

    if (! preset["chain"].isArray() || preset["chain"].size() == 0)
        addItem ("No blocks yet. Use 'Add block' above.", {}, {}, 0, true, true, false);
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
