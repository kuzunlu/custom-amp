#include "Common.h"

namespace amp
{

static juce::String joinSorted (juce::StringArray names)
{
    names.sort (false);
    return names.joinIntoString (", ");
}

void checkKeys (const juce::var& object, const std::set<juce::String>& allowed, const juce::String& where)
{
    juce::StringArray unknown, allowedNames;

    if (auto* obj = object.getDynamicObject())
        for (auto& property : obj->getProperties())
            if (allowed.count (property.name.toString()) == 0)
                unknown.add (property.name.toString());

    if (unknown.isEmpty())
        return;

    for (auto& name : allowed)
        allowedNames.add (name);

    throw PresetError (where + ": unknown setting(s) " + joinSorted (unknown) + ". Allowed: " + joinSorted (allowedNames));
}

Cfg::Cfg (const juce::var& value, const juce::String& where) : object (value), place (where)
{
    if (object.getDynamicObject() == nullptr)
        throw PresetError (where + ": must be an object, like { \"type\": \"eq\" }");
}

bool Cfg::has (const char* name) const
{
    return ! get (name).isVoid();
}

juce::var Cfg::get (const char* name) const
{
    // A JSON null counts as "not set".
    return object.getDynamicObject()->getProperty (name);
}

double Cfg::number (const char* name, double fallback) const
{
    auto v = get (name);

    if (v.isVoid())
        return fallback;

    if (v.isInt() || v.isInt64() || v.isDouble() || v.isBool())
        return (double) v;

    throw PresetError (place + ": '" + name + "' must be a number");
}

juce::String Cfg::text (const char* name, const juce::String& fallback) const
{
    auto v = get (name);

    if (v.isVoid())
        return fallback;

    if (v.isString())
        return v.toString();

    throw PresetError (place + ": '" + name + "' must be text in quotes");
}

bool Cfg::flag (const char* name, bool fallback) const
{
    auto v = get (name);

    if (v.isVoid())
        return fallback;

    if (v.isBool() || v.isInt() || v.isInt64() || v.isDouble())
        return (bool) v;

    throw PresetError (place + ": '" + name + "' must be true or false");
}

juce::Array<juce::var> Cfg::list (const char* name) const
{
    auto v = get (name);

    if (v.isVoid())
        return {};

    if (auto* array = v.getArray())
        return *array;

    throw PresetError (place + ": '" + name + "' must be a list, like [ ... ]");
}

void Cfg::checkKeys (const std::set<juce::String>& allowed) const
{
    amp::checkKeys (object, allowed, place);
}

} // namespace amp
