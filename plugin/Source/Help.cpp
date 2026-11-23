#include "Help.h"

#include "BinaryData.h"

SettingsHelp::SettingsHelp()
{
    doc = juce::JSON::parse (juce::String::fromUTF8 (BinaryData::settings_json, BinaryData::settings_jsonSize));
}

juce::String SettingsHelp::tip (const juce::var& entry)
{
    if (! entry.isObject())
        return {};

    juce::String text = entry.getProperty ("label", "").toString();
    const auto shortText = entry.getProperty ("short", "").toString();

    if (shortText.isNotEmpty())
        text << ": " << shortText;

    text << "\n\n" << entry.getProperty ("tooltip", "").toString();

    const auto unit = entry.getProperty ("unit", "").toString();
    juce::StringArray facts;

    if (entry.hasProperty ("min") && entry.hasProperty ("max"))
        facts.add ("Range " + entry["min"].toString() + " to " + entry["max"].toString() + (unit.isNotEmpty() ? " " + unit : ""));

    if (entry.hasProperty ("default") && ! entry["default"].isVoid() && ! entry["default"].isArray() && ! entry["default"].isObject())
        facts.add ("default " + entry["default"].toString());

    if (! facts.isEmpty())
        text << "\n\n" << facts.joinIntoString (", ") << ".";

    return text.trim();
}

juce::StringArray SettingsHelp::blockTypes() const
{
    juce::StringArray types;

    if (auto* obj = doc["common"]["settings"]["type"]["choices"].getDynamicObject())
        for (auto& p : obj->getProperties())
            types.add (p.name.toString());

    return types;
}

juce::String SettingsHelp::settingTip (const juce::String& type, const juce::String& key) const
{
    auto entry = blockSettings (type)[key.toRawUTF8()];

    if (! entry.isObject())
        entry = doc["common"]["settings"][key.toRawUTF8()];

    return tip (entry);
}
