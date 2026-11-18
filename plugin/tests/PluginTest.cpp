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
        editor->setSize (700, 500);
        editor->setSize (1200, 900);
        check (editor->getWidth() == 1200, "opens and resizes");
    }

    file.deleteFile();
    q.reset();
    p.reset();
    std::cout << (failures == 0 ? "All passed\n" : juce::String (failures) + " failed\n");
    return failures == 0 ? 0 : 1;
}
