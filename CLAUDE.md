# custom-amp

A guitar amp built from a few blocks (EQ, gain stage, sag, feedback, cab IR) that can go in any order,
so it can copy any amp. The idea: every amp is EQ and clipping, plus power supply sag and a feedback loop.

**Final product:** a C++ JUCE plugin (VST3 + standalone app) for live playing.
**Now:** a Python prototype that processes `.wav` files offline, to get the sound design right first.
The Python code will stay as the reference: the plugin must give the same output for the same preset.

## Commands

Use `C:\Python314\python.exe`. Only NumPy is used (no SciPy; filters and resampling are written by hand).

```
python tools/make_test_audio.py                                       # fake DI riff + fake cab IR into audio/
python run.py presets/crunch.json audio/test_di.wav out/crunch.wav    # run a preset
```

`.wav` files are not in git (`.gitignore`). `audio/` test files can always be made again.

## Layout

- `ampsim/blocks.py`: all blocks (`EQ`, `Gain`, `Sag`, `Feedback`, `IRLoader`) and the clipping curves
- `ampsim/chain.py`: builds an amp from a preset, links sag blocks to their targets
- `ampsim/filters.py`: biquad filters (RBJ cookbook), FFT convolution, resampling, `check_keys`
- `ampsim/oversample.py`: oversampling for clipping (chunked) and up/down for the feedback loop
- `ampsim/wavio.py`: wav read/write
- `presets/*.json`: one file = one amp
- `shared/settings.json`: help text for every setting (see below)

## How the blocks work (keep these rules in the C++ version too)

- **Gain**: pre filters -> drive -> `curve(x + bias) - curve(bias)` at 8x -> post filters -> level.
  A 10 Hz high-pass is always added at the end, because uneven clipping leaves a DC offset.
- **Sag**: listens where it sits (sound passes through unchanged). Envelope with attack/release, then
  `headroom = 1 - amount * tanh(env * sens)`. Targets clip as `h * curve(x / h + bias)`, so quiet
  sound is not changed. Targets must come AFTER the sag block (a target before it would need a loop).
- **Feedback**: inner chain gets `input - amount * path(output)`. **There is no delay in the loop.**
  Each sample is found by solving the loop equation (`solve_falling`, Illinois method). Do not "simplify"
  this to a 1-sample delay: that oscillates when loop gain is above 1, and real amps run 2-10.
  Filters have `tick(v, commit=False)` to try a value without moving on. Only `eq` and `gain` can go
  inside. `amount` must be >= 0 (the solver needs the loop to be one-way rising; positive feedback is not done yet).
- **Presets**: every unknown setting name is an error (`check_keys`), so typos never pass silently.
- `SCALAR_CURVES` (used in the loop) must stay the same as `CURVES` (used with NumPy).

## shared/settings.json: the one place for help text

Every setting has: label, type, unit, min/max (plugin knob range), default, `short` (one line) and
`tooltip` (detailed). The plugin will show these as hover tooltips. **When you add, rename or change the
default of a setting, update this file in the same change.** Names, choices and defaults must match the code.
Write the text in plain, simple English (the user is not a native English speaker).

## Checks done so far (redo them after changing the DSP)

Tests are small scripts run from the scratchpad, comparing results with theory:

- Oversampling: hard clip of a 4.5 kHz tone, fold-back noise 21 dB below the harmonics at 1x, 62 dB at 8x.
- Chunked oversampling equals one-piece processing (difference ~1e-15).
- Sag on a loud burst: headroom 0.91 -> 0.54 in ~20 ms, back in ~600 ms; quiet sound unchanged.
- Feedback, linear inner gain 10, amount 0.1: closed-loop gain 5.000 at 200 Hz, 1 kHz and 5 kHz.
  With a -12 dB high shelf in the path: matches the exact theory (5.00 at 200 Hz, 8.05 at 8 kHz).
- Feedback at the same output level: distortion 0.83% with feedback vs 3.99% without.
- Feedback with loop gain 63 stays stable and matches theory.
- Feedback with amount 0 equals a plain gain stage (only the first/last 1 ms of a file differ).
- Feedback speed: ~1 s of work per 1 s of audio (4x).

Idea for later: turn these into a real `tests/` folder.

## Plan for the C++ plugin

- JUCE, exact version pinned. Needs Visual Studio and CMake (not installed on this machine yet).
- Latency target: ~0.2 ms for the amp itself. Oversample the WHOLE amp once (up at the input, down at
  the end) with minimum-phase filters, instead of per gain stage like the Python version.
  EQ, sag and feedback add no latency. Use zero-latency convolution for the IR.
- Smooth parameter changes over a few ms (no zipper noise). Use `double` for filter state
  (low frequencies at 8x sample rate are not precise enough in `float`).
- Load `shared/settings.json` as JUCE BinaryData for tooltips and knob ranges.

## Status

Done: step 1 (eq, gain, ir, oversampling), step 2 (sag), step 3 (feedback), step 4 (clean, crunch,
high gain presets; distortion at soft/medium/hard playing is in the README).
The fake DI uses a triangle pluck shape plus a pickup low-pass. A white-noise pluck was far too bright
(half its energy above 2 kHz) and made the presets look wrong.
Ideas for later: a `tonestack` block built from real component values (the bass/mid/treble circuits of
real amps, where the knobs affect each other); positive feedback; sag targets before the sag block.
The user often can not listen to the output, so report measured numbers, not only "it sounds right".
