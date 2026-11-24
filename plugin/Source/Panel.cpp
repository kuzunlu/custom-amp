#include "Panel.h"

#include "Inspector.h"
#include "dsp/Common.h"

namespace
{
bool isNumber (const juce::var& v)
{
    return v.isInt() || v.isInt64() || v.isDouble();
}

juce::var follow (juce::var node, const juce::StringArray& path)
{
    for (auto& step : path)
        node = node.isArray() ? node[step.getIntValue()] : node.getProperty (juce::Identifier (step), {});

    return node;
}

// The block with this id: in the chain, inside feedback loops and inside split paths.
juce::var findBlock (const juce::var& chain, const juce::String& id, juce::StringArray& path)
{
    for (int i = 0; chain.isArray() && i < chain.size(); ++i)
    {
        const auto block = chain[i];
        path.add (juce::String (i));

        if (block["id"].isString() && block["id"].toString() == id)
            return block;

        path.add ("chain");

        if (auto found = findBlock (block["chain"], id, path); ! found.isVoid())
            return found;

        path.remove (path.size() - 1);
        path.add ("paths");

        for (int k = 0; block["paths"].isArray() && k < block["paths"].size(); ++k)
        {
            path.add (juce::String (k));
            path.add ("chain");

            if (auto found = findBlock (block["paths"][k]["chain"], id, path); ! found.isVoid())
                return found;

            path.removeRange (path.size() - 2, 2);
        }

        path.removeRange (path.size() - 2, 2);
    }

    return {};
}

// The settings.json entry of the number setting that `setting` names.
juce::var findSetting (const juce::var& block, const juce::String& setting, const SettingsHelp& help, const juce::String& where)
{
    const auto parts = juce::StringArray::fromTokens (setting, ".", "");
    auto entries = help.blockSettings (block["type"].toString());
    auto node = block;
    const auto notANumber = where + ": '" + setting + "' is not a number setting of block '" + block["id"].toString() + "'";

    for (int i = 0;;)
    {
        const auto entry = entries.getProperty (juce::Identifier (parts[i].isEmpty() ? juce::String ("_") : parts[i]), {});

        if (! entry.isObject())
            throw amp::PresetError (notANumber);

        const auto type = entry["type"].toString();

        if (type == "number")
        {
            if (i != parts.size() - 1)
                throw amp::PresetError (notANumber);

            return entry;
        }

        if ((type != "band-list" && type != "path-list") || i + 2 >= parts.size())
            throw amp::PresetError (notANumber);

        const auto items = node.getProperty (juce::Identifier (parts[i]), {});
        const auto index = parts[i + 1];

        if (index.isEmpty() || ! index.containsOnly ("0123456789") || index.getIntValue() >= (items.isArray() ? items.size() : 0))
            throw amp::PresetError (where + ": block '" + block["id"].toString() + "' has no item " + index + " in '" + parts[i] + "'");

        node = items[index.getIntValue()];
        entries = help.section (type == "band-list" ? "band" : "split_path")["settings"];
        i += 2;
    }
}

PanelKnob makeKnob (const juce::var& entry, const juce::String& label, const juce::StringArray& blockPath,
                    const juce::String& blockId, const juce::String& setting, const juce::var& range)
{
    PanelKnob k;
    k.label = label.isNotEmpty() ? label : entry["label"].toString();
    k.unit = entry["unit"].toString();
    k.blockPath = blockPath;
    k.blockId = blockId;
    k.settingPath = juce::StringArray::fromTokens (setting, ".", "");
    k.atZero = range.isArray() ? (double) range[0] : (double) entry["min"];
    k.atTen = range.isArray() ? (double) range[1] : (double) entry["max"];
    k.defaultValue = isNumber (entry["default"]) ? (double) entry["default"] : k.atZero;
    return k;
}

// Without a "panel" list: a tone stack's three knobs and the last eq's level, in the main chain.
std::vector<PanelKnob> guess (const juce::var& preset, const SettingsHelp& help)
{
    std::vector<PanelKnob> knobs;
    const auto chain = preset["chain"];
    int tonestack = -1, lastEq = -1;

    for (int i = 0; chain.isArray() && i < chain.size(); ++i)
    {
        const auto type = chain[i]["type"].toString();

        if (type == "tonestack" && tonestack < 0)
            tonestack = i;

        if (type == "eq")
            lastEq = i;
    }

    if (tonestack >= 0)
        for (auto [label, setting] : { std::pair { "Bass", "bass" }, std::pair { "Middle", "mid" }, std::pair { "Treble", "treble" } })
            knobs.push_back (makeKnob (help.blockSettings ("tonestack")[setting], label, { "chain", juce::String (tonestack) },
                                       chain[tonestack]["id"].toString(), setting, {}));

    if (lastEq >= 0)
        knobs.push_back (makeKnob (help.blockSettings ("eq")["level_db"], "Master", { "chain", juce::String (lastEq) },
                                   chain[lastEq]["id"].toString(), "level_db", {}));

    return knobs;
}

// The object that holds the setting (the block, a band or a split path) and the setting's name in it.
juce::var holderOf (const juce::var& preset, const PanelKnob& knob)
{
    auto node = follow (preset, knob.blockPath);

    for (int i = 0; i + 1 < knob.settingPath.size(); i += 2)
        node = node[knob.settingPath[i].toRawUTF8()][knob.settingPath[i + 1].getIntValue()];

    return node;
}

// A guessed panel is written into the preset before it is changed, so its knobs stay.
void makeExplicit (const juce::var& preset, const SettingsHelp& help)
{
    auto* obj = preset.getDynamicObject();

    if (obj == nullptr || preset.hasProperty ("panel"))
        return;

    juce::Array<juce::var> list;

    for (auto& k : guess (preset, help))
    {
        auto block = follow (preset, k.blockPath);
        juce::String id = block["id"].toString();

        if (id.isEmpty())
        {
            juce::StringArray taken;
            collectIds (preset, taken);
            const auto base = block["type"].toString();
            id = base;

            for (int n = 2; taken.contains (id); ++n)
                id = base + "_" + juce::String (n);

            if (auto* b = block.getDynamicObject())
                b->setProperty ("id", id);
        }

        auto* knob = new juce::DynamicObject();
        knob->setProperty ("label", k.label);
        knob->setProperty ("block", id);
        knob->setProperty ("setting", k.settingPath.joinIntoString ("."));
        list.add (juce::var (knob));
    }

    obj->setProperty ("panel", list);
}
}

namespace panel
{
std::vector<PanelKnob> read (const juce::var& preset, const SettingsHelp& help, bool& guessed)
{
    guessed = ! preset.hasProperty ("panel");

    if (guessed)
        return guess (preset, help);

    const auto list = preset["panel"];

    if (! list.isArray())
        throw amp::PresetError ("panel must be a list of knobs");

    if (list.size() > maxKnobs)
        throw amp::PresetError ("panel has " + juce::String (list.size()) + " knobs; at most " + juce::String (maxKnobs) + " fit");

    std::vector<PanelKnob> knobs;

    for (int n = 1; n <= list.size(); ++n)
    {
        const auto knob = list[n - 1];
        const auto where = "panel knob " + juce::String (n);

        if (! knob.isObject())
            throw amp::PresetError (where + ": must be an object with 'block' and 'setting'");

        amp::checkKeys (knob, { "label", "block", "setting", "range" }, where);

        if (! knob["block"].isString() || ! knob["setting"].isString())
            throw amp::PresetError (where + ": needs 'block' (a block id) and 'setting' (its name)");

        if (knob.hasProperty ("label") && ! knob["label"].isString())
            throw amp::PresetError (where + ": 'label' must be text");

        const auto range = knob["range"];

        if (knob.hasProperty ("range")
            && ! (range.isArray() && range.size() == 2 && isNumber (range[0]) && isNumber (range[1]) && (double) range[0] != (double) range[1]))
            throw amp::PresetError (where + ": 'range' must be two different numbers, like [-40, 0]");

        const auto id = knob["block"].toString();
        juce::StringArray blockPath { "chain" };
        const auto block = findBlock (preset["chain"], id, blockPath);

        if (block.isVoid())
            throw amp::PresetError (where + ": no block has the id '" + id + "'");

        const auto entry = findSetting (block, knob["setting"].toString(), help, where);
        knobs.push_back (makeKnob (entry, knob["label"].toString(), blockPath, id, knob["setting"].toString(), range));
    }

    return knobs;
}

double getValue (const juce::var& preset, const PanelKnob& knob)
{
    const auto value = holderOf (preset, knob)[knob.settingPath[knob.settingPath.size() - 1].toRawUTF8()];
    return isNumber (value) ? (double) value : knob.defaultValue;
}

void setValue (const juce::var& preset, const PanelKnob& knob, double value)
{
    if (auto* obj = holderOf (preset, knob).getDynamicObject())
        obj->setProperty (knob.settingPath[knob.settingPath.size() - 1], tidyNumber (value));
}

juce::String addKnob (const juce::var& preset, const juce::var& block, const juce::String& setting, const SettingsHelp& help)
{
    makeExplicit (preset, help);
    auto* list = preset["panel"].getArray();
    auto* blockObject = block.getDynamicObject();

    if (list == nullptr || blockObject == nullptr)
        return "This preset's panel can not be changed here (fix the 'panel' list in the file).";

    if (list->size() >= maxKnobs)
        return "The front panel is full (" + juce::String (maxKnobs) + " knobs). Remove one first: right-click it on the panel.";

    auto id = block["id"].toString();

    if (id.isEmpty())
    {
        juce::StringArray taken;
        collectIds (preset, taken);
        const auto base = block["type"].toString();
        id = base;

        for (int n = 2; taken.contains (id); ++n)
            id = base + "_" + juce::String (n);

        blockObject->setProperty ("id", id);
    }

    for (auto& k : *list)
        if (k["block"].toString() == id && k["setting"].toString() == setting)
            return "That setting is already on the front panel.";

    juce::var entry;

    try
    {
        entry = findSetting (block, setting, help, "knob");
    }
    catch (const amp::PresetError& e)
    {
        return e.what();
    }

    auto* knob = new juce::DynamicObject();
    knob->setProperty ("label", entry["label"]);
    knob->setProperty ("block", id);
    knob->setProperty ("setting", setting);
    list->add (juce::var (knob));
    return {};
}

void removeKnob (const juce::var& preset, int index, const SettingsHelp& help)
{
    makeExplicit (preset, help);

    if (auto* list = preset["panel"].getArray())
        list->remove (index);
}

void renameKnob (const juce::var& preset, int index, const juce::String& label, const SettingsHelp& help)
{
    makeExplicit (preset, help);

    if (auto* list = preset["panel"].getArray())
        if (auto* knob = (*list)[index].getDynamicObject())
            knob->setProperty ("label", label.trim());
}
}
