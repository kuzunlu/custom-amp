#pragma once

#include "Common.h"

#include <vector>

namespace amp
{

// Stands in for the player (it is NOT physics): follows the note being played and notices a hand on the strings.
// Used by the room and restring blocks, which need to know what the guitarist is doing.
//   The note: YIN (de Cheveigne and Kawahara 2002) on the input at about 12 kHz, every 10 ms, 60 Hz to 1 kHz, one note
//   (with a chord, the strongest). d(lag) = how much the signal differs from itself `lag` later; the first lag where d
//   (divided by its running average) falls under 0.15 is the period.
//   Muted: the input falls to a tenth within about 10 ms (a hand on the strings), or stays below muteLevel.
//   A pick attack: the level (a peak follower that falls in 100 ms) jumps to more than twice where it was about 20 ms
//   before (and above muteLevel). So a note picked again while it still rings louder than half counts as no new attack.
class Player
{
public:
    // window: how many samples (at about 12 kHz) YIN compares; shorter finds a new note sooner, longer is steadier.
    // checkMs: how often it looks. recent: compare the newest samples with older ones (finds a new note after about
    // window + one period); otherwise the oldest part of its memory with newer ones (the room's tracker: steadier).
    void prepare (double rate, double muteLevel, int window = 512, double checkMs = 10, bool recent = false);
    void reset() noexcept;

    // One input sample. True when it found the note (again): every 10 ms while a note is playing.
    bool hear (double x) noexcept;

    double period() const noexcept { return notePeriod; }  // samples at the input rate; 0 = no note yet
    bool isMuted() const noexcept { return muted; }
    bool isStopped() const noexcept { return fastLevel < 0.1 * slowLevel; }  // the hand-on-the-strings part of muted
    bool attacked() const noexcept { return attack; }                         // a pick attack at this sample

private:
    bool track() noexcept;

    double rate = 48000, muteLevel = 0.001, notePeriod = 0;
    double fastLevel = 0, slowLevel = 0, fastFall = 0, slowFall = 0, base = 0, baseRate = 0;
    bool muted = true, armed = true, attack = false, recent = false;
    int window = 512, decimate = 4, decimCount = 0, sinceCheck = 0, checkEvery = 120;
    double decimSum = 0;
    std::vector<double> low, diff, flat;
    int lowWrite = 0;
};

} // namespace amp
