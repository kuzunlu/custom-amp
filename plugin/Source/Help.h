#pragma once

#include <juce_core/juce_core.h>

// Help text, ranges and choices for every setting, from shared/settings.json (built into the plugin).
class SettingsHelp
{
public:
    SettingsHelp();

    // Tooltip text for one entry of the file: label, short line, long text, range and default.
    static juce::String tip (const juce::var& entry);

    juce::var blockInfo (const juce::String& type) const       { return doc["blocks"][type.toRawUTF8()]; }
    juce::var blockSettings (const juce::String& type) const   { return blockInfo (type)["settings"]; }
    juce::var section (const juce::String& name) const         { return doc[name.toRawUTF8()]; }
    juce::StringArray blockTypes() const;

    juce::String blockTip (const juce::String& type) const     { return tip (blockInfo (type)); }
    juce::String settingTip (const juce::String& type, const juce::String& key) const;
    juce::String sectionTip (const juce::String& name) const   { return tip (section (name)); }
    juce::String sectionSettingTip (const juce::String& name, const juce::String& key) const
    {
        return tip (section (name)["settings"][key.toRawUTF8()]);
    }

private:
    juce::var doc;
};
