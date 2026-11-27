#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include "Help.h"

// One block (or one split path) in the preset: the list that holds it and its place in that list.
// The list is shared with the preset data, so changing it changes the preset.
struct Selection
{
    juce::var list;
    int index = -1;
    bool isPath = false;  // a path of a split block, not a block

    bool isValid() const { return list.isArray() && juce::isPositiveAndBelow (index, list.size()); }
    juce::var item() const { return isValid() ? list[index] : juce::var(); }
    bool operator== (const Selection& other) const { return list.getArray() == other.list.getArray() && index == other.index; }
};

// The blocks of the preset, one row each, with every setting that is set. Hover for help, click to edit.
// Drag a block's name (or a split path's) to move it: a line shows where it goes, also into a feedback loop or a split
// path. Blocks dragged from the BlockPalette ("new:<type>") are added where they are dropped.
class ChainView final : public juce::Component,
                        public juce::DragAndDropTarget
{
public:
    explicit ChainView (const SettingsHelp& h) : help (h) {}

    void setPreset (const juce::var& preset, const Selection& selected);
    void layout (int width);  // sets its own height to fit

    std::function<void (const Selection&)> onSelect;
    std::function<void (const Selection&)> onBlockMenu;  // right-click on a block's name (X-ray listening)
    // A drop: what = "move" (the dragged block or path) or "new:<type>", into `list` at `index` (counted before a move).
    std::function<void (const juce::String& what, const Selection& dragged, const juce::var& list, int index, bool paths)> onDrop;

    const Selection& getDragged() const { return dragged; }  // the block or path being dragged
    // What a drop at `where` does (also used by the test program): false when no place takes it.
    bool dropAt (const juce::String& what, const Selection& item, juce::Point<int> where);
    juce::Rectangle<int> headerBounds (const Selection& target) const;  // where a block's (or path's) name is shown

    static juce::String formatValue (const juce::String& key, const juce::var& value);

    bool isInterestedInDragSource (const SourceDetails& details) override;
    void itemDragMove (const SourceDetails& details) override;
    void itemDragExit (const SourceDetails& details) override;
    void itemDropped (const SourceDetails& details) override;
    void paintOverChildren (juce::Graphics& g) override;

private:
    struct Item
    {
        std::unique_ptr<juce::Label> label;
        Selection target;
        int indent = 0;
        bool startsRow = false, isHeader = false;
    };

    // A place to drop: `index` in `list` (a chain, or a split's paths), drawn just above item `item` or just below it.
    struct Slot
    {
        juce::var list;
        int index = 0, item = -1, indent = 0;
        bool paths = false, below = false;
    };

    void addBlocks (const juce::var& list, int indent, const Selection& selected);
    void addItem (const juce::String& text, const juce::String& tip, const Selection& target, int indent,
                  bool startsRow, bool isHeader, bool isSelected);
    int slotY (const Slot& slot) const;
    int findSlot (juce::Point<int> where, bool paths) const;  // the nearest place for a block (or a path), -1 = none

    const SettingsHelp& help;
    std::vector<Item> items;
    std::vector<Slot> slots;
    Selection dragged;
    int dropSlot = -1;
};

// The block types as chips above the chain: drag one into the chain to add it there, click it to add it at the end.
// Drop a block from the chain on "Remove" to take it out.
class BlockPalette final : public juce::Component
{
public:
    explicit BlockPalette (const SettingsHelp& h);
    ~BlockPalette() override;

    int layout (int width);  // sets its own size to fit, returns the height

    std::function<void (const juce::String& type)> onAdd;  // a click on a chip
    std::function<void()> onRemove;                        // a block dropped on Remove

private:
    class Chip;
    class Trash;
    std::vector<std::unique_ptr<juce::Label>> chips;
};
