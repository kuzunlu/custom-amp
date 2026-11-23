#pragma once

#include "ChainView.h"

// Edits the selected block: one row per setting from settings.json (set ones bright, defaults dim),
// plus buttons to move, remove and add blocks. Changes go straight into the preset data.
class Inspector final : public juce::Component
{
public:
    explicit Inspector (const SettingsHelp& h);
    ~Inspector() override;

    // An invalid selection shows a hint (and "Paste at the end"). The preset is needed to keep ids unique.
    void show (const Selection& selection, const juce::var& preset);
    void layout (int width);                  // sets its own height to fit
    const Selection& getSelection() const { return selection; }

    // Copy, cut, paste after, duplicate: what the buttons and Ctrl+C/X/V/D do. Paste works without a selection
    // too (at the end of the chain). Each returns false when there is nothing to do or the paste does not fit.
    bool copySelected();
    bool cutSelected();
    bool pasteAfterSelected();
    bool duplicateSelected();
    void removeSelected();

    // Called after every change. rebuild = the rows must be made again (curve changed, band added, ...).
    std::function<void (bool rebuild)> onChanged;
    std::function<void (const Selection&)> onSelect;  // after adding or moving a block

    class Row;  // one setting (defined in Inspector.cpp)

private:
    void addRowsForBlock (const juce::var& block);
    juce::TextButton* addButton (const juce::String& text, const juce::String& tip, std::function<void()> action);
    void addInsideButton (const juce::var& item, bool loopOnly);
    void changed (bool rebuild);
    void moveBy (int offset);
    void showAddMenu (juce::Component* button, juce::var list, int insertAt, bool loopOnly);
    bool insertAfterSelected (const juce::String& text);
    void showProblem (const juce::String& text);

    const SettingsHelp& help;
    Selection selection;
    juce::var preset;
    juce::Label title, summary;
    std::vector<std::unique_ptr<juce::TextButton>> buttons;
    std::vector<std::unique_ptr<Row>> rows;
};

// A new block of each type, with the settings it can not work without.
juce::var makeBlock (const juce::String& type);

// Turns copied text (JSON) into a block, or a split path when asPath, ready to go into `preset`: a new copy whose
// ids are made unique (v2 -> v2_2), with its own sag targets and apply_after following the new names.
// Returns an empty var and sets `problem` when the text is not a block (or a path).
juce::var makePastedItem (const juce::String& text, const juce::var& preset, bool asPath, juce::String& problem);

// Numbers in the preset are rounded to 4 significant digits, and whole numbers are written as whole numbers.
juce::var tidyNumber (double value);

// "470p", "22n", "33k", "1M", "4.7e-10": a part value in ohms or farads (0 if it does not parse).
double parseEngineering (const juce::String& text);
juce::String formatEngineering (double value);
