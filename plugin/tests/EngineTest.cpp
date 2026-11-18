// Runs the C++ engine without a DAW, to compare it with the Python prototype (tools/cpp_check.py).
//
//   EngineTest core   <preset.json> <in.f64> <out.f64> <rate>   the oversampled part only, at <rate>, no oversampling
//   EngineTest tail   <preset.json> <in.f64> <out.f64> <rate>   the cab IR part only
//   EngineTest render <preset.json> <in.wav> <out.wav> [factorLog2] [blockSize]
//                     the whole plugin engine; prints the CPU time
//
// .f64 files are raw little-endian doubles (numpy: x.astype("<f8").tofile(path)).

#include "../Source/dsp/Engine.h"

#include <juce_audio_formats/juce_audio_formats.h>

#include <iostream>

static std::vector<double> readRaw (const juce::File& file)
{
    juce::MemoryBlock data;

    if (! file.loadFileAsData (data))
        throw std::runtime_error ("can not read " + file.getFullPathName().toStdString());

    std::vector<double> x (data.getSize() / sizeof (double));
    std::memcpy (x.data(), data.getData(), x.size() * sizeof (double));
    return x;
}

static void writeRaw (const juce::File& file, const std::vector<double>& x)
{
    file.deleteFile();
    juce::FileOutputStream out (file);
    out.write (x.data(), x.size() * sizeof (double));
}

static juce::var loadPreset (const juce::File& file)
{
    juce::var preset;
    auto result = juce::JSON::parse (file.loadFileAsString(), preset);

    if (result.failed())
        throw amp::PresetError (file.getFileName() + ": " + result.getErrorMessage());

    return preset;
}

static int runPart (const juce::StringArray& args, bool core)
{
    const juce::File presetFile (args[1]);
    const double rate = args[4].getDoubleValue();
    auto x = readRaw (juce::File (args[2]));

    amp::Amp amp (loadPreset (presetFile), rate, rate, presetFile.getParentDirectory(), {});
    const int block = 4096;
    amp.prepare (block, block);

    const auto start = juce::Time::getMillisecondCounterHiRes();

    for (size_t i = 0; i < x.size(); i += block)
    {
        const int n = (int) std::min ((size_t) block, x.size() - i);

        if (core)
            amp.processCore (x.data() + i, n);
        else
            amp.processTail (x.data() + i, n);
    }

    const auto ms = juce::Time::getMillisecondCounterHiRes() - start;
    writeRaw (juce::File (args[3]), x);
    std::cout << (core ? "core" : "tail") << ": " << x.size() << " samples in " << ms << " ms\n";
    return 0;
}

static int render (const juce::StringArray& args)
{
    const juce::File presetFile (args[1]);
    juce::AudioFormatManager formats;
    formats.registerBasicFormats();
    std::unique_ptr<juce::AudioFormatReader> reader (formats.createReaderFor (juce::File (args[2])));

    if (reader == nullptr)
        throw std::runtime_error ("can not read " + args[2].toStdString());

    juce::AudioBuffer<float> input ((int) reader->numChannels, (int) reader->lengthInSamples);
    reader->read (&input, 0, input.getNumSamples(), 0, true, true);
    std::vector<double> x ((size_t) input.getNumSamples(), 0.0);

    // The amp is mono: the average of the channels, like run.py.
    for (int i = 0; i < input.getNumSamples(); ++i)
    {
        double sum = 0.0;

        for (int ch = 0; ch < input.getNumChannels(); ++ch)
            sum += (double) input.getSample (ch, i);

        x[(size_t) i] = sum / input.getNumChannels();
    }

    amp::Engine::Options options;
    options.preset = loadPreset (presetFile);
    options.presetFolder = presetFile.getParentDirectory();
    options.sampleRate = reader->sampleRate;
    options.factorLog2 = args.size() > 4 ? args[4].getIntValue() : 3;
    options.maxBlock = args.size() > 5 ? args[5].getIntValue() : 256;

    const auto buildStart = juce::Time::getMillisecondCounterHiRes();
    amp::Engine engine (options);
    const auto buildMs = juce::Time::getMillisecondCounterHiRes() - buildStart;

    const auto start = juce::Time::getMillisecondCounterHiRes();

    for (size_t i = 0; i < x.size(); i += (size_t) options.maxBlock)
        engine.process (x.data() + i, (int) std::min ((size_t) options.maxBlock, x.size() - i));

    const auto ms = juce::Time::getMillisecondCounterHiRes() - start;
    const double seconds = (double) x.size() / reader->sampleRate;

    std::cout << engine.amp.name << "\n";

    for (auto& line : engine.amp.report)
        std::cout << "   " << line << "\n";

    std::cout << "   built in " << juce::roundToInt (buildMs) << " ms, latency " << engine.latencySamples()
              << " samples, output level " << juce::Decibels::gainToDecibels (engine.makeup) << " dB\n"
              << "   " << seconds << " s of audio in " << ms / 1000.0 << " s = "
              << 100.0 * ms / 1000.0 / seconds << "% of one CPU core\n";

    juce::AudioBuffer<float> output (1, (int) x.size());

    for (size_t i = 0; i < x.size(); ++i)
        output.setSample (0, (int) i, (float) x[i]);

    const juce::File outFile (args[3]);
    outFile.deleteFile();
    juce::WavAudioFormat wav;
    std::unique_ptr<juce::OutputStream> stream = std::make_unique<juce::FileOutputStream> (outFile);
    auto writer = wav.createWriterFor (stream, juce::AudioFormatWriterOptions{}.withSampleRate (reader->sampleRate)
                                                                              .withNumChannels (1)
                                                                              .withBitsPerSample (32)
                                                                              .withSampleFormat (juce::AudioFormatWriterOptions::SampleFormat::floatingPoint));

    if (writer == nullptr)
        throw std::runtime_error ("can not write " + args[3].toStdString());

    writer->writeFromAudioSampleBuffer (output, 0, output.getNumSamples());
    return 0;
}

int main (int argc, char* argv[])
{
    juce::StringArray args;

    for (int i = 1; i < argc; ++i)
        args.add (argv[i]);

    try
    {
        if (args.size() >= 5 && args[0] == "core")
            return runPart (args, true);

        if (args.size() >= 5 && args[0] == "tail")
            return runPart (args, false);

        if (args.size() >= 4 && args[0] == "render")
            return render (args);
    }
    catch (const std::exception& e)
    {
        std::cerr << "error: " << e.what() << "\n";
        return 1;
    }

    std::cerr << "usage: EngineTest core|tail <preset.json> <in.f64> <out.f64> <rate>\n"
                 "       EngineTest render <preset.json> <in.wav> <out.wav> [factorLog2] [blockSize]\n";
    return 2;
}
