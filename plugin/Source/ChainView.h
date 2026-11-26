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
class ChainView final : public juce::Component
{
public:
    explicit ChainView (const SettingsHelp& h) : help (h) {}

    void setPreset (const juce::var& preset, const Selection& selected);
    void layout (int width);  // sets its own height to fit

    std::function<void (const Selection&)> onSelect;
    std::function<void (const Selection&)> onBlockMenu;  // right-click on a block's name (X-ray listening)

    static juce::String formatValue (const juce::String& key, const juce::var& value);

private:
    struct Item
    {
        std::unique_ptr<juce::Label> label;
        Selection target;
        int indent = 0;
        bool startsRow = false, isHeader = false;
    };

    void addBlocks (const juce::var& list, int indent, const Selection& selected);
    void addItem (const juce::String& text, const juce::String& tip, const Selection& target, int indent,
                  bool startsRow, bool isHeader, bool isSelected);

    const SettingsHelp& help;
    std::vector<Item> items;
};
