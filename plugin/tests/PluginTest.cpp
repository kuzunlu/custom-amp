// Drives the plugin like a DAW does, without a DAW: play audio, switch presets while playing, load a
// broken preset file and fix it, change the quality, save and restore the state, open the editor.
//
//   PluginTest

#include "../Source/PluginEditor.h"
#include "../Source/PluginProcessor.h"

#include <iostream>

namespace
{
int failures = 0;

void check (bool ok, const juce::String& what)
{
    std::cout << (ok ? "  ok    " : "  FAIL  ") << what << "\n";

    if (! ok)
        ++failures;
}

constexpr double rate = 48000.0;
constexpr int block = 256;

// Plays `seconds` of a plucked-like test riff through the plugin (message loop running in between,
// so the plugin's timer can swap engines). Returns the output's RMS in dBFS, or +inf if it was not finite.
double play (CustomAmpProcessor& p, double seconds)
{
    const auto riff = amp::makeLevelRiff (rate);
    juce::AudioBuffer<float> buffer (2, block);
    juce::MidiBuffer midi;
    double sumSquares = 0.0;
    bool finite = true;
    const int blocks = (int) (seconds * rate / block);

    for (int b = 0; b < blocks; ++b)
    {
        for (int i = 0; i < block; ++i)
        {
            const auto v = (float) riff[(size_t) ((b * block + i) % (int) riff.size())];
            buffer.setSample (0, i, v);
            buffer.setSample (1, i, v);
        }

        p.processBlock (buffer, midi);

        for (int i = 0; i < block; ++i)
        {
            const float v = buffer.getSample (0, i);
            finite = finite && std::isfinite (v);
            sumSquares += (double) v * v;
        }

        if (b % 8 == 0)
            juce::MessageManager::getInstance()->runDispatchLoopUntil (1);
    }

    return finite ? juce::Decibels::gainToDecibels (std::sqrt (sumSquares / (blocks * block)), -200.0)
                  : std::numeric_limits<double>::infinity();
}

// Lets the background builder finish and the timer hand over the result.
void waitForBuild (CustomAmpProcessor& p)
{
    for (int i = 0; i < 40; ++i)
    {
        juce::MessageManager::getInstance()->runDispatchLoopUntil (50);
        play (p, 0.02);
    }
}

std::unique_ptr<CustomAmpProcessor> makePlugin()
{
    auto p = std::make_unique<CustomAmpProcessor>();
    p->setPlayConfigDetails (2, 2, rate, block);
    p->prepareToPlay (rate, block);
    return p;
}

// Finds a control of the editor by its component id (the setting's name in the preset).
template <typename Type>
Type* findControl (juce::Component& root, const juce::String& id)
{
    for (auto* child : root.getChildren())
    {
        if (child->getComponentID() == id)
            if (auto* found = dynamic_cast<Type*> (child))
                return found;

        if (auto* found = findControl<Type> (*child, id))
            return found;
    }

    return nullptr;
}

// Every block and split path in a list (and inside them), as selections.
void collect (const juce::var& list, std::vector<Selection>& out)
{
    for (int i = 0; list.isArray() && i < list.size(); ++i)
    {
        out.push_back ({ list, i, false });
        const auto item = list[i];
        collect (item["chain"], out);

        for (int k = 0; item["paths"].isArray() && k < item["paths"].size(); ++k)
        {
            out.push_back ({ item["paths"], k, true });
            collect (item["paths"][k]["chain"], out);
        }
    }
}

void pump (int ms = 20)
{
    juce::MessageManager::getInstance()->runDispatchLoopUntil (ms);
}

// Plays a plucked note (A2, dying away over a few seconds) and returns the output. `during` is called after
// `editAt` seconds. The note is played in real time, so the background builder takes as long as in a DAW.
std::vector<float> playNote (CustomAmpProcessor& p, double seconds, double editAt, std::function<void()> during)
{
    juce::AudioBuffer<float> buffer (2, block);
    juce::MidiBuffer midi;
    const int blocks = (int) (seconds * rate / block), editBlock = (int) (editAt * rate / block);
    std::vector<float> out;
    long long n = 0;
    const auto start = juce::Time::getMillisecondCounterHiRes();

    for (int b = 0; b < blocks; ++b)
    {
        for (int i = 0; i < block; ++i, ++n)
        {
            const double t = (double) n / rate;
            double v = 0.0;

            for (int k = 1; k <= 8; ++k)
                v += std::sin (juce::MathConstants<double>::twoPi * 110.0 * k * t) / k * std::exp (-t * (0.6 + 0.4 * k));

            buffer.setSample (0, i, (float) (0.35 * v));
            buffer.setSample (1, i, (float) (0.35 * v));
        }

        if (b == editBlock && during != nullptr)
            during();

        p.processBlock (buffer, midi);

        for (int i = 0; i < block; ++i)
            out.push_back (buffer.getSample (0, i));

        // Wait until this block's real time has passed, letting the message thread run meanwhile.
        const double due = start + 1000.0 * (b + 1) * block / rate;

        while (juce::Time::getMillisecondCounterHiRes() < due)
            pump (1);
    }

    return out;
}
}

// Saves pictures of the plugin window (for the docs): a working preset, and a preset file with a typo.
int snapshots (const juce::File& folder)
{
    auto p = makePlugin();

    auto save = [&] (const juce::String& name)
    {
        std::unique_ptr<juce::AudioProcessorEditor> editor (p->createEditor());
        editor->setSize (860, 640);
        auto image = editor->createComponentSnapshot (editor->getLocalBounds(), true, 2.0f);
        auto file = folder.getChildFile (name);
        file.deleteFile();
        juce::FileOutputStream out (file);
        juce::PNGImageFormat().writeImageToStream (image, out);
        std::cout << "wrote " << file.getFullPathName() << "\n";
    };

    p->loadBuiltInPreset ("brit_800.json");
    waitForBuild (*p);
    save ("plugin_window.png");

    // The same amp with a block selected, so the inspector shows its settings.
    for (auto [index, name] : { std::pair { 4, "plugin_inspector_gain.png" }, std::pair { 6, "plugin_inspector_tonestack.png" },
                                std::pair { 1, "plugin_inspector_eq.png" } })
    {
        std::unique_ptr<juce::AudioProcessorEditor> editor (p->createEditor());
        auto* ed = dynamic_cast<CustomAmpEditor*> (editor.get());
        editor->setSize (1120, 820);
        ed->select ({ ed->getEditedPreset()["chain"], index, false });
        pump (100);
        auto image = editor->createComponentSnapshot (editor->getLocalBounds(), true, 2.0f);
        auto file = folder.getChildFile (name);
        file.deleteFile();
        juce::FileOutputStream out (file);
        juce::PNGImageFormat().writeImageToStream (image, out);
        std::cout << "wrote " << file.getFullPathName() << "\n";
    }

    auto file = folder.getChildFile ("my_amp.json");
    file.replaceWithText (R"({ "name": "My amp", "chain": [ { "type": "gain", "drive_db": 12, "curv": "tube" } ] })");
    p->loadPresetFile (file);
    waitForBuild (*p);
    save ("plugin_error.png");
    return 0;
}

int main (int argc, char* argv[])
{
    juce::ScopedJuceInitialiser_GUI gui;

    if (argc == 3 && juce::String (argv[1]) == "--snapshot")
        return snapshots (juce::File (argv[2]));

    auto p = makePlugin();

    std::cout << "Default preset\n";
    check (! p->getStatus().isError, "builds: " + p->getStatus().text.upToFirstOccurrenceOf ("\n", false, false));
    check (p->getLatencySamples() > 0 && p->getLatencySamples() < 20, "latency " + juce::String (p->getLatencySamples()) + " samples");
    auto level = play (*p, 2.0);
    check (std::abs (level + 20.0) < 3.0, "the level riff comes out near -20 dBFS: " + juce::String (level, 1));

    std::cout << "Every built-in preset, switched while playing\n";

    for (auto& preset : CustomAmpProcessor::getBuiltInPresets())
    {
        p->loadBuiltInPreset (preset.fileName);
        waitForBuild (*p);
        level = play (*p, 1.0);
        check (! p->getStatus().isError && std::abs (level + 20.0) < 3.0,
               preset.fileName + ": " + juce::String (level, 1) + " dBFS"
                   + (p->getStatus().isError ? "  " + p->getStatus().text : juce::String()));
    }

    std::cout << "A preset file with a typo, then fixed while the plugin watches it\n";
    auto file = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("custom_amp_test.json");
    file.replaceWithText (R"({ "name": "Test", "chain": [ { "type": "gain", "drive_db": 12, "curv": "tube" } ] })");
    p->loadPresetFile (file);
    waitForBuild (*p);
    check (p->getStatus().isError && p->getStatus().text.contains ("curv"), "the error names the typo: "
           + p->getStatus().text.upToFirstOccurrenceOf ("\n", false, false));
    level = play (*p, 0.5);
    check (std::isfinite (level) && level > -60.0, "the last working preset keeps playing: " + juce::String (level, 1) + " dBFS");

    juce::Thread::sleep (1100);  // file times have a 1 s resolution on some disks
    file.replaceWithText (R"({ "name": "Test fixed", "chain": [ { "type": "gain", "drive_db": 12, "curve": "tube" } ] })");
    waitForBuild (*p);
    waitForBuild (*p);
    check (! p->getStatus().isError && p->getStatus().text.startsWith ("Test fixed"), "reloaded by itself after the fix");

    std::cout << "Quality\n";
    const int latency8 = p->getLatencySamples();
    p->parameters.getParameter ("quality")->setValueNotifyingHost (0.0f);  // 2x
    waitForBuild (*p);
    check (p->getStatus().text.contains ("96.0 kHz"), "2x runs at 96 kHz");
    check (p->getLatencySamples() != latency8, "latency changes with it: " + juce::String (latency8) + " -> "
           + juce::String (p->getLatencySamples()) + " samples");

    std::cout << "Save and restore\n";
    p->loadBuiltInPreset ("us_blackface_clean.json");
    p->parameters.getParameter ("output_db")->setValueNotifyingHost (0.25f);
    waitForBuild (*p);
    juce::MemoryBlock state;
    p->getStateInformation (state);
    auto q = makePlugin();
    q->setStateInformation (state.getData(), (int) state.getSize());
    waitForBuild (*q);
    check (q->getPresetSource() == "builtin:us_blackface_clean.json", "preset: " + q->getPresetSource());
    check (std::abs (q->parameters.getParameter ("output_db")->getValue() - 0.25f) < 1e-6f, "output knob");
    check (q->getStatus().text.contains ("96.0 kHz"), "quality (2x)");

    std::cout << "Editor\n";
    {
        std::unique_ptr<juce::AudioProcessorEditor> editor (q->createEditor());
        editor->setSize (800, 500);
        editor->setSize (1200, 900);
        check (editor->getWidth() == 1200, "opens and resizes");
    }

    std::cout << "Editing in the window\n";
    {
        int selected = 0;

        for (auto& preset : CustomAmpProcessor::getBuiltInPresets())
        {
            q->loadBuiltInPreset (preset.fileName);
            waitForBuild (*q);
            std::unique_ptr<juce::AudioProcessorEditor> editor (q->createEditor());
            auto* ed = dynamic_cast<CustomAmpEditor*> (editor.get());
            std::vector<Selection> all;
            collect (ed->getEditedPreset()["chain"], all);

            for (auto& s : all)
            {
                ed->select (s);
                pump (5);
                ++selected;
            }
        }

        check (selected > 100, "every block of every built-in preset opens in the inspector (" + juce::String (selected) + " items)");

        q->loadBuiltInPreset ("brit_800.json");
        waitForBuild (*q);
        const auto levelLine = [&q] { return q->getStatus().text.fromFirstOccurrenceOf ("latency", false, false).upToFirstOccurrenceOf ("\n", false, false); };
        const auto before = levelLine();
        std::unique_ptr<juce::AudioProcessorEditor> editor (q->createEditor());
        auto* ed = dynamic_cast<CustomAmpEditor*> (editor.get());
        ed->select ({ ed->getEditedPreset()["chain"], 6, false });  // the tone stack
        pump (50);
        auto* treble = findControl<juce::Slider> (*editor, "treble");
        check (treble != nullptr, "the tone stack's treble slider is there");

        if (treble != nullptr)
        {
            treble->setValue (9.0, juce::sendNotificationSync);
            waitForBuild (*q);
            check (q->isEdited() && q->getPresetText().contains ("\"treble\": 9"), "moving it changes the preset");
            check (! q->getStatus().isError && q->getStatus().text.contains ("level kept"), "rebuilt with the level kept: " + levelLine()
                   + " (was: " + before + ")");
        }

        // Swap amps while a note rings: two plugins play the same note, one gets an edit you can not hear
        // (bass 5 -> 5.01) after 0.5 s. The new amp is warmed up and faded in, so the outputs stay close.
        {
            auto a = makePlugin(), b = makePlugin();

            for (auto* x : { a.get(), b.get() })
            {
                x->loadBuiltInPreset ("brit_800.json");
                waitForBuild (*x);
                playNote (*x, 1.0, -1.0, {});  // the same history in both
            }

            auto text = b->getPresetData();
            text["chain"][6].getDynamicObject()->setProperty ("bass", 5.01);
            const auto edited = juce::JSON::toString (text, true);
            const auto ref = playNote (*a, 2.0, -1.0, {});
            const auto out = playNote (*b, 2.0, 0.5, [&b, edited] { b->setEditedPreset (edited); });
            double peak = 0.0, worst = 0.0, worstBefore = 0.0;
            const size_t editSample = (size_t) (0.5 * rate);

            for (size_t i = 0; i < ref.size(); ++i)
            {
                peak = juce::jmax (peak, (double) std::abs (ref[i]));
                auto& w = i < editSample ? worstBefore : worst;
                w = juce::jmax (w, (double) std::abs (out[i] - ref[i]));
            }

            const double db = juce::Decibels::gainToDecibels (worst / peak, -200.0);
            check (db < -30.0, "no click when the amp is swapped under a ringing note: biggest difference "
                   + juce::String (db, 1) + " dB below the peak (before the edit: "
                   + juce::String (juce::Decibels::gainToDecibels (worstBefore / peak, -200.0), 1) + " dB)");
        }

        auto saved = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("custom_amp_saved.json");
        check (q->saveEditedPreset (saved) && ! q->isEdited() && q->getPresetFile() == saved, "Save as writes the file and watches it");
        juce::var reread;
        check (juce::JSON::parse (saved.loadFileAsString(), reread).wasOk() && reread["chain"][6]["treble"].toString() == "9",
               "the saved file has the change");
        saved.deleteFile();
    }

    std::cout << "Undo and redo\n";
    {
        // The level in the status: "auto level 12.3 dB" or "level kept at 12.3 dB".
        const auto levelOf = [&q]
        {
            return q->getStatus().text.fromFirstOccurrenceOf ("level ", false, false).upToFirstOccurrenceOf (" dB", false, false)
                       .fromLastOccurrenceOf (" ", false, false);
        };

        q->loadBuiltInPreset ("brit_800.json");
        waitForBuild (*q);
        const auto original = q->getPresetText();
        const auto originalLevel = levelOf();
        std::unique_ptr<juce::AudioProcessorEditor> editor (q->createEditor());
        auto* ed = dynamic_cast<CustomAmpEditor*> (editor.get());
        ed->select ({ ed->getEditedPreset()["chain"], 6, false });  // the tone stack
        pump (50);

        // A slider drag: many changes close together are one step.
        if (auto* treble = findControl<juce::Slider> (*editor, "treble"))
            for (double v = 5.5; v <= 8.0; v += 0.5)
            {
                treble->setValue (v, juce::sendNotificationSync);
                pump (30);
            }

        waitForBuild (*q);
        check (q->canUndo() && q->getPresetText().contains ("\"treble\": 8"), "a slider drag changes the preset");
        q->undo();
        waitForBuild (*q);
        check (q->getPresetText() == original && q->getPresetSource() == "builtin:brit_800.json" && ! q->isEdited(),
               "one undo takes back the whole drag");
        check (ed->getSelection().isValid() && ed->getSelection().index == 6, "the tone stack stays selected");
        auto* treble = findControl<juce::Slider> (*editor, "treble");
        check (treble != nullptr && std::abs (treble->getValue() - (double) ed->getEditedPreset()["chain"][6]["treble"]) < 1e-9,
               "its slider shows the old value");

        q->redo();
        waitForBuild (*q);
        check (q->isEdited() && q->getPresetText().contains ("\"treble\": 8"), "redo brings it back");

        q->loadBuiltInPreset ("us_blackface_clean.json");
        waitForBuild (*q);
        q->undo();
        waitForBuild (*q);
        check (q->getPresetText().contains ("\"treble\": 8") && q->isEdited(), "undo of a preset load brings back the edited amp");
        check (levelOf() == originalLevel, "with the level it had: " + levelOf() + " dB (was " + originalLevel + " dB)");

        // An edit after undo throws the redo steps away.
        check (q->canRedo(), "redo is possible after undo");
        ed->select ({ ed->getEditedPreset()["chain"], 6, false });
        pump (50);

        if (auto* bass = findControl<juce::Slider> (*editor, "bass"))
            bass->setValue (3.0, juce::sendNotificationSync);

        pump (50);
        check (! q->canRedo(), "a new edit clears redo");

        // A preset file that changed after a step: undoing to that step must not look like the file.
        auto watched = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("custom_amp_undo.json");
        watched.replaceWithText (R"({ "name": "Undo A", "chain": [ { "type": "gain", "drive_db": 6 } ] })");
        q->loadPresetFile (watched);
        waitForBuild (*q);
        juce::Thread::sleep (1100);
        watched.replaceWithText (R"({ "name": "Undo B", "chain": [ { "type": "gain", "drive_db": 9 } ] })");
        waitForBuild (*q);
        check (q->getStatus().text.startsWith ("Undo B"), "the changed file reloads by itself");
        q->undo();
        waitForBuild (*q);
        waitForBuild (*q);
        check (q->getStatus().text.startsWith ("Undo A") && q->isEdited(),
               "undo goes back to the old file text, marked as edited (the watcher leaves it alone)");
        watched.deleteFile();
    }

    std::cout << "Copy, cut, paste, duplicate\n";
    {
        const auto userClipboard = juce::SystemClipboard::getTextFromClipboard();  // put back at the end
        q->loadBuiltInPreset ("brit_800.json");
        waitForBuild (*q);
        std::unique_ptr<juce::AudioProcessorEditor> editor (q->createEditor());
        auto* ed = dynamic_cast<CustomAmpEditor*> (editor.get());
        const auto key = [ed] (int k) { return ed->keyPressed (juce::KeyPress (k, juce::ModifierKeys::commandModifier, 0)); };
        const auto chain = [ed] { return ed->getEditedPreset()["chain"]; };
        const auto selectAt = [ed, chain] (int index) { ed->select ({ chain(), index, false }); pump (50); };
        const auto firstLine = [&q] { return q->getStatus().text.upToFirstOccurrenceOf ("\n", false, false); };

        // The feedback loop (nfb: pi, pa). The copy's ids get a number; the sag before it still drives the first pa.
        selectAt (9);
        check (key ('d'), "Ctrl+D duplicates");
        waitForBuild (*q);
        const auto copy = chain()[10];
        check (chain().size() == 12 && copy["id"].toString() == "nfb_2" && copy["chain"][0]["id"].toString() == "pi_2"
                   && copy["chain"][1]["id"].toString() == "pa_2",
               "the copy's ids get a number: " + copy["id"].toString() + ", " + copy["chain"][0]["id"].toString() + ", "
                   + copy["chain"][1]["id"].toString());
        check (chain()[8]["targets"][0].toString() == "pa", "the sag still targets the first power block");
        check (! q->getStatus().isError, "the preset still builds: " + firstLine());
        check (ed->getSelection().index == 10, "the copy is selected");

        // Copy the tone stack, paste it after the first block.
        selectAt (6);
        check (key ('c') && juce::SystemClipboard::getTextFromClipboard().contains ("\"tonestack\""), "Ctrl+C copies it as JSON");
        selectAt (0);
        check (key ('v'), "Ctrl+V pastes");
        pump (50);
        check (chain().size() == 13 && juce::JSON::toString (chain()[1]) == juce::JSON::toString (chain()[7]),
               "the pasted block is the same as the copied one");
        check (ed->getSelection().index == 1, "the pasted block is selected");

        check (key ('x') && chain().size() == 12, "Ctrl+X takes it out");
        waitForBuild (*q);
        q->undo();
        waitForBuild (*q);
        check (chain().size() == 13 && chain()[1]["type"].toString() == "tonestack", "undo puts it back");

        // Nothing selected: paste at the end (an eq can go after the cab).
        int potIndex = -1;

        for (int i = 0; i < chain().size(); ++i)
            if (chain()[i]["id"].toString() == "gain_pot")
                potIndex = i;

        selectAt (potIndex);
        key ('c');
        ed->select ({});
        pump (50);
        check (key ('v') && chain()[chain().size() - 1]["id"].toString() == "gain_pot_2", "with nothing selected it goes at the end");
        waitForBuild (*q);
        check (! q->getStatus().isError, "and the preset builds: " + firstLine());

        // The id rules, on their own.
        juce::String problem;
        const auto preset = ed->getEditedPreset();
        auto split = makePastedItem (R"({ "type": "split", "paths": [ { "chain": [ { "type": "sag", "targets": [ "v1a", "pa" ] },
                                         { "type": "gain", "id": "v1a" } ] } ] })", preset, false, problem);
        const auto inner = split["paths"][0]["chain"];
        check (inner[1]["id"].toString() == "v1a_2" && inner[0]["targets"][0].toString() == "v1a_2"
                   && inner[0]["targets"][1].toString() == "pa",
               "a sag inside the copy follows its block's new name; a target outside the copy stays");
        check (makePastedItem (R"({ "type": "feedback", "id": "nfb_2" })", preset, false, problem)["id"].toString() == "nfb_3",
               "nfb_2 becomes nfb_3");
        check (makePastedItem (R"({ "type": "gain" })", preset, true, problem).isVoid() && problem.contains ("split paths"),
               "a block can not go between split paths");
        check (makePastedItem (R"({ "chain": [] })", preset, false, problem).isVoid(), "a split path can not go into a chain");
        check (makePastedItem ("hello", preset, false, problem).isVoid(), "text that is not a block is refused");

        juce::SystemClipboard::copyTextToClipboard (userClipboard);
    }

    file.deleteFile();
    q.reset();
    p.reset();
    std::cout << (failures == 0 ? "All passed\n" : juce::String (failures) + " failed\n");
    return failures == 0 ? 0 : 1;
}
