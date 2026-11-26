#include "SignalView.h"

#include "Theme.h"

namespace
{
const double analysisSeconds = 0.171;  // what the spectrum looks at: 2^16 samples at 384 kHz (5.9 Hz per bin)
const int spansMs[] { 2, 5, 10, 20, 50, 100, 200, 500 };
const int stripHeight = 66, nameWidth = 200;

juce::String formatVolts (double v)
{
    v = std::abs (v);

    if (v >= 100) return juce::String (juce::roundToInt (v)) + " V";
    if (v >= 10)  return juce::String (v, 1) + " V";
    if (v >= 1)   return juce::String (v, 2) + " V";
    if (v >= 0.1) return juce::String (juce::roundToInt (v * 1000)) + " mV";
    if (v >= 0.001) return juce::String (v * 1000, 1) + " mV";
    return juce::String (v * 1e6, 1) + " uV";
}

// The blocks of a chain in the order the engine's flatten() lists them: a block, then the blocks inside it.
void listBlocks (const juce::var& list, int indent, std::vector<SignalView::Trace>& out)
{
    if (! list.isArray())
        return;

    for (int i = 0; i < list.size(); ++i)
    {
        const auto block = list[i];
        const auto type = block["type"].toString(), id = block["id"].toString();
        SignalView::Trace t;
        t.name = juce::String (i + 1) + ". " + type + (id.isNotEmpty() ? "  " + id : juce::String());
        t.target = { list, i, false };
        t.indent = indent;
        t.isUnit = type == "sag" || (type == "gate" && block["apply_after"].toString().isNotEmpty());
        out.push_back (t);

        if (type == "power" && (double) block.getProperty ("supply_ohms", 0.0) > 0)
        {
            // Its supply voltage (supply_ohms), as a share of plate_v: a strip of its own under it.
            t.name = "   supply of " + (id.isNotEmpty() ? id : type);
            t.isUnit = true;
            t.unitVolts = (double) block.getProperty ("plate_v", 450.0);
            out.push_back (t);
        }

        if (type == "feedback")
            listBlocks (block["chain"], indent + 1, out);

        if (type == "split" && block["paths"].isArray())
            for (auto& path : *block["paths"].getArray())
                listBlocks (path["chain"], indent + 1, out);
    }
}

// A plucked string: harmonics that die away, the higher ones faster (like the level riff).
double pluck (double f, double t)
{
    double v = 0.0;

    for (int k = 1; k <= 8; ++k)
        v += std::sin (juce::MathConstants<double>::twoPi * f * k * t) / k * std::exp (-t * (4.0 + 1.5 * k));

    return v;
}
}

//==============================================================================
// One strip: the name and what was measured on the left, the waveform, spectrum or curve on the right. With a kept
// "A" (Keep as A), A is drawn in orange behind the current signal (white), on the same scale.
class SignalView::Strip final : public juce::Component, public juce::SettableTooltipClient
{
public:
    Strip (SignalView& v, const Trace& t) : view (v), trace (t)
    {
        if (trace.target.isValid())
            setMouseCursor (juce::MouseCursor::PointingHandCursor);

        setTooltip (trace.target.isValid() ? "Click to edit this block." : juce::String());
    }

    void mouseUp (const juce::MouseEvent& e) override
    {
        if (e.mouseWasClicked() && trace.target.isValid() && view.onSelect != nullptr)
            view.onSelect (trace.target);
    }

    void paint (juce::Graphics& g) override
    {
        const bool selected = trace.target.isValid() && trace.target == view.selection;
        auto area = getLocalBounds().reduced (0, 2);
        g.setColour (selected ? theme::chipSelected : theme::panel);
        g.fillRect (area);

        const Trace* held = view.heldFor (trace);
        auto names = area.removeFromLeft (nameWidth).reduced (6, 4);
        names.removeFromLeft (trace.indent * 14);
        g.setColour (trace.target.isValid() ? theme::accent : juce::Colours::white);
        g.setFont (juce::FontOptions (13.0f, juce::Font::bold));
        g.drawText (trace.name, names.removeFromTop (18), juce::Justification::centredLeft, true);
        g.setFont (juce::FontOptions (11.5f));

        if (held != nullptr)
        {
            g.setColour (heldColour);
            g.drawText ("A: " + held->summary.upToFirstOccurrenceOf ("\n", false, false), names.removeFromBottom (14),
                        juce::Justification::centredLeft, true);
        }

        g.setColour (theme::muted);
        g.drawFittedText (trace.summary, names, juce::Justification::topLeft, held != nullptr ? 2 : 3, 1.0f);

        auto plot = area.reduced (4, 3).toFloat();
        g.setColour (theme::background);
        g.fillRect (plot);

        if (view.curveView)
            paintCurve (g, plot, held);
        else if (view.frequencyView)
            paintSpectrum (g, plot, held);
        else
            paintWave (g, plot, held);
    }

private:
    inline static const juce::Colour heldColour { 0xffe0a040 };  // A: the accent orange

    int spanCount (const Trace& t) const
    {
        return juce::jmin ((int) t.samples.size(), juce::roundToInt (view.spanBox.getSelectedId() * 0.001 * t.rate));
    }

    // Output against input over the time span: a clipping stage draws its curve, a filter an ellipse (its phase),
    // bias shift and sag make the line wander from one cycle to the next.
    void paintCurve (juce::Graphics& g, juce::Rectangle<float> plot, const Trace* held)
    {
        const auto pairs = [this] (const Trace& t) { return juce::jmin (spanCount (t), (int) t.inputs.size()); };

        if (pairs (trace) < 2)
        {
            g.setColour (theme::muted);
            g.setFont (juce::FontOptions (11.0f));
            g.drawText (trace.target.isValid() ? "(not a signal: see the time view)" : "(the input itself)", plot, juce::Justification::centred);
            return;
        }

        float inTop = 1e-9f, outTop = 1e-9f;

        for (const auto* t : { &trace, held })
            if (t != nullptr)
                for (int i = 0; i < pairs (*t); ++i)
                {
                    inTop = juce::jmax (inTop, std::abs (t->inputs[(size_t) i]));
                    outTop = juce::jmax (outTop, std::abs (t->samples[(size_t) i]));
                }

        inTop *= 1.1f;
        outTop *= 1.1f;
        auto box = plot.withWidth (juce::jmin (plot.getWidth(), plot.getHeight() * 3)).withCentre (plot.getCentre());
        const auto xOf = [&] (float v) { return box.getCentreX() + v / inTop * box.getWidth() * 0.5f; };
        const auto yOf = [&] (float v) { return box.getCentreY() - v / outTop * box.getHeight() * 0.5f; };

        g.setColour (theme::chip);
        g.drawHorizontalLine ((int) box.getCentreY(), box.getX(), box.getRight());
        g.drawVerticalLine ((int) box.getCentreX(), box.getY(), box.getBottom());

        for (const auto* t : { held, &trace })
        {
            const int count = t != nullptr ? pairs (*t) : 0;

            if (count < 2)
                continue;

            juce::Path path;
            const int step = juce::jmax (1, count / 4000);

            for (int i = 0; i < count; i += step)
            {
                const float x = xOf (t->inputs[(size_t) i]), y = yOf (t->samples[(size_t) i]);
                i == 0 ? path.startNewSubPath (x, y) : path.lineTo (x, y);
            }

            g.setColour (t == held ? heldColour : juce::Colours::white);
            g.strokePath (path, juce::PathStrokeType (1.0f));
        }

        drawScale (g, plot, "in +/- " + formatVolts (inTop) + "  ->  out +/- " + formatVolts (outTop));
    }

    void paintWave (juce::Graphics& g, juce::Rectangle<float> plot, const Trace* held)
    {
        float top = trace.isUnit ? 1.0f : juce::jmax (1e-9f, trace.peak, held != nullptr ? held->peak : 0.0f) * 1.1f;
        // A headroom, gain or supply strip shows from a round number under its lowest value up to 1, so a small sag shows.
        float bottom = 0.0f;

        if (trace.isUnit)
        {
            float lowest = 1.0f;

            for (const auto* t : { &trace, held })
                if (t != nullptr && spanCount (*t) > 0)
                    lowest = juce::jmin (lowest, *std::min_element (t->samples.begin(), t->samples.begin() + spanCount (*t)));

            bottom = juce::jlimit (0.0f, 0.9f, std::floor ((lowest - 0.02f) * 10.0f) / 10.0f);
        }

        const auto yOf = [&] (float v)
        {
            return trace.isUnit ? plot.getBottom() - (v - bottom) / (top - bottom) * plot.getHeight()
                                : plot.getCentreY() - v / top * plot.getHeight() * 0.5f;
        };

        g.setColour (theme::chip);
        g.drawHorizontalLine ((int) yOf (0.0f), plot.getX(), plot.getRight());

        // Fewer samples than pixels: a line through them. More: the lowest and highest value in each pixel column.
        for (const auto* t : { held, &trace })
        {
            const int count = t != nullptr ? spanCount (*t) : 0;

            if (count < 2)
                continue;

            juce::Path path;
            const int width = (int) plot.getWidth();
            const auto colour = t == held ? heldColour : juce::Colours::white;

            if (count <= 2 * width)
            {
                for (int i = 0; i < count; ++i)
                {
                    const float x = plot.getX() + plot.getWidth() * (float) i / (float) (count - 1);
                    i == 0 ? path.startNewSubPath (x, yOf (t->samples[0])) : path.lineTo (x, yOf (t->samples[(size_t) i]));
                }

                g.setColour (colour);
                g.strokePath (path, juce::PathStrokeType (1.2f));
                continue;
            }

            std::vector<float> lo ((size_t) width), hi ((size_t) width);

            for (int c = 0; c < width; ++c)
            {
                const int from = (int) ((juce::int64) c * count / width), to = juce::jmax (from + 1, (int) ((juce::int64) (c + 1) * count / width));
                const auto [mn, mx] = std::minmax_element (t->samples.begin() + from, t->samples.begin() + to);
                lo[(size_t) c] = *mn;
                hi[(size_t) c] = *mx;
            }

            for (int c = 0; c < width; ++c)
                c == 0 ? path.startNewSubPath (plot.getX(), yOf (hi[0])) : path.lineTo (plot.getX() + (float) c, yOf (hi[(size_t) c]));

            for (int c = width - 1; c >= 0; --c)
                path.lineTo (plot.getX() + (float) c + 1.0f, yOf (lo[(size_t) c]));

            path.closeSubPath();
            g.setColour (t == held ? colour.withAlpha (0.35f) : colour);
            g.fillPath (path);
            g.setColour (colour);
            g.strokePath (path, juce::PathStrokeType (1.0f));
        }

        drawScale (g, plot, ! trace.isUnit ? "+/- " + formatVolts (top)
                            : trace.unitVolts > 0 ? juce::String (juce::roundToInt (bottom * trace.unitVolts)) + " to "
                                                        + juce::String (juce::roundToInt (trace.unitVolts)) + " V"
                                                  : juce::String (bottom, 1) + " to 1");
    }

    // The scale in the top right corner, on its own background so the trace does not cover it.
    static void drawScale (juce::Graphics& g, juce::Rectangle<float> plot, const juce::String& text)
    {
        const juce::FontOptions font (11.0f);
        const float width = (float) juce::GlyphArrangement::getStringWidthInt (juce::Font (font), text) + 8.0f;
        const juce::Rectangle<float> box (plot.getRight() - width - 2.0f, plot.getY() + 2.0f, width, 14.0f);
        g.setColour (theme::background.withAlpha (0.85f));
        g.fillRect (box);
        g.setColour (theme::muted);
        g.setFont (font);
        g.drawText (text, box, juce::Justification::centred);
    }

    void paintSpectrum (juce::Graphics& g, juce::Rectangle<float> plot, const Trace* held)
    {
        g.setFont (juce::FontOptions (11.0f));

        if (trace.spectrumDb.empty())
        {
            g.setColour (theme::muted);
            g.drawText (trace.isUnit ? "(a headroom or gain: see the time view)" : "(too short for a spectrum)",
                        plot, juce::Justification::centred);
            return;
        }

        if (held != nullptr && held->spectrumDb.empty())
            held = nullptr;

        const double loHz = 20.0, hiHz = juce::jmin (20000.0, trace.binHz * (double) (trace.spectrumDb.size() - 1));
        float highest = *std::max_element (trace.spectrumDb.begin(), trace.spectrumDb.end());

        if (held != nullptr)
            highest = juce::jmax (highest, *std::max_element (held->spectrumDb.begin(), held->spectrumDb.end()));

        const float topDb = 20.0f * std::ceil (highest / 20.0f);
        const float rangeDb = 100.0f;
        const auto xOf = [&] (double hz) { return plot.getX() + plot.getWidth() * (float) (std::log (hz / loHz) / std::log (hiHz / loHz)); };
        const auto yOf = [&] (float db) { return plot.getY() + plot.getHeight() * juce::jlimit (0.0f, 1.0f, (topDb - db) / rangeDb); };

        g.setColour (theme::chip);

        for (float db = topDb; db > topDb - rangeDb; db -= 20.0f)
            g.drawHorizontalLine ((int) yOf (db), plot.getX(), plot.getRight());

        for (double hz : { 100.0, 1000.0, 10000.0 })
            if (hz < hiHz)
                g.drawVerticalLine ((int) xOf (hz), plot.getY(), plot.getBottom());

        // Each pixel column: the highest bin in it, or (where bins are wider than a pixel) the bin under it.
        for (const auto* t : { held, &trace })
        {
            if (t == nullptr)
                continue;

            juce::Path path;
            const int width = (int) plot.getWidth();
            const auto& spectrum = t->spectrumDb;

            for (int c = 0; c < width; ++c)
            {
                const double f0 = loHz * std::pow (hiHz / loHz, (double) c / width);
                const double f1 = loHz * std::pow (hiHz / loHz, (double) (c + 1) / width);
                const int b0 = (int) std::ceil (f0 / t->binHz), b1 = (int) std::floor (f1 / t->binHz);
                float db = spectrum[(size_t) juce::jlimit (0, (int) spectrum.size() - 1, juce::roundToInt (f0 / t->binHz))];

                for (int b = b0; b <= b1 && b < (int) spectrum.size(); ++b)
                    db = juce::jmax (db, spectrum[(size_t) b]);

                const float x = plot.getX() + (float) c, y = yOf (db);
                c == 0 ? path.startNewSubPath (x, y) : path.lineTo (x, y);
            }

            g.setColour (t == held ? heldColour : juce::Colours::white);
            g.strokePath (path, juce::PathStrokeType (1.2f));
        }

        drawScale (g, plot, "top line " + formatVolts (std::pow (10.0, topDb / 20.0)) + ", lines 20 dB apart");
        g.setColour (theme::muted);

        for (auto [hz, text] : { std::pair { 100.0, "100" }, std::pair { 1000.0, "1k" }, std::pair { 10000.0, "10k" } })
            if (hz < hiHz)
                g.drawText (text, juce::Rectangle<float> (xOf (hz) + 2, plot.getBottom() - 14, 30, 12), juce::Justification::left);
    }

    SignalView& view;
    const Trace& trace;
};

// The strips, one under the other (inside the viewport).
class SignalView::Strips final : public juce::Component
{
public:
    void layout (int width)
    {
        for (int i = 0; i < (int) strips.size(); ++i)
            strips[(size_t) i]->setBounds (0, i * stripHeight, width, stripHeight);

        setSize (width, juce::jmax (1, (int) strips.size() * stripHeight));
    }

    std::vector<std::unique_ptr<Strip>> strips;
};

//==============================================================================
SignalView::SignalView (CustomAmpProcessor& p) : processor (p)
{
    signalBox.addItemList ({ "Sine wave", "Plucked note", "Power chord (root, fifth, octave)", "Loud, then soft (sag, bias shift)",
                             "Your playing (the last 0.6 s)" }, 1);
    signalBox.setSelectedId (1, juce::dontSendNotification);
    signalBox.setTooltip ("The test signal. Only this view hears it: its own copy of the amp plays it, not the sound you hear.");
    signalBox.onChange = [this] { signalChanged(); };
    addAndMakeVisible (signalBox);

    for (auto ms : spansMs)
        spanBox.addItem (juce::String (ms) + " ms", ms);

    spanBox.setSelectedId (20, juce::dontSendNotification);
    spanBox.setTooltip ("How much time the waveforms show.");
    spanBox.onChange = [this] { requestRun(); };
    addAndMakeVisible (spanBox);

    for (auto* b : { &timeButton, &frequencyButton, &curveButton })
    {
        b->setClickingTogglesState (true);
        b->setRadioGroupId (1);
        b->setColour (juce::TextButton::buttonOnColourId, theme::chipSelected);
        addAndMakeVisible (*b);
    }

    timeButton.setToggleState (true, juce::dontSendNotification);
    timeButton.setTooltip ("Waveforms: the signal over time after each block.");
    frequencyButton.setTooltip ("Spectra: which frequencies come out of each block, and how loud. For a sine, the summary "
                                "lists the harmonics each block adds (H2 = twice the frequency, and so on).");
    curveButton.setTooltip ("Curves: each block's output against its input over the time span. A clipping stage draws its "
                            "clipping curve, a filter an ellipse (it shifts the timing), and bias shift or sag make the line "
                            "wander from one cycle to the next.");
    timeButton.onClick = [this] { setFrequencyView (false); };
    frequencyButton.onClick = [this] { setFrequencyView (true); };
    curveButton.onClick = [this] { setCurveView (true); };

    keepButton.setClickingTogglesState (true);
    keepButton.setColour (juce::TextButton::buttonOnColourId, theme::chipSelected);
    keepButton.setTooltip ("A/B: keep what every strip shows now as 'A' (orange). Then change a setting: each strip shows "
                           "the new signal (white) on top of A, on the same scale, and A's numbers under its own. Click again to drop A.");
    keepButton.onClick = [this] { keepAsA (keepButton.getToggleState()); };
    addAndMakeVisible (keepButton);

    freqSlider.setRange (20.0, 5000.0, 1.0);
    freqSlider.setSkewFactorFromMidPoint (300.0);
    freqSlider.setValue (220.0, juce::dontSendNotification);
    freqSlider.setTextValueSuffix (" Hz");
    freqSlider.setTooltip ("The note's frequency (for a chord: its lowest note). Low E on a guitar is 82 Hz, the A string 110 Hz.");
    levelSlider.setRange (0.005, 100.0, 0.001);  // up to 100 V: a power block alone needs tens of volts at its grids
    levelSlider.setSkewFactorFromMidPoint (0.5);
    levelSlider.setValue (0.5, juce::dontSendNotification);
    levelSlider.setTextValueSuffix (" V");
    levelSlider.setTooltip ("The peak of the test signal in volts, at the amp's input. A guitar pickup gives about 0.1 V "
                            "played softly and 0.5 V or more played hard. A power block on its own needs tens of volts "
                            "(its grids swing about as far as its bias voltage).");

    for (auto* s : { &freqSlider, &levelSlider })
    {
        s->setSliderStyle (juce::Slider::LinearHorizontal);
        s->setTextBoxStyle (juce::Slider::TextBoxLeft, false, 76, 20);
        s->onValueChange = [this] { requestRun(); };
        addAndMakeVisible (*s);
    }

    freqLabel.setText ("Note", juce::dontSendNotification);
    levelLabel.setText ("Peak", juce::dontSendNotification);
    addAndMakeVisible (freqLabel);
    addAndMakeVisible (levelLabel);

    infoLabel.setColour (juce::Label::textColourId, theme::muted);
    infoLabel.setFont (juce::FontOptions (12.0f));
    addAndMakeVisible (infoLabel);

    strips = std::make_unique<Strips>();
    viewport.setViewedComponent (strips.get(), false);
    viewport.setScrollBarsShown (true, false);
    addAndMakeVisible (viewport);

    runner.startThread();
}

SignalView::~SignalView()
{
    runner.signalThreadShouldExit();
    runner.request ({});  // wakes it up
    runner.stopThread (5000);
    cancelPendingUpdate();
}

void SignalView::setPreset (const juce::var& p, const juce::String& text)
{
    preset = p;
    presetText = text;
    requestRun();
}

void SignalView::setSelection (const Selection& s)
{
    selection = s;
    repaint();

    for (auto& strip : strips->strips)
        strip->repaint();
}

void SignalView::setSignal (Signal s, double freq, double peakV)
{
    freqSlider.setValue (freq, juce::dontSendNotification);
    levelSlider.setValue (peakV, juce::dontSendNotification);
    signalBox.setSelectedId ((int) s + 1, juce::dontSendNotification);
    signalChanged();
}

void SignalView::setFrequencyView (bool frequency)
{
    frequencyView = frequency;
    curveView = false;
    (frequency ? frequencyButton : timeButton).setToggleState (true, juce::dontSendNotification);
    spanBox.setEnabled (! frequency);

    for (auto& strip : strips->strips)
        strip->repaint();
}

void SignalView::keepAsA (bool keep)
{
    held = keep ? traces : std::vector<Trace> {};
    keepButton.setToggleState (keep, juce::dontSendNotification);

    for (auto& strip : strips->strips)
        strip->repaint();
}

const SignalView::Trace* SignalView::heldFor (const Trace& t) const
{
    if (held.empty())
        return nullptr;

    // The block's name without its number ("3. gain  v1b" -> "gain  v1b"), and the how-many-th of that name.
    const auto key = [] (const Trace& x) { return x.name.fromFirstOccurrenceOf (". ", false, false).trim() + "|" + juce::String (x.indent); };
    const auto nth = [&key] (const std::vector<Trace>& list, const Trace* x)
    {
        int count = 0;

        for (auto& y : list)
        {
            if (&y == x)
                return count;

            count += key (y) == key (*x) ? 1 : 0;
        }

        return -1;
    };

    const int wanted = nth (traces, &t);
    int count = 0;

    for (auto& h : held)
        if (key (h) == key (t) && h.isUnit == t.isUnit)
            if (count++ == wanted)
                return &h;

    return nullptr;
}

void SignalView::setCurveView (bool curve)
{
    curveView = curve;
    frequencyView = false;
    (curve ? curveButton : timeButton).setToggleState (true, juce::dontSendNotification);
    spanBox.setEnabled (true);  // the curve is drawn over the time span

    for (auto& strip : strips->strips)
        strip->repaint();
}

void SignalView::setTimeSpanMs (int ms)
{
    spanBox.setSelectedId (ms, juce::sendNotificationSync);
}

void SignalView::signalChanged()
{
    // Each signal starts with the time span that shows it best (a few cycles of a sine, the attack of a pluck).
    const auto s = (Signal) (signalBox.getSelectedId() - 1);
    const int span = s == Signal::sine ? 20 : s == Signal::pluck || s == Signal::chord ? 50 : 500;
    spanBox.setSelectedId (span, juce::dontSendNotification);
    freqSlider.setEnabled (s != Signal::playing);
    levelSlider.setEnabled (s != Signal::playing);
    requestRun();
}

bool SignalView::isWaiting() const
{
    return shownGeneration != generation || dirty;
}

void SignalView::visibilityChanged()
{
    if (isVisible() && dirty)
        requestRun();
}

void SignalView::requestRun()
{
    if (! isVisible() || presetText.isEmpty())
    {
        dirty = true;  // run when it is shown
        return;
    }

    dirty = false;
    Request r;
    r.text = presetText;
    r.options = processor.getSignalOptions();
    r.signal = (Signal) (signalBox.getSelectedId() - 1);
    r.freq = freqSlider.getValue();
    r.peakV = levelSlider.getValue();
    r.spanSeconds = spanBox.getSelectedId() * 0.001;
    r.generation = ++generation;

    if (r.signal == Signal::playing)
        r.playing = processor.getRecentInput (0.6);

    infoLabel.setText ("Playing the test signal through the amp...", juce::dontSendNotification);
    runner.request (std::move (r));
}

void SignalView::Runner::request (Request r)
{
    {
        const std::lock_guard<std::mutex> l (lock);
        next = std::move (r);
    }

    wakeUp.signal();
}

void SignalView::Runner::run()
{
    while (! threadShouldExit())
    {
        wakeUp.wait (500);
        std::optional<Request> r;

        {
            const std::lock_guard<std::mutex> l (lock);
            std::swap (r, next);
        }

        if (! r.has_value() || threadShouldExit() || r->text.isEmpty())
            continue;

        auto out = compute (*r);

        {
            const std::lock_guard<std::mutex> l (owner.resultLock);
            owner.result = std::move (out);
        }

        owner.triggerAsyncUpdate();
    }
}

SignalView::Result SignalView::compute (const Request& r)
{
    Result out;
    out.generation = r.generation;
    out.text = r.text;

    try
    {
        juce::var parsed;

        if (juce::JSON::parse (r.text, parsed).failed() || ! parsed.isObject())
            throw amp::PresetError ("the preset is not valid JSON");

        auto options = r.options;
        options.preset = parsed;
        amp::Engine engine (options);
        const double rate = options.sampleRate;
        const double recordSeconds = juce::jmax (r.spanSeconds, analysisSeconds);
        const int recordCount = (int) std::ceil (recordSeconds * rate);

        // The test signal: a run-up (so a sine has settled, or silence so a note starts from rest), then the part recorded.
        std::vector<double> x;
        const double twoPi = juce::MathConstants<double>::twoPi;

        if (r.signal == Signal::playing)
        {
            x = r.playing;
        }
        else
        {
            const int preCount = (int) ((r.signal == Signal::sine ? 0.5 : 0.1) * rate);
            x.assign ((size_t) (preCount + recordCount), 0.0);

            for (size_t i = 0; i < x.size(); ++i)
            {
                const double t = (double) i / rate, tn = t - (double) preCount / rate;  // tn = time since the note started

                switch (r.signal)
                {
                    case Signal::sine:     x[i] = std::sin (twoPi * r.freq * t) * juce::jmin (1.0, t / 0.02); break;
                    case Signal::pluck:    x[i] = tn >= 0 ? pluck (r.freq, tn) : 0.0; break;
                    case Signal::chord:    x[i] = tn >= 0 ? pluck (r.freq, tn) + pluck (r.freq * 1.5, tn) + pluck (r.freq * 2, tn) : 0.0; break;
                    case Signal::loudSoft: x[i] = tn >= 0 ? std::sin (twoPi * r.freq * tn) * (tn < 0.2 ? 1.0 : 0.25) : 0.0; break;
                    case Signal::playing:  break;
                }
            }

            double biggest = 1e-12;

            for (auto v : x)
                biggest = std::max (biggest, std::abs (v));

            for (auto& v : x)
                v *= r.peakV / biggest;
        }

        const int start = juce::jmax (0, (int) x.size() - recordCount);

        std::vector<Trace> traces (1);
        traces[0].name = "Input";
        listBlocks (parsed["chain"], 0, traces);
        const auto core = amp::flatten (engine.amp.core), tail = amp::flatten (engine.amp.tail);
        size_t supplies = 0;

        for (auto& t : traces)
            supplies += t.unitVolts > 0 ? 1 : 0;

        const size_t rooms = engine.amp.room != nullptr ? 1 : 0;

        if (traces.size() != 1 + core.size() + tail.size() + supplies + rooms)
            throw amp::PresetError ("the signal view could not match the blocks of this preset");

        const auto play = [&engine, &x] (int from, int to)
        {
            std::vector<double> chunk ((size_t) engine.maxBlock());

            for (int i = from; i < to; i += engine.maxBlock())
            {
                const int n = juce::jmin (engine.maxBlock(), to - i);
                std::copy (x.begin() + i, x.begin() + i + n, chunk.begin());
                engine.process (chunk.data(), n);
            }
        };

        play (0, start);
        std::vector<amp::Block*> blocks (core.begin(), core.end());
        blocks.insert (blocks.end(), tail.begin(), tail.end());

        if (engine.amp.room != nullptr)
            blocks.push_back (engine.amp.room.get());  // its strip: the string's motion it adds to the input

        for (size_t k = 0, ti = 1; k < blocks.size(); ++k, ++ti)
        {
            const auto reserve = (size_t) ((double) (x.size() - (size_t) start) * engine.coreRate() / rate) + 4096;
            auto& t = traces[ti];
            t.rate = k < core.size() ? engine.coreRate() : rate;
            t.samples.reserve (reserve);
            blocks[k]->probe = &t.samples;

            if (! t.isUnit && dynamic_cast<amp::Room*> (blocks[k]) == nullptr)
            {
                t.inputs.reserve (reserve);
                blocks[k]->probeIn = &t.inputs;  // for the curve view (a headroom, gain or string motion has no input to plot it against)
            }

            if (ti + 1 < traces.size() && traces[ti + 1].unitVolts > 0)
                if (auto* power = dynamic_cast<amp::Power*> (blocks[k]))
                {
                    auto& s = traces[++ti];
                    s.rate = t.rate;
                    s.samples.reserve (reserve);
                    power->supplyProbe = &s.samples;
                }
        }

        traces[0].rate = rate;
        for (size_t i = (size_t) start; i < x.size(); ++i)
            traces[0].samples.push_back ((float) x[i]);
        play (start, (int) x.size());

        for (auto* b : blocks)
        {
            b->probe = b->probeIn = nullptr;

            if (auto* power = dynamic_cast<amp::Power*> (b))
                power->supplyProbe = nullptr;
        }

        // What each strip says about itself: the peak, and for a sine the harmonics (from the spectrum).
        for (auto& t : traces)
        {
            const int spanCount = juce::jmin ((int) t.samples.size(), juce::roundToInt (r.spanSeconds * t.rate));
            float lo = 0.0f, hi = 0.0f;

            for (int i = 0; i < spanCount; ++i)
            {
                lo = juce::jmin (lo, t.samples[(size_t) i]);
                hi = juce::jmax (hi, t.samples[(size_t) i]);
                t.peak = juce::jmax (t.peak, std::abs (t.samples[(size_t) i]));
            }

            if (t.isUnit)
            {
                float mn = 1.0f, mx = 0.0f;

                for (int i = 0; i < spanCount; ++i)
                {
                    mn = juce::jmin (mn, t.samples[(size_t) i]);
                    mx = juce::jmax (mx, t.samples[(size_t) i]);
                }

                if (t.unitVolts > 0)
                    t.summary = "B+ " + juce::String (juce::roundToInt (mn * t.unitVolts)) + " to " + juce::String (juce::roundToInt (mx * t.unitVolts))
                                + " V (plate_v at idle: " + juce::String (juce::roundToInt (t.unitVolts)) + " V)";
                else
                    t.summary = (t.name.contains ("sag") ? "headroom " : "gain ") + juce::String (mn, 2) + " to " + juce::String (mx, 2);

                continue;
            }

            const int available = juce::jmin ((int) t.samples.size(), (int) (analysisSeconds * t.rate));
            int order = 0;

            while ((2 << order) <= available)
                ++order;

            t.summary = "peak " + formatVolts (t.peak);

            if (order < 8)
                continue;

            const int size = 1 << order;
            juce::dsp::FFT fft (order);
            std::vector<float> data ((size_t) (2 * size), 0.0f);
            double windowSum = 0.0;

            for (int i = 0; i < size; ++i)
            {
                // Blackman-Harris window: its leakage is 92 dB down, so quiet harmonics next to a loud one show.
                const double a = twoPi * i / (size - 1);
                const double w = 0.35875 - 0.48829 * std::cos (a) + 0.14128 * std::cos (2 * a) - 0.01168 * std::cos (3 * a);
                data[(size_t) i] = (float) (t.samples[(size_t) i] * w);
                windowSum += w;
            }

            fft.performFrequencyOnlyForwardTransform (data.data(), true);
            t.binHz = t.rate / size;
            const int bins = juce::jmin (size / 2, (int) (20000.0 / t.binHz) + 2);
            t.spectrumDb.resize ((size_t) bins);

            for (int b = 0; b < bins; ++b)
                t.spectrumDb[(size_t) b] = (float) (20.0 * std::log10 (2.0 * data[(size_t) b] / windowSum + 1e-12));

            if (r.signal != Signal::sine)
                continue;

            // The level of each harmonic: the power of the bins around k times the note, added up. (The highest
            // bin alone depends on where the note falls between two bins: up to 0.8 dB off with this window.)
            const auto harmonic = [&t, bins, &r] (int k)
            {
                const int centre = juce::roundToInt (k * r.freq / t.binHz);
                double power = 0.0;

                for (int b = juce::jmax (0, centre - 5); b <= juce::jmin (bins - 1, centre + 5); ++b)
                    power += std::pow (10.0, t.spectrumDb[(size_t) b] / 10.0);

                return std::sqrt (power);
            };

            const double first = harmonic (1);
            double sum = 0.0;
            juce::String list;

            for (int k = 2; k <= 10 && k * r.freq < juce::jmin (20000.0, t.binHz * (bins - 4)); ++k)
            {
                const double a = harmonic (k);
                sum += a * a;

                if (k <= 4)
                    list << "  H" << k << " " << juce::roundToInt (20.0 * std::log10 (a / first + 1e-12));
            }

            if (first > 0.0)
                t.summary << "   THD " << juce::String (100.0 * std::sqrt (sum) / first, 1) << "%\n" << list.trim() << " dB";
        }

        out.traces = std::move (traces);
    }
    catch (const std::exception& e)
    {
        out.problem = e.what();
    }

    return out;
}

void SignalView::handleAsyncUpdate()
{
    std::optional<Result> r;

    {
        const std::lock_guard<std::mutex> l (resultLock);
        std::swap (r, result);
    }

    if (! r.has_value() || r->generation < shownGeneration)
        return;

    shownGeneration = r->generation;
    problem = r->problem;
    traces = std::move (r->traces);

    // Clicking a strip selects its block in the editor's preset (the runner had its own copy).
    if (r->text == presetText)
    {
        std::vector<Trace> places (1);
        listBlocks (preset["chain"], 0, places);

        for (size_t i = 0; i < traces.size() && i < places.size(); ++i)
            traces[i].target = places[i].target;
    }

    infoLabel.setText (problem.isNotEmpty() ? "Can not play it: " + problem
                                            : "Its own copy of the amp plays this (you do not hear it). Click a strip to edit that block.",
                       juce::dontSendNotification);
    infoLabel.setColour (juce::Label::textColourId, problem.isNotEmpty() ? theme::error : theme::muted);
    rebuildStrips();
}

void SignalView::rebuildStrips()
{
    strips->strips.clear();

    for (auto& t : traces)
    {
        strips->strips.push_back (std::make_unique<Strip> (*this, t));
        strips->addAndMakeVisible (*strips->strips.back());
    }

    strips->layout (juce::jmax (100, viewport.getWidth() - viewport.getScrollBarThickness()));
}

void SignalView::paint (juce::Graphics& g)
{
    g.fillAll (theme::background);
}

void SignalView::resized()
{
    auto area = getLocalBounds();
    auto row = area.removeFromTop (26);
    signalBox.setBounds (row.removeFromLeft (juce::jmin (250, row.getWidth() / 2)));
    row.removeFromLeft (6);
    timeButton.setBounds (row.removeFromLeft (60));
    frequencyButton.setBounds (row.removeFromLeft (84));
    curveButton.setBounds (row.removeFromLeft (60));
    row.removeFromLeft (6);
    keepButton.setBounds (row.removeFromRight (juce::jmin (90, row.getWidth())));
    row.removeFromLeft (6);
    spanBox.setBounds (row.removeFromLeft (80));

    area.removeFromTop (4);
    row = area.removeFromTop (24);
    const int half = row.getWidth() / 2;
    auto left = row.removeFromLeft (half), right = row;
    freqLabel.setBounds (left.removeFromLeft (40));
    freqSlider.setBounds (left.reduced (0, 1));
    levelLabel.setBounds (right.removeFromLeft (40));
    levelSlider.setBounds (right.reduced (0, 1));

    infoLabel.setBounds (area.removeFromTop (20));
    viewport.setBounds (area);
    strips->layout (juce::jmax (100, viewport.getWidth() - viewport.getScrollBarThickness()));
}
