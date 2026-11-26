#include "MeterView.h"
#include "Theme.h"

MeterView::MeterView (CustomAmpProcessor& p) : processor (p)
{
    setTooltip ("In / Out: the level the amp gets (after Input) and the level that leaves the plugin (0 dB = full scale; "
                "here 1 V at the amp's input, a guitar played hard is about -6 dB). The red light: a sample reached full "
                "scale in the last 2 s (at In: the audio interface's input clipped). Click to clear.\n"
                "Tuner: the note at the amp's input and how many cents off it is (green within 3 cents).\n"
                "CPU: how much of each audio block's time the plugin needs.");
    startTimerHz (30);
}

MeterView::~MeterView()
{
    processor.meters.tunerOn = false;
}

void MeterView::visibilityChanged()
{
    processor.meters.tunerOn = isShowing();
}

juce::String MeterView::noteName (double frequency, double& cents)
{
    static const char* names[] = { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };
    const double midi = 69 + 12 * std::log2 (frequency / 440.0);
    const int nearest = juce::roundToInt (midi);
    cents = 100 * (midi - nearest);
    return juce::String (names[((nearest % 12) + 12) % 12]) + juce::String (nearest / 12 - 1);
}

void MeterView::timerCallback()
{
    auto& m = processor.meters;
    processor.meters.tunerOn = isShowing();

    // Peaks: jump up at once, fall 20 dB per second (so a short peak stays visible).
    const auto db = [] (float peak) { return peak > 1e-5f ? 20 * std::log10 (peak) : -100.0f; };
    const float fall = 20.0f / 30;
    inDb = juce::jmax (db (m.inPeak.exchange (0)), inDb - fall);
    outDb = juce::jmax (db (m.outPeak.exchange (0)), outDb - fall);
    const auto now = juce::Time::getMillisecondCounter();

    if (m.inClip.exchange (false))
        inClipUntil = now + 2000;

    if (m.outClip.exchange (false))
        outClipUntil = now + 2000;

    cpu = m.cpu.load();
    hz = m.noteHz.load();
    repaint();
}

void MeterView::mouseDown (const juce::MouseEvent&)
{
    inClipUntil = outClipUntil = 0;
    repaint();
}

void MeterView::paint (juce::Graphics& g)
{
    auto area = getLocalBounds();
    g.setColour (theme::panel);
    g.fillRoundedRectangle (area.toFloat(), 4);
    area.reduce (6, 4);
    const auto now = juce::Time::getMillisecondCounter();

    // Two level bars, -60..0 dB, a clip light above each.
    auto bars = area.removeFromLeft (52);
    g.setFont (juce::FontOptions (11.0f));

    for (auto [name, level, clipUntil] : { std::tuple { "In", inDb, inClipUntil }, std::tuple { "Out", outDb, outClipUntil } })
    {
        auto column = bars.removeFromLeft (26);
        g.setColour (theme::muted);
        g.drawText (name, column.removeFromBottom (14), juce::Justification::centred);
        auto light = column.removeFromTop (8).reduced (6, 0);
        g.setColour (now < clipUntil ? theme::error : juce::Colour (0xff3a2a2a));
        g.fillRect (light);
        column.removeFromTop (3);
        auto bar = column.reduced (7, 0);
        g.setColour (juce::Colour (0xff15171a));
        g.fillRect (bar);
        const float share = juce::jlimit (0.0f, 1.0f, (level + 60) / 60);
        const auto filled = bar.withTop (bar.getBottom() - juce::roundToInt (share * (float) bar.getHeight()));
        g.setColour (level > -3 ? theme::error : level > -12 ? theme::accent : juce::Colour (0xff6fbf73));
        g.fillRect (filled);
    }

    area.removeFromLeft (6);

    // The tuner: the note, its cents, a needle on -50..50 cents.
    auto cpuLine = area.removeFromBottom (14);
    g.setColour (theme::muted);
    g.drawText ("CPU " + juce::String (100 * cpu, 1) + "%", cpuLine, juce::Justification::centredRight);

    if (hz > 0)
    {
        double cents = 0;
        const auto name = noteName (hz, cents);
        const bool inTune = std::abs (cents) < 3;
        g.setColour (inTune ? juce::Colour (0xff6fbf73) : juce::Colour (0xfff2efe8));
        g.setFont (juce::FontOptions (24.0f, juce::Font::bold));
        auto top = area.removeFromTop (30);
        g.drawText (name, top, juce::Justification::centredLeft);
        g.setFont (juce::FontOptions (12.0f));
        g.setColour (theme::muted);
        g.drawText ((cents >= 0 ? "+" : "") + juce::String (cents, 0) + " c  " + juce::String (hz, 1) + " Hz", top, juce::Justification::centredRight);
        auto scale = area.removeFromTop (14).toFloat();
        g.setColour (juce::Colour (0xff15171a));
        g.fillRect (scale);
        g.setColour (theme::muted);
        g.fillRect (scale.getCentreX() - 0.5f, scale.getY(), 1.0f, scale.getHeight());
        g.setColour (inTune ? juce::Colour (0xff6fbf73) : theme::accent);
        const float x = scale.getCentreX() + (float) (cents / 50) * scale.getWidth() / 2;
        g.fillRect (x - 2, scale.getY(), 4.0f, scale.getHeight());
    }
    else
    {
        g.setColour (theme::muted);
        g.setFont (juce::FontOptions (12.0f));
        g.drawText ("Tuner: play a note", area.removeFromTop (30), juce::Justification::centredLeft);
    }
}
