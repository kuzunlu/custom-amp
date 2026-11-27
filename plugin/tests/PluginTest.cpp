// Drives the plugin like a DAW does, without a DAW: play audio, switch presets while playing, load a
// broken preset file and fix it, change the quality, save and restore the state, open the editor.
//
//   PluginTest

#include "../Source/PluginEditor.h"
#include "../Source/PluginProcessor.h"

#include <iostream>
#include <map>

#if JUCE_WINDOWS
// Windows lets one program at a time open the clipboard (user32.dll, which JUCE links anyway).
extern "C" __declspec (dllimport) int __stdcall OpenClipboard (void* window);
extern "C" __declspec (dllimport) int __stdcall CloseClipboard();
#endif

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
        ed->showView (false);
        ed->select ({ ed->getEditedPreset()["chain"], index, false });
        pump (100);
        auto image = editor->createComponentSnapshot (editor->getLocalBounds(), true, 2.0f);
        auto file = folder.getChildFile (name);
        file.deleteFile();
        juce::FileOutputStream out (file);
        juce::PNGImageFormat().writeImageToStream (image, out);
        std::cout << "wrote " << file.getFullPathName() << "\n";
    }

    // The front panel of two more amps.
    for (auto [preset, name] : { std::pair { "us_hotrod_lead.json", "plugin_panel_hotrod.png" }, std::pair { "brit_top_boost.json", "plugin_panel_top_boost.png" } })
    {
        p->loadBuiltInPreset (preset);
        waitForBuild (*p);
        std::unique_ptr<juce::AudioProcessorEditor> editor (p->createEditor());
        dynamic_cast<CustomAmpEditor*> (editor.get())->showView (true);
        editor->setSize (1120, 720);
        pump (100);
        auto image = editor->createComponentSnapshot (editor->getLocalBounds(), true, 2.0f);
        auto out = folder.getChildFile (name);
        out.deleteFile();
        juce::FileOutputStream stream (out);
        juce::PNGImageFormat().writeImageToStream (image, stream);
        std::cout << "wrote " << out.getFullPathName() << "\n";
    }

    // The signal view: what every block of brit_800 does to a test signal, tall enough for every strip.
    p->loadBuiltInPreset ("brit_800.json");
    waitForBuild (*p);

    for (auto [signal, freq, peak, mode, name] :  // mode: 0 time, 1 frequency, 2 curve
         { std::tuple { SignalView::Signal::sine, 220.0, 0.1, 0, "signals_sine_soft_time.png" },
           std::tuple { SignalView::Signal::sine, 220.0, 0.5, 0, "signals_sine_hard_time.png" },
           std::tuple { SignalView::Signal::sine, 220.0, 0.1, 1, "signals_sine_soft_spectrum.png" },
           std::tuple { SignalView::Signal::sine, 220.0, 0.5, 1, "signals_sine_hard_spectrum.png" },
           std::tuple { SignalView::Signal::loudSoft, 110.0, 0.5, 0, "signals_loud_soft_time.png" },
           std::tuple { SignalView::Signal::sine, 220.0, 0.5, 2, "signals_sine_hard_curve.png" } })
    {
        std::unique_ptr<juce::AudioProcessorEditor> editor (p->createEditor());
        auto* ed = dynamic_cast<CustomAmpEditor*> (editor.get());
        editor->setSize (1120, 720);
        ed->showSignals (true);
        auto& view = ed->getSignalView();
        view.setFrequencyView (mode == 1);

        if (mode == 2)
            view.setCurveView (true);

        view.setSignal (signal, freq, peak);

        for (int i = 0; i < 300 && view.isWaiting(); ++i)
            pump (50);

        view.setBounds (0, 0, 1000, 80 + 66 * (int) view.getTraces().size());
        pump (100);
        auto image = view.createComponentSnapshot (view.getLocalBounds(), true, 1.5f);
        auto out = folder.getChildFile (name);
        out.deleteFile();
        juce::FileOutputStream stream (out);
        juce::PNGImageFormat().writeImageToStream (image, stream);
        std::cout << "wrote " << out.getFullPathName() << "\n";
    }

    // brit_800 with its sag block replaced by a rectifier supply on the power block (250 ohm, 47 uF).
    {
        auto preset = juce::JSON::parse (juce::JSON::toString (p->getPresetData()));
        auto* chain = preset["chain"].getArray();

        for (int i = chain->size(); --i >= 0;)
        {
            if ((*chain)[i]["type"] == "sag")
                chain->remove (i);
            else if ((*chain)[i]["type"] == "feedback")
                for (auto& inner : *(*chain)[i]["chain"].getArray())
                    if (inner["type"] == "power")
                        inner.getDynamicObject()->setProperty ("supply_ohms", 250);
        }

        auto supplyFile = folder.getChildFile ("brit_800_supply.json");
        supplyFile.replaceWithText (juce::JSON::toString (preset));
        p->loadPresetFile (supplyFile);
        waitForBuild (*p);
        std::unique_ptr<juce::AudioProcessorEditor> editor (p->createEditor());
        auto* ed = dynamic_cast<CustomAmpEditor*> (editor.get());
        editor->setSize (1120, 720);
        ed->showSignals (true);
        auto& view = ed->getSignalView();
        view.setFrequencyView (false);
        view.setSignal (SignalView::Signal::loudSoft, 110.0, 0.5);

        for (int i = 0; i < 300 && view.isWaiting(); ++i)
            pump (50);

        view.setBounds (0, 0, 1000, 80 + 66 * (int) view.getTraces().size());
        pump (100);
        auto image = view.createComponentSnapshot (view.getLocalBounds(), true, 1.5f);
        auto out = folder.getChildFile ("signals_supply_loud_soft.png");
        out.deleteFile();
        juce::FileOutputStream stream (out);
        juce::PNGImageFormat().writeImageToStream (image, stream);
        std::cout << "wrote " << out.getFullPathName() << "\n";
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
    std::cout << std::unitbuf;  // every line out at once, so a crash still shows how far it got

    if (argc == 3 && juce::String (argv[1]) == "--snapshot")
        return snapshots (juce::File (argv[2]));

    auto p = makePlugin();

    std::cout << "Default preset\n";
    check (! p->getStatus().isError, "builds: " + p->getStatus().text.upToFirstOccurrenceOf ("\n", false, false));
    check (p->getLatencySamples() > 0 && p->getLatencySamples() < 20, "latency " + juce::String (p->getLatencySamples()) + " samples");
    auto level = play (*p, 2.0);
    check (std::abs (level + 20.0) < 3.0, "the level riff comes out near -20 dBFS: " + juce::String (level, 1));

    std::cout << "Every built-in preset, switched while playing\n";
    const auto riff = amp::makeLevelRiff (rate);
    double riffSquares = 0.0;

    for (auto v : riff)
        riffSquares += v * v;

    const double riffLevel = juce::Decibels::gainToDecibels (std::sqrt (riffSquares / (double) riff.size()));

    for (auto& preset : CustomAmpProcessor::getBuiltInPresets())
    {
        p->loadBuiltInPreset (preset.fileName);
        waitForBuild (*p);
        level = play (*p, 1.0);
        // An effect (auto_level off) comes out near the guitar's own level (the riff's), an amp near -20 dBFS. The talk
        // box goes after another amp, so a guitar is not its input: only that it plays.
        const bool effect = preset.fileName.startsWith ("effect_"), afterAmp = preset.fileName == "effect_talk_box.json";
        check (! p->getStatus().isError && (afterAmp ? level > -60.0 && level < 0.0 : std::abs (level - (effect ? riffLevel : -20.0)) < (effect ? 6.0 : 3.0)),
               preset.fileName + ": " + juce::String (level, 1) + " dBFS" + (effect ? " (the guitar: " + juce::String (riffLevel, 1) + ")" : juce::String())
                   + (p->getStatus().isError ? "  " + p->getStatus().text : juce::String()));
    }

    {
        auto empty = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("custom_amp_empty.json");
        empty.replaceWithText (R"({ "name": "Nothing", "auto_level": false, "chain": [] })");
        p->loadPresetFile (empty);
        waitForBuild (*p);
        level = play (*p, 1.0);
        check (! p->getStatus().isError && std::abs (level - riffLevel) < 0.05 && p->getStatus().text.contains ("auto level off: 0.0 dB"),
               "an empty chain with auto_level off: output = input (" + juce::String (level, 2) + " dBFS, the guitar " + juce::String (riffLevel, 2) + ")");
        empty.deleteFile();
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

        // A new tone stack style without a mid knob: the mid settings must go, or the preset stops with an error.
        {
            auto tone = ed->getEditedPreset()["chain"][6];
            tone.getDynamicObject()->setProperty ("mid", 7);
            ed->select ({ ed->getEditedPreset()["chain"], 6, false });
            pump (50);
            auto* style = findControl<juce::ComboBox> (*editor, "style");
            check (style != nullptr, "the tone stack's style menu is there");

            if (style != nullptr)
            {
                for (int i = 0; i < style->getNumItems(); ++i)
                    if (style->getItemText (i) == "james")
                        style->setSelectedItemIndex (i, juce::sendNotificationSync);

                pump (50);
                waitForBuild (*q);
                const auto now = q->getPresetData()["chain"][6];
                check (now["style"].toString() == "james" && ! now.hasProperty ("mid") && ! q->getStatus().isError,
                       "switching the tone stack to james removes its mid knob and builds: "
                           + q->getStatus().text.upToFirstOccurrenceOf ("\n", false, false));
                check (findControl<juce::Slider> (*editor, "mid") == nullptr && findControl<juce::Slider> (*editor, "bass") != nullptr,
                       "the inspector shows no mid slider for james");
            }

            q->undo();
            waitForBuild (*q);
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
        check (! ed->keyPressed (juce::KeyPress ('d', juce::ModifierKeys::commandModifier, 0)), "on the front panel the block keys do nothing");
        ed->showView (false);  // the chain editor, where the blocks are
        const auto key = [ed] (int k) { return ed->keyPressed (juce::KeyPress (k, juce::ModifierKeys::commandModifier, 0)); };
        const auto chain = [ed] { return ed->getEditedPreset()["chain"]; };
        const auto selectAt = [ed, chain] (int index) { ed->select ({ chain(), index, false }); pump (50); };
        const auto firstLine = [&q] { return q->getStatus().text.upToFirstOccurrenceOf ("\n", false, false); };

        // The feedback loop (nfb: pi, pa). The copy's ids get a number; the first loop keeps its names.
        selectAt (8);
        check (key ('d'), "Ctrl+D duplicates");
        waitForBuild (*q);
        const auto copy = chain()[9];
        check (chain().size() == 11 && copy["id"].toString() == "nfb_2" && copy["chain"][0]["id"].toString() == "pi_2"
                   && copy["chain"][1]["id"].toString() == "pa_2",
               "the copy's ids get a number: " + copy["id"].toString() + ", " + copy["chain"][0]["id"].toString() + ", "
                   + copy["chain"][1]["id"].toString());
        check (chain()[8]["chain"][1]["id"].toString() == "pa", "the first loop's power block is still pa");
        check (! q->getStatus().isError, "the preset still builds: " + firstLine());
        check (ed->getSelection().index == 9, "the copy is selected");

        // Copy the tone stack, paste it after the first block.
        selectAt (6);
        bool copied = true, onClipboard = false;

        for (int i = 0; i < 100 && ! onClipboard; ++i, juce::Thread::sleep (20))  // it may be busy for a while: copy again every 0.5 s
        {
            if (i % 25 == 0)
                copied = key ('c') && copied;

            onClipboard = juce::SystemClipboard::getTextFromClipboard().contains ("\"tonestack\"");
        }

        // Another program can hold the Windows clipboard open for minutes; then no program can copy, and this test can
        // not tell anything (seen on 2026-11-26: 10 of 10 tries to open it failed). Say so instead of failing.
        bool locked = false;
#if JUCE_WINDOWS
        if (! onClipboard)
        {
            locked = OpenClipboard (nullptr) == 0;

            if (! locked)
                CloseClipboard();
        }
#endif
        if (locked)
            std::cout << "  skip  Ctrl+C copies it as JSON: another program holds the Windows clipboard open, so nothing can copy now" << std::endl;
        else
            check (copied && onClipboard, "Ctrl+C copies it as JSON");
        selectAt (0);
        check (key ('v'), "Ctrl+V pastes");
        pump (50);
        const auto withoutId = [] (const juce::var& item)
        {
            auto copy = juce::JSON::parse (juce::JSON::toString (item));
            copy.getDynamicObject()->removeProperty ("id");
            return juce::JSON::toString (copy);
        };
        check (chain().size() == 12 && withoutId (chain()[1]) == withoutId (chain()[7]) && chain()[1]["id"].toString() == "tone_2",
               "the pasted block is the same as the copied one, with its own id: " + chain()[1]["id"].toString());
        check (ed->getSelection().index == 1, "the pasted block is selected");

        check (key ('x') && chain().size() == 11, "Ctrl+X takes it out");
        waitForBuild (*q);
        q->undo();
        waitForBuild (*q);
        check (chain().size() == 12 && chain()[1]["type"].toString() == "tonestack", "undo puts it back");

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

        // A gain stage pasted "at the end" goes before the cab (only eq and ir can follow it).
        selectAt (0);
        key ('c');
        ed->select ({});
        pump (50);
        key ('v');
        const int last = chain().size() - 1;
        check (chain()[last - 2]["id"].toString() == "v1a_2" && chain()[last - 1]["type"].toString() == "ir",
               "a gain stage pasted at the end goes before the cab IR");
        waitForBuild (*q);
        check (! q->getStatus().isError, "and the preset builds: " + firstLine());

        const auto list = [] (const char* json) { return juce::JSON::parse (juce::String (json)); };
        const auto gain = makeBlock ("gain"), eq = makeBlock ("eq");
        check (endInsertIndex (list (R"([ { "type": "gain" }, { "type": "eq" }, { "type": "ir" }, { "type": "eq" } ])"), gain) == 2
                   && endInsertIndex (list (R"([ { "type": "gain" }, { "type": "eq" }, { "type": "ir" }, { "type": "eq" } ])"), eq) == 4
                   && endInsertIndex (list (R"([ { "type": "gain" }, { "type": "eq" } ])"), gain) == 2,
               "end place: before the ir (eq right before it stays in front), at the very end with no ir");
        const auto withRoom = list (R"([ { "type": "gain" }, { "type": "ir" }, { "type": "room" } ])");
        check (endInsertIndex (withRoom, gain) == 1 && endInsertIndex (withRoom, eq) == 2 && endInsertIndex (withRoom, makeBlock ("room")) == 3,
               "a room block stays last: a gain goes before the cab, an eq before the room, a new room at the very end");

        // Inside the feedback loop only loop blocks can go. (brit_800 has no sag block: one is put in front for a
        // moment, only to copy it.)
        chain().getArray()->insert (0, list (R"({ "type": "sag", "targets": [] })"));
        selectAt (0);
        key ('c');
        chain().getArray()->remove (0);
        int loopIndex = -1;

        for (int i = 0; i < chain().size(); ++i)
            if (chain()[i]["id"].toString() == "nfb")
                loopIndex = i;

        const auto loop = chain()[loopIndex]["chain"];
        const int loopSize = loop.size();
        ed->select ({ loop, 0, false });
        pump (50);
        check (! key ('v') && loop.size() == loopSize, "a sag block can not be pasted into the feedback loop");
        const bool duplicated = key ('d');
        const auto newId = loop[1]["id"].toString();
        check (duplicated && loop.size() == loopSize + 1 && newId == "pi_3", "a gain stage in the loop can be duplicated: " + newId);
        waitForBuild (*q);
        check (! q->getStatus().isError, "and the preset builds: " + firstLine());

        // The id rules, on their own.
        juce::String problem;
        const auto preset = ed->getEditedPreset();
        auto split = makePastedItem (R"({ "type": "split", "paths": [ { "chain": [ { "type": "sag", "targets": [ "v1b", "pa" ] },
                                         { "type": "gain", "id": "v1b" } ] } ] })", preset, false, problem);
        const auto inner = split["paths"][0]["chain"];
        check (inner[1]["id"].toString() == "v1b_2" && inner[0]["targets"][0].toString() == "v1b_2"
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

    std::cout << "Front panel\n";
    {
        SettingsHelp help;

        // The shared cases: exactly these errors.
        juce::var cases;
        const auto casesFile = juce::File::getCurrentWorkingDirectory().getChildFile ("plugin/tests/panel_cases.json");
        check (juce::JSON::parse (casesFile.loadFileAsString(), cases).wasOk(), "read " + casesFile.getFullPathName());
        int same = 0, total = 0;

        for (int i = 0; cases["cases"].isArray() && i < cases["cases"].size(); ++i, ++total)
        {
            auto* preset = new juce::DynamicObject();
            preset->setProperty ("chain", cases["chain"]);
            preset->setProperty ("panel", cases["cases"][i]["panel"]);
            juce::String error;
            bool guessed = false;

            try
            {
                panel::read (juce::var (preset), help, guessed);
            }
            catch (const amp::PresetError& e)
            {
                error = e.what();
            }

            const auto expected = cases["cases"][i]["error"];

            if (error == (expected.isVoid() ? juce::String() : expected.toString()))
                ++same;
            else
                std::cout << "        case " << i + 1 << ": got '" << error << "', expected '" << expected.toString() << "'\n";
        }

        check (total > 10 && same == total, "the shared panel cases give exactly their errors: " + juce::String (same) + " of " + juce::String (total));

        const std::map<juce::String, int> knobCounts { { "brit_800.json", 6 }, { "brit_plexi.json", 6 }, { "brit_top_boost.json", 3 },
                                                       { "clean.json", 5 }, { "crunch.json", 5 }, { "german_high_gain.json", 7 },
                                                       { "high_gain.json", 6 }, { "us_blackface_clean.json", 4 },
                                                       { "us_hotrod_lead.json", 8 }, { "us_modern_high_gain.json", 7 },
                                                       { "instrument_bass.json", 6 }, { "instrument_cello.json", 6 },
                                                       { "instrument_bowed_sitar.json", 7 }, { "instrument_bell_strings.json", 6 },
                                                       { "instrument_talking_guitar.json", 6 }, { "effect_strings.json", 4 },
                                                       { "effect_restring.json", 5 }, { "effect_bow.json", 6 }, { "effect_talk_box.json", 6 } };
        int right = 0;

        for (auto& [fileName, count] : knobCounts)
        {
            q->loadBuiltInPreset (fileName);
            waitForBuild (*q);

            if ((int) q->getPanel().size() == count && ! q->isPanelGuessed() && ! q->getStatus().isError)
                ++right;
            else
                std::cout << "        " << fileName << ": " << q->getPanel().size() << " knobs, " << q->getStatus().text.upToFirstOccurrenceOf ("\n", false, false) << "\n";
        }

        check (right == (int) knobCounts.size(), "every built-in preset has its own panel and builds");

        q->loadBuiltInPreset ("brit_800.json");
        waitForBuild (*q);
        const auto knob = [&q] (int n) { return q->parameters.getParameter ("knob_" + juce::String (n)); };
        const auto position = [&knob] (int n) { return knob (n)->convertFrom0to1 (knob (n)->getValue()); };
        const auto chain = [&q] { return q->getPresetData()["chain"]; };
        check (std::abs (position (1) - 7.0f) < 0.01f && std::abs (position (4) - 6.0f) < 0.01f && std::abs (position (5) - 5.0f) < 0.01f
                   && std::abs (position (6) - 5.75f) < 0.01f,
               "the knobs show the preset: Gain " + juce::String (position (1), 2) + " (-12 dB of -40..0), Treble "
                   + juce::String (position (4), 2) + ", Presence " + juce::String (position (5), 2) + ", Master " + juce::String (position (6), 2));

        knob (6)->setValueNotifyingHost (1.0f);  // Master to 10
        knob (5)->setValueNotifyingHost (1.0f);  // Presence to 10 (its range runs backwards: 0 dB at 0, -12 dB at 10)
        waitForBuild (*q);
        check ((double) chain()[7]["level_db"] == 0.0 && (double) chain()[8]["path"][0]["gain_db"] == -12.0 && q->isEdited(),
               "moving knobs from the DAW changes the preset: master " + chain()[7]["level_db"].toString() + " dB, presence band "
                   + chain()[8]["path"][0]["gain_db"].toString() + " dB");
        check (q->getStatus().text.contains ("level kept"), "and keeps the level");

        // A knob moved in small steps (like a drag or automation) is one undo step.
        q->undo();
        waitForBuild (*q);
        const auto gainBefore = chain()[1]["level_db"].toString();

        for (float v = 0.75f; v <= 0.951f; v += 0.05f)
        {
            knob (1)->setValueNotifyingHost (v);
            pump (150);  // the timer writes each move
        }

        waitForBuild (*q);
        const auto gainAfter = chain()[1]["level_db"].toString();
        q->undo();
        waitForBuild (*q);
        check (gainAfter == "-2" && chain()[1]["level_db"].toString() == gainBefore && std::abs (position (1) - 7.0f) < 0.01f,
               "a moving knob is one undo step: " + gainBefore + " dB -> " + gainAfter + " dB -> undo -> "
                   + chain()[1]["level_db"].toString() + " dB, knob back at " + juce::String (position (1), 2));

        // A preset without a panel: guessed knobs, and they work.
        auto noPanel = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("custom_amp_no_panel.json");
        noPanel.replaceWithText (R"({ "name": "No panel", "chain": [ { "type": "gain", "drive_db": 12 }, { "type": "tonestack" },
                                    { "type": "eq", "level_db": -6 } ] })");
        q->loadPresetFile (noPanel);
        waitForBuild (*q);
        check (q->isPanelGuessed() && q->getPanel().size() == 4 && q->getPanel()[3].label == "Master",
               "without a panel: guessed Bass, Middle, Treble, Master (" + juce::String ((int) q->getPanel().size()) + " knobs)");
        knob (1)->setValueNotifyingHost (0.8f);
        waitForBuild (*q);
        check ((double) chain()[1]["bass"] == 8.0 && ! q->getPresetData().hasProperty ("panel"), "a guessed knob turns the tone stack's bass");

        // Put on the front panel (right-click a slider): a guessed panel becomes the preset's own, the block gets an id.
        {
            std::unique_ptr<juce::AudioProcessorEditor> editor (q->createEditor());
            auto* ed = dynamic_cast<CustomAmpEditor*> (editor.get());
            check (! ed->isShowingPanel(), "the window remembers the last view (the chain editor, from the copy test)");
            ed->showView (true);
            check (ed->isShowingPanel() && ed->getPanelView().getNumKnobsShown() == 4, "the panel shows the 4 knobs");
            ed->putOnPanel (ed->getEditedPreset()["chain"][0], "drive_db");
            waitForBuild (*q);
            const auto panelList = q->getPresetData()["panel"];
            check (panelList.size() == 5 && panelList[4]["block"].toString() == "gain" && chain()[0]["id"].toString() == "gain"
                       && chain()[1]["id"].toString() == "tonestack" && ! q->isPanelGuessed() && ed->getPanelView().getNumKnobsShown() == 5,
                   "Put on the front panel adds knob 5; the blocks get ids (gain, tonestack)");

            for (auto* setting : { "level_db", "bias", "bias_shift", "shift_release_ms" })
                ed->putOnPanel (ed->getEditedPreset()["chain"][0], setting);

            pump (50);
            check (q->getPresetData()["panel"].size() == 8, "up to 8 knobs");
            ed->putOnPanel (ed->getEditedPreset()["chain"][0], "shift_attack_ms");
            pump (50);
            check (q->getPresetData()["panel"].size() == 8, "a ninth is refused");
            waitForBuild (*q);
            check (! q->getStatus().isError, "the preset with 8 knobs builds: " + q->getStatus().text.upToFirstOccurrenceOf ("\n", false, false));
        }

        // A mistake in the panel stops the preset with its message; the last amp keeps playing.
        noPanel.replaceWithText (R"({ "name": "Bad panel", "chain": [ { "type": "gain", "id": "v1" } ],
                                    "panel": [ { "block": "v2", "setting": "drive_db" } ] })");
        q->loadPresetFile (noPanel);
        waitForBuild (*q);
        check (q->getStatus().isError && q->getStatus().text.startsWith ("panel knob 1: no block has the id 'v2'"),
               "a wrong panel shows the error: " + q->getStatus().text.upToFirstOccurrenceOf ("\n", false, false));
        noPanel.deleteFile();
    }

    std::cout << "X-ray listening\n";
    {
        auto x = makePlugin();
        auto amp = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("custom_amp_xray.json");
        amp.replaceWithText (R"({ "name": "X-ray", "chain": [ { "type": "gain", "id": "v1", "curve": "triode", "plate_volts": true },
            { "type": "eq", "id": "nothing" }, { "type": "eq", "level_db": -30 }, { "type": "gain", "id": "dirt", "curve": "tube", "drive_db": 18 },
            { "type": "split", "id": "both", "paths": [ { "chain": [] } ] } ] })");
        x->loadPresetFile (amp);
        waitForBuild (*x);
        const double normal = play (*x, 1.0);
        const auto listen = [&x] (const juce::StringArray& path, CustomAmpProcessor::Listen mode)
        {
            x->xray (path, mode);
            waitForBuild (*x);
            play (*x, 2.0);        // the twin catches up with everything the amp has heard
            return play (*x, 1.0);
        };

        // An eq with no bands changes nothing: amp minus twin must be silence (the two line up sample for sample).
        const double same = listen ({ "chain", "1" }, CustomAmpProcessor::Listen::onlyAdds);
        check (x->getXrayBlock() == "nothing" && same < normal - 100, "X-ray of a block that changes nothing: "
                                                                          + juce::String (same - normal, 0) + " dB against the amp (silence)");
        const double distortion = listen ({ "chain", "3" }, CustomAmpProcessor::Listen::onlyAdds);
        const double without = listen ({ "chain", "3" }, CustomAmpProcessor::Listen::without);
        check (x->getXrayBlock() == "dirt" && distortion > normal - 30 && std::abs (without - normal) > 0.1,
               "X-ray of a distorting stage: only what it adds " + juce::String (distortion - normal, 1) + " dB against the amp; the amp without it "
                   + juce::String (without - normal, 1) + " dB");
        x->stopXray();
        waitForBuild (*x);
        const double back = play (*x, 1.0);
        x->xray ({ "chain", "4" }, CustomAmpProcessor::Listen::onlyAdds);
        check (std::abs (back - normal) < 0.01 && x->getXrayBlock().isEmpty(), "stop X-ray: the amp again (" + juce::String (back - normal, 3)
                                                                                     + " dB); a split can not be X-rayed (no single 'off')");
        amp.deleteFile();
    }

    std::cout << "Meters and tuner\n";
    {
        auto m = makePlugin();
        waitForBuild (*m);
        const auto feed = [&m] (double hz, float peak, double seconds)
        {
            juce::AudioBuffer<float> buffer (2, block);
            juce::MidiBuffer midi;
            static double phase = 0;

            for (int b = 0; b < (int) (seconds * rate / block); ++b)
            {
                for (int i = 0; i < block; ++i, phase += juce::MathConstants<double>::twoPi * hz / rate)
                    for (int ch = 0; ch < 2; ++ch)
                        buffer.setSample (ch, i, peak * (float) std::sin (phase));

                m->processBlock (buffer, midi);
            }
        };

        // A2 10 cents sharp at 0.5 V: the tuner says A2 +10, the input meter -6.02 dB (1 V = 0 dB).
        m->meters.tunerOn = true;
        m->meters.inPeak = 0;
        feed (110.0 * std::pow (2.0, 10 / 1200.0), 0.5f, 1.0);
        double cents = 0;
        const auto name = MeterView::noteName (m->meters.noteHz.load(), cents);
        const double inDb = 20 * std::log10 (m->meters.inPeak.exchange (0));
        const float cpu = m->meters.cpu.load();
        check (name == "A2" && std::abs (cents - 10) < 0.5 && std::abs (inDb + 6.02) < 0.01 && cpu > 0 && cpu < 1 && ! m->meters.inClip.load(),
               "tuner: " + name + " " + juce::String (cents, 2) + " cents (A2 +10), input meter " + juce::String (inDb, 2) + " dB (-6.02), CPU "
                   + juce::String (100 * cpu, 2) + "%, no clip");

        // Full scale at the input lights the clip light; silence: no note.
        feed (110.0, 1.0f, 0.1);
        const bool clipped = m->meters.inClip.exchange (false);
        feed (110.0, 0.0f, 0.5);
        double c2 = 0;
        check (clipped && m->meters.noteHz.load() == 0 && MeterView::noteName (82.4069, c2) == "E2" && std::abs (c2) < 0.01
                   && MeterView::noteName (440.0, c2) == "A4",
               "full scale at the input: clip light; silence: no note; note names E2 (82.41 Hz), A4 (440 Hz)");

        // The tracker only runs while a window shows the tuner.
        m->meters.tunerOn = false;
        m->meters.noteHz = 0;
        feed (220.0, 0.5f, 1.0);
        check (m->meters.noteHz.load() == 0, "with no window showing it, the tuner does not run (CPU then "
                                                  + juce::String (100 * m->meters.cpu.load(), 2) + "%; " + m->getPresetSource() + ")");
    }

    std::cout << "Signal view\n";
    {
        auto s = makePlugin();
        s->loadBuiltInPreset ("brit_800.json");
        waitForBuild (*s);
        std::unique_ptr<juce::AudioProcessorEditor> editor (s->createEditor());
        auto* ed = dynamic_cast<CustomAmpEditor*> (editor.get());
        auto& view = ed->getSignalView();
        const auto waitForSignals = [&view]
        {
            for (int i = 0; i < 300 && view.isWaiting(); ++i)
                pump (50);
        };
        // The level of harmonic k of the note in a strip, as a share of the first (from the strip's spectrum).
        const auto harmonic = [] (const SignalView::Trace& t, double freq, int k)
        {
            const auto level = [&t, freq] (int h)
            {
                const int centre = juce::roundToInt (h * freq / t.binHz);
                double power = 0.0;

                for (int b = centre - 5; b <= centre + 5; ++b)
                    power += std::pow (10.0, t.spectrumDb[(size_t) b] / 10.0);

                return std::sqrt (power);
            };

            return level (k) / level (1);
        };

        ed->showSignals (true);
        view.setSignal (SignalView::Signal::sine, 220.0, 0.5);
        waitForSignals();
        const auto& traces = view.getTraces();
        check (view.getProblem().isEmpty() && traces.size() == 14 && traces[10].name == "1. gain  pi" && traces[10].indent == 1
                   && traces[11].name == "2. power  pa" && traces[12].name.contains ("supply of pa"),
               "brit_800: the input, 12 blocks (the loop's two inside it) and pa's supply: " + juce::String ((int) traces.size()) + " strips"
                   + (view.getProblem().isNotEmpty() ? ", " + view.getProblem() : juce::String()));

        if (traces.size() == 14)
        {
            check (std::abs (traces[0].peak - 0.5f) < 0.005f && harmonic (traces[0], 220.0, 2) < 1e-4,
                   "the input is the sine: peak " + juce::String (traces[0].peak, 3) + " V, 2nd harmonic "
                       + juce::String (100.0 * harmonic (traces[0], 220.0, 2), 4) + "%");
            float lowest = 1.0f, highest = 0.0f;

            for (auto v : traces[12].samples)
            {
                lowest = juce::jmin (lowest, v);
                highest = juce::jmax (highest, v);
            }

            check (traces[12].isUnit && lowest > 0.6f && lowest < 0.995f && highest <= 1.0f,
                   "the supply strip is B+ / plate_v (80 ohm, 50 uF): " + juce::String (lowest, 3) + " to " + juce::String (highest, 3)
                       + ", " + traces[12].summary);
            const double inner = (double) traces[11].samples.size() / traces[11].rate;
            check (traces[11].rate > 300000.0 && inner > 0.17 && traces[11].peak > 1.0f,
                   "the power block inside the loop is recorded at the core rate: " + juce::String (inner, 3) + " s, peak "
                       + juce::String (traces[11].peak, 1) + " V");
            check (traces[10].inputs.size() == traces[10].samples.size() && traces[10].inputs.size() > 1000,
                   "a block inside the loop has its inputs too (curve view): " + juce::String ((int) traces[10].inputs.size()) + " pairs");

            view.onSelect (traces[3].target);
            pump (50);
            check (ed->getSelection().index == 2 && ed->getSelection().item()["id"].toString() == "v1b", "clicking strip 3 selects v1b");
        }

        // One triode stage at 0.5 V: the numbers checked against tube data (Checks done so far: 2.6% 2nd, 0.1% 3rd).
        auto triode = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("custom_amp_triode.json");
        triode.replaceWithText (R"({ "name": "One triode", "chain": [ { "type": "gain", "curve": "triode", "tube": "12AX7",
                                    "plate_v": 300, "plate_ohms": 100000, "bias_v": -1.5 } ] })");
        s->loadPresetFile (triode);
        waitForBuild (*s);
        waitForSignals();

        if (view.getTraces().size() == 2)
        {
            const auto& t = view.getTraces()[1];
            const double h2 = 100.0 * harmonic (t, 220.0, 2), h3 = 100.0 * harmonic (t, 220.0, 3);
            check (std::abs (h2 - 2.64) < 0.05 && std::abs (h3 - 0.126) < 0.02,
                   "one 12AX7 stage at 0.5 V: 2nd harmonic " + juce::String (h2, 2) + "%, 3rd " + juce::String (h3, 3)
                       + "% (the same as EngineTest's triode check)");
            check (t.summary.contains ("THD") && t.summary.contains ("H2"), "its summary lists the harmonics: " + t.summary.replace ("\n", " | "));
        }
        else
        {
            check (false, "one triode: 2 strips, got " + juce::String ((int) view.getTraces().size()) + " " + view.getProblem());
        }

        view.setFrequencyView (true);
        view.setSignal (SignalView::Signal::pluck, 110.0, 0.5);
        waitForSignals();
        check (view.getTraces().size() == 2 && view.getTraces()[0].peak > 0.45f && ! view.getTraces()[1].spectrumDb.empty(),
               "a plucked note, spectrum view: peak " + juce::String (view.getTraces().empty() ? 0.0f : view.getTraces()[0].peak, 3) + " V");

        // Curve view: each block's input is recorded at the same moments as its output. A hard clip with drive x4
        // (no coupling capacitor) must be exactly clip (4 x input) at every sample.
        triode.replaceWithText (R"({ "name": "Clip", "chain": [ { "type": "gain", "curve": "hard", "drive_db": 12.0412, "coupling_nf": 0 } ] })");
        s->loadPresetFile (triode);
        waitForBuild (*s);
        view.setCurveView (true);
        view.setSignal (SignalView::Signal::sine, 220.0, 0.5);
        waitForSignals();

        if (view.getTraces().size() == 2)
        {
            const auto& t = view.getTraces()[1];
            double worst = 0.0;

            for (size_t i = 0; i < std::min (t.samples.size(), t.inputs.size()); ++i)
                worst = std::max (worst, std::abs ((double) t.samples[i] - juce::jlimit (-1.0, 1.0, 4.0 * (double) t.inputs[i])));

            check (! t.inputs.empty() && t.inputs.size() == t.samples.size() && worst < 1e-5,
                   "the curve view pairs input and output: a hard clip x4 is clip (4 x input) to " + juce::String (worst, 8)
                       + " (" + juce::String ((int) t.inputs.size()) + " pairs)");
        }
        else
        {
            check (false, "curve view: 2 strips, got " + juce::String ((int) view.getTraces().size()) + " " + view.getProblem());
        }

        // A/B: keep the strips, then insert an eq before g1 and change its drive: g1 (now block 2) still finds its A.
        triode.replaceWithText (R"({ "name": "A", "chain": [ { "type": "gain", "id": "g1", "curve": "hard", "drive_db": 12.0412, "coupling_nf": 0 } ] })");
        s->loadPresetFile (triode);
        waitForBuild (*s);
        waitForSignals();
        view.keepAsA (true);
        const auto keptPeak = view.getTraces().size() == 2 ? view.getTraces()[1].peak : 0.0f;
        triode.replaceWithText (R"({ "name": "B", "chain": [ { "type": "eq", "id": "e0" },
                                    { "type": "gain", "id": "g1", "curve": "hard", "drive_db": 0, "coupling_nf": 0 } ] })");
        s->loadPresetFile (triode);
        waitForBuild (*s);
        waitForSignals();

        if (view.getTraces().size() == 3)
        {
            const auto* a = view.heldFor (view.getTraces()[2]);
            check (a != nullptr && a->name == "1. gain  g1" && a->peak == keptPeak && view.heldFor (view.getTraces()[1]) == nullptr
                       && std::abs (view.getTraces()[2].peak - 0.5f) < 0.01f && std::abs (keptPeak - 1.0f) < 0.01f,
                   "A/B: g1 (now block 2) finds its A (peak " + juce::String (keptPeak, 3) + " V, clipped) under the new one (peak "
                       + juce::String (view.getTraces()[2].peak, 3) + " V); the new eq has no A");
        }
        else
        {
            check (false, "A/B: 3 strips, got " + juce::String ((int) view.getTraces().size()) + " " + view.getProblem());
        }

        view.keepAsA (false);
        check (view.heldFor (view.getTraces().back()) == nullptr, "dropping A: no strip has one");

        // Rectifier sag (supply_ohms): a 40 V loud part, then 10 V, through one EL34 pair.
        view.setFrequencyView (false);
        std::vector<SignalView::Trace> stiff, sagging;

        for (auto [ohms, into] : { std::pair { 0, &stiff }, std::pair { 250, &sagging } })
        {
            triode.replaceWithText (R"({ "name": "Supply", "chain": [ { "type": "power", "id": "pa", "tube": "EL34", "plate_v": 450,
                                        "bias_v": -45, "coupling_nf": 0, "supply_uf": 47, "supply_ohms": )" + juce::String (ohms) + " } ] }");
            s->loadPresetFile (triode);
            waitForBuild (*s);
            view.setSignal (SignalView::Signal::loudSoft, 110.0, 40.0);
            waitForSignals();
            *into = view.getTraces();
        }

        view.setTimeSpanMs (500);
        triode.replaceWithText (R"({ "name": "No supply setting", "chain": [ { "type": "power", "id": "pa", "tube": "EL34", "plate_v": 450,
                                    "bias_v": -45, "coupling_nf": 0 } ] })");
        s->loadPresetFile (triode);
        waitForBuild (*s);
        waitForSignals();
        check (stiff.size() == 2 && view.getTraces().size() == 2 && stiff[1].samples == view.getTraces()[1].samples,
               "supply_ohms 0 = the power block as before, sample for sample");

        if (sagging.size() == 3 && stiff.size() == 2)
        {
            const auto& supply = sagging[2].samples;
            const auto at = [&supply, &sagging] (double seconds) { return supply[(size_t) (seconds * sagging[2].rate)]; };
            const float lowest = *std::min_element (supply.begin(), supply.begin() + (int) (0.2 * sagging[2].rate));
            check (sagging[2].name.contains ("supply of pa") && std::abs (supply[0] - 1.0f) < 1e-6f && lowest < 0.97f && at (0.49) > lowest,
                   "the supply starts at plate_v, drops while playing hard (B+ down to " + juce::String (juce::roundToInt (450.0f * lowest))
                       + " V) and comes back up when soft (" + juce::String (juce::roundToInt (450.0f * at (0.49))) + " V)");
            check (sagging[1].peak < stiff[1].peak, "the pair compresses: peak " + juce::String (sagging[1].peak, 1) + " V with sag, "
                                                        + juce::String (stiff[1].peak, 1) + " V without");

            // After the loud part the supply recovers with R C / (1 + g R): the tubes take g = 1.14 mA more per volt of
            // supply (worked out from the tube model at 10 V drive, 446 V), like a resistor across the capacitor.
            const double coreRate = sagging[2].rate, tau = 250 * 47e-6 / (1 + 1.14e-3 * 250);
            const size_t from = (size_t) (0.2 * coreRate);
            const float end = at (0.49), gap = end - supply[from];
            size_t i = from;

            while (i < supply.size() && end - supply[i] > gap * std::exp (-1.0))
                ++i;

            const double t63 = (double) (i - from) / coreRate;
            check (std::abs (t63 - tau) < 0.05 * tau, "it recovers: 63% back after " + juce::String (t63 * 1000, 2)
                                                          + " ms (theory: 11.75 ms / (1 + g R) = " + juce::String (tau * 1000, 2) + " ms)");
        }
        else
        {
            check (false, "supply: 3 strips with it, 2 without; got " + juce::String ((int) sagging.size()) + " and "
                              + juce::String ((int) stiff.size()) + " " + view.getProblem());
        }

        triode.replaceWithText (R"({ "name": "Broken", "chain": [ { "type": "gain", "curv": "tube" } ] })");
        s->loadPresetFile (triode);
        waitForBuild (*s);
        waitForSignals();
        check (view.getProblem().contains ("curv") && view.getTraces().empty(), "a broken preset shows the problem: " + view.getProblem());
        triode.deleteFile();
    }

    file.deleteFile();
    q.reset();
    p.reset();
    std::cout << (failures == 0 ? "All passed\n" : juce::String (failures) + " failed\n");
    return failures == 0 ? 0 : 1;
}
