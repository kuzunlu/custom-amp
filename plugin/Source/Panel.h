#pragma once

#include "Help.h"

#include <vector>

// One knob of the front panel: a handle on one number setting of one block. The knob reads 0 to 10.
struct PanelKnob
{
    juce::String label, unit;
    juce::StringArray blockPath;    // where the block is in the preset: property names and list places from the top
    juce::StringArray settingPath;  // "level_db", or "path", "0", "gain_db"
    juce::String blockId;           // empty for a guessed knob on a block without an id
    double atZero = 0.0, atTen = 10.0;  // the setting's value at 0 and at 10 on the knob
    double defaultValue = 0.0;          // used when the preset does not set it

    double toValue (double position) const   { return atZero + (atTen - atZero) * position / 10.0; }
    double toPosition (double value) const   { return juce::jlimit (0.0, 10.0, 10.0 * (value - atZero) / (atTen - atZero)); }
};

namespace panel
{
constexpr int maxKnobs = 8;

// The preset's front panel. A wrong "panel" list throws amp::PresetError (the messages are fixed by
// plugin/tests/panel_cases.json). Without a "panel" list it guesses knobs (a tone stack's three, the last eq's
// level) and sets `guessed`.
std::vector<PanelKnob> read (const juce::var& preset, const SettingsHelp& help, bool& guessed);

// The setting's value in the preset (its default when it is not set), and setting a new one (in place).
double getValue (const juce::var& preset, const PanelKnob& knob);
void setValue (const juce::var& preset, const PanelKnob& knob, double value);

// Editing the panel from the plugin window; all change `preset` in place. A guessed panel is first written into
// the preset (its blocks get ids). addKnob returns a message for the user when it can not add the knob.
juce::String addKnob (const juce::var& preset, const juce::var& block, const juce::String& setting, const SettingsHelp& help);
void removeKnob (const juce::var& preset, int index, const SettingsHelp& help);
void renameKnob (const juce::var& preset, int index, const juce::String& label, const SettingsHelp& help);
}
