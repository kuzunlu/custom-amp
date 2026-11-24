#pragma once

#include <juce_core/juce_core.h>

#include <set>
#include <stdexcept>

// The amp engine. It was first written in Python and matched that version to -131 dB or better before Python was
// removed (2026-11-24); EngineTest check keeps it equal to theory, datasheets and its own reference recordings.
namespace amp
{

// A mistake in a preset. The message says what is wrong.
struct PresetError : std::runtime_error
{
    explicit PresetError (const juce::String& message) : std::runtime_error (message.toStdString()) {}
};

inline double dbToGain (double db) { return std::pow (10.0, db / 20.0); }

// One object from a preset (a block, a filter band, a split path), with helpers to read its settings.
class Cfg
{
public:
    Cfg (const juce::var& value, const juce::String& where);

    const juce::String& where() const { return place; }
    const juce::var& value() const { return object; }
    bool has (const char* name) const;
    juce::var get (const char* name) const;
    double number (const char* name, double fallback) const;
    juce::String text (const char* name, const juce::String& fallback) const;
    bool flag (const char* name, bool fallback) const;
    juce::Array<juce::var> list (const char* name) const;

    // Stop with a clear message when a preset has a typo in a setting name.
    void checkKeys (const std::set<juce::String>& allowed) const;

private:
    juce::var object;
    juce::String place;
};

void checkKeys (const juce::var& object, const std::set<juce::String>& allowed, const juce::String& where);

// What blocks need to know when they are built.
struct BuildContext
{
    double rate = 48000.0;       // the rate the blocks run at (the oversampled rate in the plugin)
    double checkRate = 48000.0;  // filter freqs must be below half of this (the plugin's normal rate)
    juce::File presetFolder;     // cab IR file paths are relative to this
    bool allowIr = false;        // only the end of the chain (run at the normal rate) can have ir blocks
    juce::File irOverride;       // when set, every ir block uses this file instead of its own
};

} // namespace amp
