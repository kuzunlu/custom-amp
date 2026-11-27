#include "ChainView.h"

#include "Theme.h"

namespace
{
// A label that reports clicks, and the start of a drag (once the mouse has moved a few pixels).
class ClickLabel : public juce::Label
{
public:
    std::function<void()> onClick, onMenu, onDrag;

    void mouseDown (const juce::MouseEvent&) override { dragStarted = false; }

    void mouseDrag (const juce::MouseEvent& e) override
    {
        if (onDrag != nullptr && ! dragStarted && ! e.mods.isPopupMenu() && e.getDistanceFromDragStart() > 5)
        {
            dragStarted = true;
            onDrag();
        }
    }

    void mouseUp (const juce::MouseEvent& e) override
    {
        if (e.mods.isPopupMenu() && onMenu != nullptr)
            onMenu();
        else if (onClick != nullptr && e.mouseWasClicked())
            onClick();
    }

private:
    bool dragStarted = false;
};

constexpr int indentWidth = 24;

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

        if (isHeader && ! target.isPath)
            label->onMenu = [this, target] { if (onBlockMenu != nullptr) onBlockMenu (target); };

        if (isHeader)
            label->onDrag = [this, target, source = label.get()]
            {
                auto* container = juce::DragAndDropContainer::findParentDragContainerFor (this);

                if (container != nullptr && ! container->isDragAndDropActive())
                {
                    dragged = target;
                    container->startDragging ("move", source);
                }
            };
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
        slots.push_back ({ list, i, (int) items.size(), indent, false, false });  // above this block
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
                slots.push_back ({ paths, k, (int) items.size(), indent + 1, true, false });
                addItem ("path " + juce::String (k + 1), help.sectionTip ("split_path"), pathHere, indent + 1, true, true, pathSelected);

                for (auto* key : { "level_db", "invert" })
                    if (path.hasProperty (key))
                        addItem (formatValue (key, path[key]), help.sectionSettingTip ("split_path", key), pathHere,
                                 indent + 1, false, false, pathSelected);

                addBlocks (path["chain"], indent + 2, selected);
            }

            slots.push_back ({ paths, paths.size(), (int) items.size() - 1, indent + 1, true, true });  // after the last path
        }
    }

    // The end of this list (of an empty one: just under the block or path it belongs to).
    slots.push_back ({ list, list.size(), (int) items.size() - 1, indent, false, true });
}

void ChainView::setPreset (const juce::var& preset, const Selection& selected)
{
    items.clear();
    slots.clear();
    dropSlot = -1;

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
    const int rowHeight = 22, gap = 4;
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

int ChainView::slotY (const Slot& slot) const
{
    if (! juce::isPositiveAndBelow (slot.item, (int) items.size()))
        return 2;

    const auto b = items[(size_t) slot.item].label->getBounds();
    return slot.below ? b.getBottom() + 2 : b.getY() - 2;
}

int ChainView::findSlot (juce::Point<int> where, bool paths) const
{
    // The nearest place; between places at the same height (the end of a loop's chain and the place after the loop),
    // the mouse's x picks the depth: further left = the outer list.
    int best = -1;
    double bestScore = 0.0;
    const int x = juce::jlimit (0, 4 * indentWidth, where.x);

    for (int k = 0; k < (int) slots.size(); ++k)
    {
        if (slots[(size_t) k].paths != paths)
            continue;

        const double score = std::abs (slotY (slots[(size_t) k]) - where.y) + 0.5 * std::abs (x - slots[(size_t) k].indent * indentWidth);

        if (best < 0 || score < bestScore)
        {
            best = k;
            bestScore = score;
        }
    }

    return best;
}

bool ChainView::dropAt (const juce::String& what, const Selection& item, juce::Point<int> where)
{
    const int k = findSlot (where, what == "move" && item.isPath);

    if (k < 0)
        return false;

    const auto slot = slots[(size_t) k];  // a copy: the drop may rebuild the view

    if (onDrop != nullptr)
        onDrop (what, item, slot.list, slot.index, slot.paths);

    return true;
}

juce::Rectangle<int> ChainView::headerBounds (const Selection& target) const
{
    for (auto& item : items)
        if (item.isHeader && item.target == target && item.target.isPath == target.isPath)
            return item.label->getBounds();

    return {};
}

bool ChainView::isInterestedInDragSource (const SourceDetails& details)
{
    const auto what = details.description.toString();
    return what == "move" ? dragged.isValid() : what.startsWith ("new:");
}

void ChainView::itemDragMove (const SourceDetails& details)
{
    const auto what = details.description.toString();
    dropSlot = findSlot (details.localPosition, what == "move" && dragged.isPath);
    repaint();

    // Near the top or bottom of the visible part: scroll, so a block can go anywhere in a long chain.
    if (auto* viewport = findParentComponentOfClass<juce::Viewport>())
    {
        const auto p = viewport->getLocalPoint (this, details.localPosition);
        viewport->autoScroll (p.x, p.y, 24, 12);
    }
}

void ChainView::itemDragExit (const SourceDetails&)
{
    dropSlot = -1;
    repaint();
}

void ChainView::itemDropped (const SourceDetails& details)
{
    dropSlot = -1;
    repaint();
    dropAt (details.description.toString(), dragged, details.localPosition);
}

void ChainView::paintOverChildren (juce::Graphics& g)
{
    if (! juce::isPositiveAndBelow (dropSlot, (int) slots.size()))
        return;

    const auto& slot = slots[(size_t) dropSlot];
    const int x = slot.indent * indentWidth;
    g.setColour (theme::accent);
    g.fillRect (x, slotY (slot) - 1, getWidth() - x, 3);
}

// A block type: drag it into the chain, or click it to add it at the end.
class BlockPalette::Chip final : public ClickLabel
{
public:
    explicit Chip (const juce::String& t) : type (t)
    {
        onDrag = [this]
        {
            if (auto* container = juce::DragAndDropContainer::findParentDragContainerFor (this))
                if (! container->isDragAndDropActive())
                    container->startDragging ("new:" + type, this);
        };
    }

    const juce::String type;
};

// Drop a block from the chain here to take it out.
class BlockPalette::Trash final : public juce::Label,
                                  public juce::DragAndDropTarget
{
public:
    explicit Trash (BlockPalette& p) : owner (p) { show (false); }

    bool isInterestedInDragSource (const SourceDetails& details) override { return details.description.toString() == "move"; }
    void itemDragEnter (const SourceDetails&) override { show (true); }
    void itemDragExit (const SourceDetails&) override { show (false); }

    void itemDropped (const SourceDetails&) override
    {
        show (false);

        if (owner.onRemove != nullptr)
            owner.onRemove();
    }

private:
    void show (bool over)
    {
        setColour (juce::Label::textColourId, over ? theme::background : theme::error);
        setColour (juce::Label::backgroundColourId, over ? theme::error : theme::chip);
    }

    BlockPalette& owner;
};

BlockPalette::BlockPalette (const SettingsHelp& help)
{
    for (auto& type : help.blockTypes())
    {
        auto chip = std::make_unique<Chip> (type);
        chip->setText (type, juce::dontSendNotification);
        chip->setTooltip ("Drag it into the chain to add it there (a line shows where), or click it to add it at the end.\n\n"
                          + help.blockInfo (type)["short"].toString());
        chip->onClick = [this, type] { if (onAdd != nullptr) onAdd (type); };
        chip->setColour (juce::Label::backgroundColourId, theme::chip);
        chip->setColour (juce::Label::textColourId, juce::Colours::white);
        chip->setMouseCursor (juce::MouseCursor::DraggingHandCursor);
        chips.push_back (std::move (chip));
    }

    auto trash = std::make_unique<Trash> (*this);
    trash->setText ("Remove", juce::dontSendNotification);
    trash->setTooltip ("Drag a block (by its name) from the chain onto this to take it out. Undo brings it back.");
    chips.push_back (std::move (trash));

    for (auto& chip : chips)
    {
        chip->setFont (juce::FontOptions (13.0f));
        chip->setBorderSize ({ 1, 6, 1, 6 });
        addAndMakeVisible (*chip);
    }
}

BlockPalette::~BlockPalette() = default;

int BlockPalette::layout (int width)
{
    const int rowHeight = 22, gap = 4;
    int x = 0, y = 0;

    for (auto& chip : chips)
    {
        const int w = juce::jmin (width, juce::GlyphArrangement::getStringWidthInt (chip->getFont(), chip->getText()) + 14);

        if (x > 0 && x + w > width)
        {
            x = 0;
            y += rowHeight + gap;
        }

        chip->setBounds (x, y, w, rowHeight);
        x += w + gap;
    }

    setSize (width, y + rowHeight);
    return getHeight();
}
