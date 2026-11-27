#include "Inspector.h"

#include "MenuSlider.h"
#include "Theme.h"
#include "dsp/Engine.h"

#include <map>
#include <optional>

//==============================================================================
juce::var tidyNumber (double value)
{
    if (value == 0.0 || ! std::isfinite (value))
        return 0;

    const double digits = std::pow (10.0, 3 - std::floor (std::log10 (std::abs (value))));
    const double rounded = std::round (value * digits) / digits;

    if (rounded == std::floor (rounded) && std::abs (rounded) < 1e15)
        return (juce::int64) rounded;

    return rounded;
}

double parseEngineering (const juce::String& text)
{
    auto t = text.trim().removeCharacters (" FfΩ").replace ("ohm", "", true).replace ("u", "µ");

    if (t.isEmpty())
        return 0.0;

    const juce::String prefixes ("pnµmkM");
    const double scales[] { 1e-12, 1e-9, 1e-6, 1e-3, 1e3, 1e6 };
    const auto last = t.getLastCharacter();

    for (int i = 0; i < prefixes.length(); ++i)
        if (last == prefixes[i] && ! t.endsWithIgnoreCase ("e"))
            return t.dropLastCharacters (1).getDoubleValue() * scales[i];

    return t.getDoubleValue();
}

juce::String formatEngineering (double value)
{
    const char* prefixes[] { "p", "n", "u", "m", "", "k", "M" };
    const double scales[] { 1e-12, 1e-9, 1e-6, 1e-3, 1.0, 1e3, 1e6 };

    for (int i = 6; i >= 0; --i)
        if (std::abs (value) >= scales[i] * 0.999 || i == 0)
            return juce::String (value / scales[i], 3).trimCharactersAtEnd ("0").trimCharactersAtEnd (".") + prefixes[i];

    return juce::String (value);
}

static juce::var parseJson (const char* text)
{
    return juce::JSON::parse (juce::String (text));
}

juce::var makeBlock (const juce::String& type)
{
    if (type == "eq")     return parseJson (R"({ "type": "eq", "bands": [ { "kind": "peak", "freq": 1000, "gain_db": 0, "q": 0.7 } ] })");
    if (type == "gain")   return parseJson (R"({ "type": "gain", "drive_db": 12 })");
    if (type == "sag")    return parseJson (R"({ "type": "sag", "targets": [] })");
    if (type == "feedback") return parseJson (R"({ "type": "feedback", "chain": [] })");
    if (type == "split")  return parseJson (R"({ "type": "split", "paths": [ { "chain": [] }, { "chain": [] } ] })");
    if (type == "ir")     return parseJson (R"({ "type": "ir", "file": "cab.wav" })");

    auto* obj = new juce::DynamicObject();
    obj->setProperty ("type", type);
    return juce::var (obj);
}

int endInsertIndex (const juce::var& chain, const juce::var& block)
{
    auto* array = chain.getArray();

    if (array == nullptr)
        return 0;

    // A room block stays last (the sound that leaves the speaker goes back to the guitar); a new room goes there.
    const auto type = block.getProperty ("type", {}).toString();
    const bool hasRoom = ! array->isEmpty() && array->getLast().getProperty ("type", {}).toString() == "room";
    const int end = array->size() - (hasRoom ? 1 : 0);

    if (type == "room")
        return array->size();

    if (type == "ir" || type == "eq")
        return end;

    juce::Array<juce::var> beforeRoom;

    for (int i = 0; i < end; ++i)
        beforeRoom.add ((*array)[i]);

    return amp::tailStart (beforeRoom);
}

// Only these can go inside a feedback loop (the loop solver needs blocks that work one sample at a time).
static bool canGoInLoop (const juce::String& type)
{
    return juce::StringArray { "eq", "tonestack", "gain", "power" }.contains (type);
}

// True when `list` is the inner chain of a feedback loop somewhere in the preset.
static bool isLoopChain (const juce::var& node, const juce::Array<juce::var>* list)
{
    if (auto* array = node.getArray())
    {
        for (auto& v : *array)
            if (isLoopChain (v, list))
                return true;
    }
    else if (auto* object = node.getDynamicObject())
    {
        if (node["type"].toString() == "feedback" && node["chain"].getArray() == list)
            return true;

        for (auto& p : object->getProperties())
            if (isLoopChain (p.value, list))
                return true;
    }

    return false;
}

// True when `list` is `node` itself or any list inside it.
static bool holdsList (const juce::var& node, const juce::Array<juce::var>* list)
{
    if (auto* array = node.getArray())
    {
        if (array == list)
            return true;

        for (auto& v : *array)
            if (holdsList (v, list))
                return true;
    }
    else if (auto* object = node.getDynamicObject())
    {
        for (auto& p : object->getProperties())
            if (holdsList (p.value, list))
                return true;
    }

    return false;
}

juce::String placeProblem (const juce::var& preset, const juce::var& item, bool isPath, const juce::var& list, bool listIsPaths)
{
    if (isPath != listIsPaths)
        return isPath ? "A split path can only go among the paths of a split."
                      : "A block can not go among a split's paths: drop it into a path, under its name.";

    if (! isPath && isLoopChain (preset, list.getArray()) && ! canGoInLoop (item["type"].toString()))
        return "Only eq, tonestack, gain and power blocks can go inside a feedback loop.";

    if (holdsList (item, list.getArray()))
        return "A block can not go inside itself.";

    return {};
}

bool moveItem (const juce::var& preset, const Selection& from, const juce::var& toList, int toIndex, bool toPaths,
               Selection& moved, juce::String& problem)
{
    moved = from;
    auto* source = from.list.getArray();
    auto* target = toList.getArray();

    if (! from.isValid() || target == nullptr || ! juce::isPositiveAndNotGreaterThan (toIndex, target->size()))
        return false;

    if (source == target && (toIndex == from.index || toIndex == from.index + 1))
        return false;  // dropped where it already is

    const auto item = from.item();
    problem = placeProblem (preset, item, from.isPath, toList, toPaths);

    if (problem.isNotEmpty())
        return false;

    source->remove (from.index);

    if (source == target && toIndex > from.index)
        --toIndex;  // it came out of the same list in front of that place

    target->insert (toIndex, item);
    moved = { toList, toIndex, from.isPath };
    return true;
}

void collectIds (const juce::var& node, juce::StringArray& ids)
{
    if (auto* array = node.getArray())
    {
        for (auto& v : *array)
            collectIds (v, ids);
    }
    else if (auto* object = node.getDynamicObject())
    {
        if (node["id"].isString())
            ids.addIfNotAlreadyThere (node["id"].toString());

        for (auto& p : object->getProperties())
            collectIds (p.value, ids);
    }
}

// Gives ids new names, and the settings that name a block (sag targets, a gate's apply_after) with them.
static void renameIds (const juce::var& node, const std::map<juce::String, juce::String>& names)
{
    auto newName = [&names] (const juce::var& v) -> std::optional<juce::String>
    {
        if (auto it = names.find (v.toString()); v.isString() && it != names.end())
            return it->second;

        return std::nullopt;
    };

    if (auto* array = node.getArray())
    {
        for (auto& v : *array)
            renameIds (v, names);
    }
    else if (auto* object = node.getDynamicObject())
    {
        for (auto* key : { "id", "apply_after" })
            if (auto name = newName (node[key]))
                object->setProperty (key, *name);

        if (auto* targets = node["targets"].getArray())
            for (auto& target : *targets)
                if (auto name = newName (target))
                    target = *name;

        for (auto& p : object->getProperties())
            renameIds (p.value, names);
    }
}

juce::var makePastedItem (const juce::String& text, const juce::var& preset, bool asPath, juce::String& problem)
{
    juce::var item;

    if (juce::JSON::parse (text, item).failed() || ! item.isObject())
    {
        problem = "There is no block to paste (copy one first: select it and press Copy).";
        return {};
    }

    if (asPath != ! item.hasProperty ("type"))
    {
        problem = asPath ? "That is a block: it can not go between split paths. Select a block to paste it after."
                         : "That is a split path, not a block: select a split path to paste it after.";
        return {};
    }

    // Ids must be unique, so ids that are taken get a number: v2 -> v2_2, and v2_2 -> v2_3.
    juce::StringArray taken, inside;
    collectIds (preset, taken);
    collectIds (item, inside);
    std::map<juce::String, juce::String> names;

    for (auto& id : inside)
    {
        if (! taken.contains (id))
            continue;

        auto base = id;
        const auto number = id.fromLastOccurrenceOf ("_", false, false);

        if (id.contains ("_") && number.isNotEmpty() && number.containsOnly ("0123456789"))
            base = id.upToLastOccurrenceOf ("_", false, false);

        int n = 2;

        while (taken.contains (base + "_" + juce::String (n)) || inside.contains (base + "_" + juce::String (n)))
            ++n;

        names[id] = base + "_" + juce::String (n);
        taken.add (names[id]);
    }

    renameIds (item, names);
    return item;
}

//==============================================================================
class Inspector::Row : public juce::Component
{
public:
    virtual int heightFor (int width) const = 0;
};

namespace
{
const int lineHeight = 24;

void styleName (juce::Label& label, bool isSet)
{
    label.setColour (juce::Label::textColourId, isSet ? juce::Colours::white : theme::muted);
    label.setFont (juce::FontOptions (14.0f, isSet ? juce::Font::bold : juce::Font::plain));
}

void styleNote (juce::Label& label)
{
    label.setColour (juce::Label::textColourId, theme::muted);
    label.setFont (juce::FontOptions (12.0f));
    label.setMinimumHorizontalScale (1.0f);
}

void setupSlider (juce::Slider& slider, const juce::var& entry, double value)
{
    double lo = entry.hasProperty ("min") ? (double) entry["min"] : 0.0;
    double hi = entry.hasProperty ("max") ? (double) entry["max"] : 1.0;
    lo = juce::jmin (lo, value);
    hi = juce::jmax (hi, value);

    if (hi <= lo)
        hi = lo + 1.0;

    slider.setSliderStyle (juce::Slider::LinearHorizontal);
    slider.setTextBoxStyle (juce::Slider::TextBoxRight, false, 78, 20);
    const double step = entry.hasProperty ("step") ? (double) entry["step"] : 0.0;  // "step": 1 = whole numbers only
    slider.setRange (lo, hi, step);

    if (entry["scale"].toString() == "log" && lo > 0.0)
        slider.setSkewFactorFromMidPoint (std::sqrt (lo * hi));

    const double span = hi - lo;
    slider.setNumDecimalPlacesToDisplay (step >= 1.0 || span >= 100.0 ? 0 : span >= 10.0 ? 1 : span >= 1.0 ? 2 : 3);

    const auto unit = entry["unit"].toString();
    slider.setTextValueSuffix (unit.isNotEmpty() ? " " + unit : juce::String());
    slider.setValue (value, juce::dontSendNotification);
}

// The right-click menu of a number slider.
void offerPanelMenu (juce::Component* slider, std::function<void()> putOnPanel)
{
    if (putOnPanel == nullptr)
        return;

    juce::PopupMenu menu;
    menu.addItem (1, "Put on the front panel");
    juce::Component::SafePointer<juce::Component> safe (slider);
    menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (slider), [safe, putOnPanel] (int result)
    {
        if (result == 1 && safe != nullptr)  // not if the row was rebuilt meanwhile
            putOnPanel();
    });
}

// One setting of an object (a block, a split path): the name, a control, a reset button, the short help.
class ValueRow final : public Inspector::Row
{
public:
    std::function<void()> putOnPanel;  // set for the number settings of blocks

    ValueRow (juce::var objectIn, const juce::String& keyIn, const juce::var& entryIn, const juce::String& noteText,
              std::function<void (bool)> changedIn, bool rebuildOnChange)
        : object (objectIn), key (keyIn), entry (entryIn), changed (std::move (changedIn)), rebuild (rebuildOnChange)
    {
        const auto tipText = SettingsHelp::tip (entry) + "\n\nName in the preset file: " + key;
        name.setText (entry.isObject() ? entry["label"].toString() : key, juce::dontSendNotification);
        name.setTooltip (tipText);
        addAndMakeVisible (name);

        note.setText (noteText, juce::dontSendNotification);
        note.setTooltip (tipText);
        styleNote (note);
        addAndMakeVisible (note);

        reset.setButtonText ("x");
        reset.setTooltip ("Remove this setting from the preset, so the default is used.");
        reset.onClick = [this]
        {
            if (auto* obj = object.getDynamicObject())
                obj->removeProperty (key);

            changed (true);
        };
        addAndMakeVisible (reset);

        const auto type = entry.isObject() ? entry["type"].toString() : juce::String ("unknown");
        const auto value = object[key.toRawUTF8()];
        const bool isSet = ! value.isVoid();

        if (type == "number")
        {
            const auto fallback = entry["default"];
            const double start = isSet ? (double) value : (fallback.isVoid() ? (double) entry.getProperty ("min", 0.0) : (double) fallback);
            setupSlider (slider, entry, start);
            slider.setTooltip (tipText);
            slider.setComponentID (key);
            slider.onValueChange = [this] { set (tidyNumber (slider.getValue())); };
            slider.onMenu = [this] { offerPanelMenu (&slider, putOnPanel); };
            addAndMakeVisible (slider);
            control = &slider;
        }
        else if (type == "choice")
        {
            int id = 1, selected = 0;
            bool numeric = true;
            const auto current = isSet ? value.toString() : entry["default"].toString();

            if (auto* choices = entry["choices"].getDynamicObject())
                for (auto& c : choices->getProperties())
                {
                    const auto choice = c.name.toString();
                    numeric = numeric && choice.containsOnly ("0123456789");
                    combo.addItem (choice, id);

                    if (choice == current)
                        selected = id;

                    ++id;
                }

            combo.setSelectedId (selected, juce::dontSendNotification);
            combo.setTooltip (tipText);
            combo.setComponentID (key);
            combo.onChange = [this, numeric]
            {
                const auto text = combo.getText();
                set (numeric ? juce::var (text.getIntValue()) : juce::var (text));
            };
            addAndMakeVisible (combo);
            control = &combo;
        }
        else if (type == "toggle")
        {
            toggle.setToggleState (isSet ? (bool) value : (bool) entry["default"], juce::dontSendNotification);
            toggle.setTooltip (tipText);
            toggle.setComponentID (key);
            toggle.onClick = [this] { set (toggle.getToggleState()); };
            addAndMakeVisible (toggle);
            control = &toggle;
        }
        else
        {
            // Text-like settings: id, apply_after, file, a list of ids or of other words (notes), or a setting the
            // plugin does not know.
            const bool isList = type == "id-list" || type == "text-list";

            if (isList && value.isArray())
            {
                juce::StringArray ids;

                for (auto& v : *value.getArray())
                    ids.add (v.toString());

                text.setText (ids.joinIntoString (", "), false);
            }
            else if (type == "unknown")
            {
                text.setText (juce::JSON::toString (value, true), false);
            }
            else
            {
                text.setText (value.toString(), false);
            }

            text.setTextToShowWhenEmpty (type == "id-list" ? "ids, separated by commas" : type == "text-list" ? "separated by commas" : "not set",
                                         theme::muted);
            text.setTooltip (tipText);
            text.setComponentID (key);
            text.onReturnKey = [this, type] { commitText (type); };
            text.onFocusLost = [this, type] { commitText (type); };
            text.setReadOnly (type == "unknown");
            addAndMakeVisible (text);
            control = &text;
        }

        styleName (name, isSet);
        reset.setEnabled (isSet);
    }

    int heightFor (int) const override { return lineHeight + 18; }

    void resized() override
    {
        auto area = getLocalBounds();
        auto top = area.removeFromTop (lineHeight);
        name.setBounds (top.removeFromLeft (juce::jmin (150, top.getWidth() / 3)));
        reset.setBounds (top.removeFromRight (24).reduced (2));
        top.removeFromRight (4);

        if (control != nullptr)
            control->setBounds (control == &toggle ? top.withWidth (30) : top);

        note.setBounds (area.withTrimmedLeft (4));
    }

private:
    void set (const juce::var& value)
    {
        if (auto* obj = object.getDynamicObject())
            obj->setProperty (key, value);

        styleName (name, true);
        reset.setEnabled (true);
        changed (rebuild);
    }

    void commitText (const juce::String& type)
    {
        if (type == "unknown")
            return;

        const auto t = text.getText().trim();
        auto* obj = object.getDynamicObject();

        if (obj == nullptr || t == lastCommitted)
            return;

        lastCommitted = t;

        if (t.isEmpty())
        {
            obj->removeProperty (key);
            changed (false);
            return;
        }

        if (type == "id-list" || type == "text-list")
        {
            juce::Array<juce::var> ids;

            for (auto& id : juce::StringArray::fromTokens (t, ", ", ""))
                if (id.isNotEmpty())
                    ids.add (id);

            set (ids);
        }
        else
        {
            set (t);
        }
    }

    juce::var object;
    juce::String key;
    juce::var entry;
    std::function<void (bool)> changed;
    bool rebuild;
    juce::String lastCommitted;

    juce::Label name, note;
    juce::TextButton reset;
    MenuSlider slider;
    juce::ComboBox combo;
    juce::ToggleButton toggle;
    juce::TextEditor text;
    juce::Component* control = nullptr;
};

// A list of filter bands (eq bands, pre, post, a feedback path): one line of controls per band.
class BandsRow final : public Inspector::Row
{
public:
    std::function<void (const juce::String& setting)> putOnPanel;  // gets "bands.0.gain_db" and the like

    BandsRow (juce::var objectIn, const juce::String& keyIn, const juce::var& entry, const juce::var& bandDocIn,
              std::function<void (bool)> changedIn)
        : object (objectIn), key (keyIn), bandDoc (bandDocIn), changed (std::move (changedIn))
    {
        const auto tipText = SettingsHelp::tip (entry) + "\n\nName in the preset file: " + key;
        name.setText (entry["label"].toString(), juce::dontSendNotification);
        name.setTooltip (tipText);
        styleName (name, object.hasProperty (key));
        addAndMakeVisible (name);

        note.setText (entry["short"].toString(), juce::dontSendNotification);
        note.setTooltip (tipText);
        styleNote (note);
        addAndMakeVisible (note);

        add.setButtonText ("Add band");
        add.setTooltip (SettingsHelp::tip (bandDoc));
        add.onClick = [this]
        {
            auto* obj = object.getDynamicObject();

            if (obj == nullptr)
                return;

            if (! object[key.toRawUTF8()].isArray())
                obj->setProperty (key, juce::Array<juce::var>());

            object[key.toRawUTF8()].getArray()->add (parseJson (R"({ "kind": "peak", "freq": 1000, "gain_db": 0, "q": 0.7 })"));
            changed (true);
        };
        addAndMakeVisible (add);

        const auto list = object[key.toRawUTF8()];

        if (list.isArray())
            for (int i = 0; i < list.size(); ++i)
                bands.push_back (std::make_unique<Band> (*this, list[i], i));
    }

    int heightFor (int) const override { return lineHeight + 18 + (int) bands.size() * (lineHeight + 4) + lineHeight + 4; }

    void resized() override
    {
        auto area = getLocalBounds();
        name.setBounds (area.removeFromTop (lineHeight));
        note.setBounds (area.removeFromTop (18).withTrimmedLeft (4));

        for (auto& b : bands)
        {
            b->layout (area.removeFromTop (lineHeight).withTrimmedLeft (12));
            area.removeFromTop (4);
        }

        add.setBounds (area.removeFromTop (lineHeight).withWidth (90).withX (12));
    }

private:
    // One band: kind, frequency, gain (shelves and peaks only), q, remove.
    struct Band
    {
        Band (BandsRow& owner, juce::var bandIn, int indexIn) : row (owner), band (bandIn), index (indexIn)
        {
            const auto settings = row.bandDoc["settings"];
            int id = 1;

            if (auto* choices = settings["kind"]["choices"].getDynamicObject())
                for (auto& c : choices->getProperties())
                {
                    kind.addItem (c.name.toString(), id);

                    if (c.name.toString() == band["kind"].toString())
                        kind.setSelectedId (id, juce::dontSendNotification);

                    ++id;
                }

            kind.setTooltip (SettingsHelp::tip (settings["kind"]));
            kind.onChange = [this]
            {
                // A 1st-order filter (one capacitor, one resistor) has no q or gain: they would be an error.
                if (isFirstOrder (kind.getText()))
                    if (auto* obj = band.getDynamicObject())
                    {
                        obj->removeProperty ("q");
                        obj->removeProperty ("gain_db");
                    }

                setValue ("kind", kind.getText(), true);
            };
            row.addAndMakeVisible (kind);

            const auto kindText = band["kind"].toString();
            hasGain = kindText == "peak" || kindText.endsWith ("shelf");
            hasQ = ! isFirstOrder (kindText);

            for (auto [slider, setting, fallback] : { std::tuple { &freq, "freq", 1000.0 }, std::tuple { &gain, "gain_db", 0.0 },
                                                      std::tuple { &q, "q", 0.707 } })
            {
                const auto entry = settings[setting];
                const auto value = band[setting];
                setupSlider (*slider, entry, value.isVoid() ? fallback : (double) value);
                slider->setSliderStyle (juce::Slider::LinearBar);  // the value is drawn inside the bar: fits a narrow space
                slider->setColour (juce::Slider::trackColourId, theme::chipSelected);
                slider->setTooltip (SettingsHelp::tip (entry));
                slider->onValueChange = [this, slider, key = juce::String (setting)] { setValue (key, tidyNumber (slider->getValue()), false); };
                slider->onMenu = [this, slider, key = juce::String (setting)]
                {
                    const auto path = row.key + "." + juce::String (index) + "." + key;
                    offerPanelMenu (slider, row.putOnPanel == nullptr ? std::function<void()>() : [this, path] { row.putOnPanel (path); });
                };

                if ((slider != &gain || hasGain) && (slider != &q || hasQ))
                    row.addAndMakeVisible (*slider);
            }

            remove.setButtonText ("x");
            remove.setTooltip ("Remove this band.");
            remove.onClick = [this]
            {
                if (auto* list = row.object[row.key.toRawUTF8()].getArray())
                    list->remove (index);

                row.changed (true);
            };
            row.addAndMakeVisible (remove);
        }

        void layout (juce::Rectangle<int> area)
        {
            remove.setBounds (area.removeFromRight (24).reduced (2));
            kind.setBounds (area.removeFromLeft (92));
            area.removeFromLeft (4);
            const int w = area.getWidth() / 3;
            freq.setBounds (area.removeFromLeft (w).reduced (2, 1));
            gain.setBounds (area.removeFromLeft (w).reduced (2, 1));
            q.setBounds (area.reduced (2, 1));
        }

        void setValue (const juce::String& setting, const juce::var& value, bool rebuild)
        {
            if (auto* obj = band.getDynamicObject())
                obj->setProperty (setting, value);

            row.changed (rebuild);
        }

        static bool isFirstOrder (const juce::String& kind) { return kind == "highpass1" || kind == "lowpass1"; }

        BandsRow& row;
        juce::var band;
        int index;
        bool hasGain = false, hasQ = true;
        juce::ComboBox kind;
        MenuSlider freq, gain, q;
        juce::TextButton remove;
    };

    juce::var object;
    juce::String key;
    juce::var bandDoc;
    std::function<void (bool)> changed;
    juce::Label name, note;
    juce::TextButton add;
    std::vector<std::unique_ptr<Band>> bands;
};

// Tone stack part values, typed like on a schematic: 470p, 22n, 33k, 1M.
class PartsRow final : public Inspector::Row
{
public:
    // partNames: the parts of the block's circuit, with their help text.
    PartsRow (juce::var objectIn, const juce::var& entry, const juce::var& partNames, std::function<void (bool)> changedIn)
        : object (objectIn), changed (std::move (changedIn))
    {
        const auto tipText = SettingsHelp::tip (entry);
        name.setText (entry["label"].toString(), juce::dontSendNotification);
        name.setTooltip (tipText);
        styleName (name, object.hasProperty ("parts"));
        addAndMakeVisible (name);

        note.setText ("Empty = the style's value. Type values like 470p, 22n, 33k or 1M.", juce::dontSendNotification);
        styleNote (note);
        addAndMakeVisible (note);

        if (auto* names = partNames.getDynamicObject())
            for (auto& n : names->getProperties())
            {
                auto part = std::make_unique<Part>();
                part->name = n.name.toString();
                part->label.setText (part->name, juce::dontSendNotification);
                part->label.setTooltip (n.value.toString());
                part->text.setTooltip (n.value.toString());
                part->text.setTextToShowWhenEmpty ("style value", theme::muted);

                const auto value = object["parts"][part->name.toRawUTF8()];

                if (! value.isVoid())
                    part->text.setText (formatEngineering ((double) value), false);

                auto* p = part.get();
                p->text.onReturnKey = [this, p] { commit (*p); };
                p->text.onFocusLost = [this, p] { commit (*p); };
                addAndMakeVisible (p->label);
                addAndMakeVisible (p->text);
                parts.push_back (std::move (part));
            }
    }

    int heightFor (int) const override { return lineHeight + 18 + ((int) parts.size() + 1) / 2 * (lineHeight + 4); }

    void resized() override
    {
        auto area = getLocalBounds();
        name.setBounds (area.removeFromTop (lineHeight));
        note.setBounds (area.removeFromTop (18).withTrimmedLeft (4));
        const int half = area.getWidth() / 2;

        for (size_t i = 0; i < parts.size(); ++i)
        {
            auto cell = juce::Rectangle<int> (12 + (int) (i % 2) * half, area.getY() + (int) (i / 2) * (lineHeight + 4),
                                              half - 16, lineHeight);
            parts[i]->label.setBounds (cell.removeFromLeft (34));
            parts[i]->text.setBounds (cell);
        }
    }

private:
    struct Part
    {
        juce::String name;
        juce::Label label;
        juce::TextEditor text;
    };

    void commit (Part& part)
    {
        auto* obj = object.getDynamicObject();

        if (obj == nullptr)
            return;

        if (! object["parts"].isObject())
            obj->setProperty ("parts", juce::var (new juce::DynamicObject()));

        auto* partsObj = object["parts"].getDynamicObject();
        const double value = parseEngineering (part.text.getText());
        const bool had = partsObj->hasProperty (part.name);

        if (value > 0.0)
        {
            partsObj->setProperty (part.name, tidyNumber (value));
            part.text.setText (formatEngineering (value), false);
        }
        else
        {
            partsObj->removeProperty (part.name);
            part.text.clear();

            if (! had)
                return;
        }

        if (partsObj->getProperties().isEmpty())
            obj->removeProperty ("parts");

        changed (false);
    }

    juce::var object;
    std::function<void (bool)> changed;
    juce::Label name, note;
    std::vector<std::unique_ptr<Part>> parts;
};
}

//==============================================================================
Inspector::Inspector (const SettingsHelp& h) : help (h)
{
    title.setFont (juce::FontOptions (19.0f, juce::Font::bold));
    title.setColour (juce::Label::textColourId, theme::accent);
    addAndMakeVisible (title);

    summary.setColour (juce::Label::textColourId, theme::muted);
    summary.setFont (juce::FontOptions (13.0f));
    summary.setJustificationType (juce::Justification::topLeft);
    addAndMakeVisible (summary);
}

Inspector::~Inspector() = default;

void Inspector::changed (bool rebuild)
{
    if (onChanged != nullptr)
        onChanged (rebuild);
}

juce::TextButton* Inspector::addButton (const juce::String& text, const juce::String& tip, std::function<void()> action)
{
    auto button = std::make_unique<juce::TextButton> (text);
    button->setTooltip (tip);
    button->onClick = std::move (action);
    addAndMakeVisible (*button);
    buttons.push_back (std::move (button));
    return buttons.back().get();
}

// "Add inside" of a feedback loop or a split path: adds at the end of the item's "chain" list.
void Inspector::addInsideButton (const juce::var& item, bool loopOnly)
{
    auto* button = addButton ("Add inside...", loopOnly ? "Add a block at the end of the loop." : "Add a block at the end of this path.", {});
    button->onClick = [this, item, button, loopOnly]
    {
        if (! item["chain"].isArray())
            if (auto* obj = item.getDynamicObject())
                obj->setProperty ("chain", juce::Array<juce::var>());

        showAddMenu (button, item["chain"], item["chain"].size(), loopOnly);
    };
}

void Inspector::moveBy (int offset)
{
    auto* list = selection.list.getArray();
    const int to = selection.index + offset;

    if (list == nullptr || ! juce::isPositiveAndBelow (to, list->size()))
        return;

    list->swap (selection.index, to);
    auto moved = selection;
    moved.index = to;
    changed (true);

    if (onSelect != nullptr)
        onSelect (moved);
}

void Inspector::removeSelected()
{
    if (auto* list = selection.list.getArray())
        list->remove (selection.index);

    changed (true);

    if (onSelect != nullptr)
        onSelect ({});
}

// The last copied block. Windows lets one program at a time open the clipboard, so a copy can fail when
// another program is reading it just then: copying tries again, and pasting falls back to this.
static juce::String& lastCopy()
{
    static juce::String text;
    return text;
}

bool Inspector::copySelected()
{
    if (! selection.isValid())
        return false;

    const auto text = juce::JSON::toString (selection.item());
    lastCopy() = text;

    // Other programs (like the clipboard history) read every new copy at once, so wait a moment before checking.
    juce::SystemClipboard::copyTextToClipboard (text);

    for (int attempt = 0; attempt < 5; ++attempt)
    {
        juce::Thread::sleep (20);

        if (juce::SystemClipboard::getTextFromClipboard() == text)
            break;

        juce::SystemClipboard::copyTextToClipboard (text);
    }

    return true;
}

bool Inspector::cutSelected()
{
    if (! copySelected())
        return false;

    removeSelected();
    return true;
}

bool Inspector::pasteAfterSelected()
{
    // The system clipboard if it holds a block (it may come from another plugin or a text editor),
    // otherwise the last block copied here.
    auto text = juce::SystemClipboard::getTextFromClipboard();
    juce::var parsed;

    if (juce::JSON::parse (text, parsed).failed() || ! parsed.isObject())
        text = lastCopy();

    return insertAfterSelected (text);
}

bool Inspector::duplicateSelected()
{
    return selection.isValid() && insertAfterSelected (juce::JSON::toString (selection.item()));
}

// Without a selection: at the end of the main chain.
bool Inspector::insertAfterSelected (const juce::String& text)
{
    const bool hasSelection = selection.isValid(), isPath = hasSelection && selection.isPath;

    if (! hasSelection && ! preset["chain"].isArray())
        if (auto* obj = preset.getDynamicObject())
            obj->setProperty ("chain", juce::Array<juce::var>());

    auto list = hasSelection ? selection.list : preset["chain"];

    if (! list.isArray())
        return false;

    juce::String problem;
    const auto item = makePastedItem (text, preset, isPath, problem);

    if (item.isVoid())
    {
        showProblem (problem);
        return false;
    }

    if (! isPath && isLoopChain (preset, list.getArray()) && ! canGoInLoop (item["type"].toString()))
    {
        showProblem ("Only eq, tonestack, gain and power blocks can go inside a feedback loop.");
        return false;
    }

    const int at = hasSelection ? selection.index + 1 : endInsertIndex (list, item);
    list.getArray()->insert (at, item);
    changed (true);

    if (onSelect != nullptr)
        onSelect ({ list, at, isPath });

    return true;
}

void Inspector::showProblem (const juce::String& text)
{
    summary.setColour (juce::Label::textColourId, theme::error);
    summary.setText (text, juce::dontSendNotification);
}

void Inspector::showAddMenu (juce::Component* button, juce::var list, int insertAt, bool loopOnly)
{
    juce::PopupMenu menu;
    const auto types = help.blockTypes();

    for (int i = 0; i < types.size(); ++i)
        if (! loopOnly || canGoInLoop (types[i]))
            menu.addItem (i + 1, types[i] + "  -  " + help.blockInfo (types[i])["short"].toString());

    menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (button),
                        [this, types, list, insertAt] (int result)
                        {
                            auto* array = list.getArray();

                            if (result <= 0 || array == nullptr)
                                return;

                            const int at = juce::jlimit (0, array->size(), insertAt);
                            array->insert (at, makeBlock (types[result - 1]));
                            changed (true);

                            if (onSelect != nullptr)
                                onSelect ({ list, at, false });
                        });
}

void Inspector::show (const Selection& newSelection, const juce::var& presetIn)
{
    selection = newSelection;
    preset = presetIn;
    rows.clear();
    buttons.clear();
    summary.setColour (juce::Label::textColourId, theme::muted);

    auto changedFn = [this] (bool rebuild) { changed (rebuild); };

    if (! selection.isValid())
    {
        title.setText ("Nothing selected", juce::dontSendNotification);
        summary.setText ("Click a block in the chain to change its settings. Every change is heard right away; "
                         "the output level stays where it is (use 'Level' to measure it again).",
                         juce::dontSendNotification);

        if (preset.isObject())
            addButton ("Paste at the end", "Add the copied block at the end of the chain (Ctrl+V), before the cab IR "
                                          "(only eq and ir blocks can come after it).", [this] { pasteAfterSelected(); });

        return;
    }

    const auto item = selection.item();
    addButton ("Up", "Move it one place up.", [this] { moveBy (-1); });
    addButton ("Down", "Move it one place down.", [this] { moveBy (1); });
    addButton ("Remove", "Take it out of the preset (Delete; Ctrl+X cuts it: removes it and keeps a copy to paste).",
               [this] { removeSelected(); });
    addButton ("Copy", "Copy it (Ctrl+C), with everything inside it, to paste somewhere else in this preset or in "
                       "another one. The copy is JSON text, so it can also be pasted into a preset file.",
               [this] { copySelected(); });
    addButton ("Paste after", "Add the copied block (or split path) right after this one (Ctrl+V). "
                              "Ids that are taken get a number (v2 becomes v2_2).",
               [this] { pasteAfterSelected(); });
    addButton ("Duplicate", "Add a copy of it right after it (Ctrl+D).", [this] { duplicateSelected(); });

    if (selection.isPath)
    {
        title.setText ("Split path " + juce::String (selection.index + 1), juce::dontSendNotification);
        summary.setText (help.section ("split_path")["short"].toString(), juce::dontSendNotification);
        summary.setTooltip (help.sectionTip ("split_path"));

        addInsideButton (item, false);

        for (auto* key : { "level_db", "invert" })
        {
            const auto entry = help.section ("split_path")["settings"][key];
            rows.push_back (std::make_unique<ValueRow> (item, key, entry, entry["short"].toString(), changedFn, false));
        }
    }
    else
    {
        const auto type = item.getProperty ("type", "").toString();
        const auto id = item.getProperty ("id", "").toString();
        title.setText (type + (id.isNotEmpty() ? "  " + id : juce::String()), juce::dontSendNotification);
        summary.setText (help.blockInfo (type)["short"].toString(), juce::dontSendNotification);
        summary.setTooltip (help.blockTip (type));

        const int index = selection.index;
        const auto list = selection.list;
        const bool inLoop = isLoopChain (preset, list.getArray());
        auto* addAfter = addButton ("Add after...", inLoop ? "Add a new block right after this one (inside a feedback loop "
                                                             "only eq, tonestack, gain and power can go)."
                                                           : "Add a new block right after this one.", {});
        addAfter->onClick = [this, list, index, addAfter, inLoop] { showAddMenu (addAfter, list, index + 1, inLoop); };

        if (type == "feedback")
            addInsideButton (item, true);

        if (type == "split")
        {
            addButton ("Add path", "Add another parallel path (empty = the clean input).", [this, item]
            {
                if (! item["paths"].isArray())
                    if (auto* obj = item.getDynamicObject())
                        obj->setProperty ("paths", juce::Array<juce::var>());

                item["paths"].getArray()->add (parseJson (R"({ "chain": [] })"));
                changed (true);
            });
        }

        addRowsForBlock (item);
    }

    for (auto& row : rows)
        addAndMakeVisible (*row);
}

void Inspector::addRowsForBlock (const juce::var& block)
{
    auto changedFn = [this] (bool rebuild) { changed (rebuild); };
    const auto type = block.getProperty ("type", "").toString();
    const auto settings = help.blockSettings (type);
    auto* obj = block.getDynamicObject();

    if (obj == nullptr || ! settings.isObject())
        return;

    const auto curve = block.getProperty ("curve", "soft").toString();
    const auto idEntry = help.section ("common")["settings"]["id"];
    rows.push_back (std::make_unique<ValueRow> (block, "id", idEntry, idEntry["short"].toString(), changedFn, false));

    for (auto& p : settings.getDynamicObject()->getProperties())
    {
        const auto key = p.name.toString();
        const auto entry = p.value;
        const auto kind = entry["type"].toString();

        if (kind == "block-list" || kind == "path-list")
            continue;  // edited by selecting the blocks inside

        if (entry.hasProperty ("curve") && entry["curve"].toString() != curve)
            continue;  // a setting of another clipping curve

        if (auto* styles = entry["styles"].getArray(); styles != nullptr && ! styles->contains (block.getProperty ("style", "british")))
            continue;  // a knob this tone stack style does not have

        if (kind == "band-list")
        {
            auto bandsRow = std::make_unique<BandsRow> (block, key, entry, help.section ("band"), changedFn);
            bandsRow->putOnPanel = [this, block] (const juce::String& setting) { if (onPutOnPanel != nullptr) onPutOnPanel (block, setting); };
            rows.push_back (std::move (bandsRow));
            continue;
        }

        if (kind == "parts")
        {
            // Each circuit has its own parts.
            const auto circuit = settings["style"]["circuits"][block.getProperty ("style", "british").toString().toRawUTF8()];
            rows.push_back (std::make_unique<PartsRow> (block, entry, entry["names"][circuit.toString().toRawUTF8()], changedFn));
            continue;
        }

        juce::String note = entry["short"].toString();

        if (key == "oversample")
            note = "Does nothing: the Quality menu oversamples the whole amp. Kept so older presets still load.";

        const bool isCurve = key == "curve";
        const bool isStyle = key == "style" && type == "tonestack";
        auto row = std::make_unique<ValueRow> (block, key, entry, note, isCurve ? std::function<void (bool)> ([this, block] (bool)
        {
            // Settings of the old curve would stop the preset with an error, so they go.
            const auto newCurve = block.getProperty ("curve", "soft").toString();

            if (auto* o = block.getDynamicObject())
                for (auto& s : help.blockSettings ("gain").getDynamicObject()->getProperties())
                    if (s.value.hasProperty ("curve") && s.value["curve"].toString() != newCurve)
                        o->removeProperty (s.name);

            changed (true);
        }) : isStyle ? std::function<void (bool)> ([this, block, settings] (bool)
        {
            // Knobs and parts the new style does not have would stop the preset with an error, so they go.
            const auto newStyle = block.getProperty ("style", "british");
            auto* o = block.getDynamicObject();

            if (o == nullptr)
                return;

            for (auto& s : settings.getDynamicObject()->getProperties())
                if (auto* styles = s.value["styles"].getArray(); styles != nullptr && ! styles->contains (newStyle))
                    o->removeProperty (s.name);

            const auto names = settings["parts"]["names"][settings["style"]["circuits"][newStyle.toString().toRawUTF8()].toString().toRawUTF8()];

            if (auto* parts = block["parts"].getDynamicObject())
            {
                juce::StringArray gone;

                for (auto& p : parts->getProperties())
                    if (! names.hasProperty (p.name))
                        gone.add (p.name.toString());

                for (auto& name : gone)
                    parts->removeProperty (name);
            }

            changed (true);
        }) : changedFn, false);

        if (kind == "number")
            row->putOnPanel = [this, block, key] { if (onPutOnPanel != nullptr) onPutOnPanel (block, key); };

        rows.push_back (std::move (row));
    }

    // Settings the plugin does not know (a typo in a preset file): shown so they can be removed.
    for (auto& p : obj->getProperties())
    {
        const auto key = p.name.toString();

        if (key != "type" && key != "id" && ! settings.hasProperty (key))
            rows.push_back (std::make_unique<ValueRow> (block, key, juce::var(),
                                                        "Not a setting of this block (a typo?). Remove it with x.", changedFn, false));
    }
}

void Inspector::layout (int width)
{
    int y = 0;
    title.setBounds (0, y, width, 26);
    y += 28;
    summary.setBounds (0, y, width, 34);
    y += 38;

    int x = 0;

    for (auto& b : buttons)
    {
        const int w = juce::GlyphArrangement::getStringWidthInt (juce::FontOptions (14.0f), b->getButtonText()) + 22;

        if (x + w > width && x > 0)
        {
            x = 0;
            y += 30;
        }

        b->setBounds (x, y, w, 26);
        x += w + 6;
    }

    y += buttons.empty() ? 0 : 36;

    for (auto& row : rows)
    {
        const int h = row->heightFor (width);
        row->setBounds (0, y, width, h);
        y += h + 8;
    }

    setSize (width, y + 8);
}
