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
python tools/check_settings.py                                        # settings.json matches the code?
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
- **Bias shift** (in gain): `g = drive*x/h + bias`; the curve gets `g + s`. When `g + s > 1` (past the top of
  the curve), `s` drops by `bias_shift * attack * (g + s - 1)`; every sample it also drifts back to 0 with the
  release time. `s` is worked out at the normal rate in `process` (Python loop) and per sample in `tick`.
  This is a memory effect: EQ + clipping alone can not do it.
- **Sag**: listens where it sits (sound passes through unchanged). Envelope with attack/release, then
  `headroom = 1 - amount * tanh(env * sens)`. Targets clip as `h * curve(x / h + bias)`, so quiet
  sound is not changed. Targets must come AFTER the sag block (a target before it would need a loop).
- **Feedback**: inner chain gets `input - amount * path(output)`. **There is no delay in the loop.**
  Each sample is found by solving the loop equation (`solve_falling`, Illinois method). Do not "simplify"
  this to a 1-sample delay: that oscillates when loop gain is above 1, and real amps run 2-10.
  Filters have `tick(v, commit=False)` to try a value without moving on. Only `eq` and `gain` can go
  inside. `amount` must be >= 0 (the solver needs the loop to be one-way rising; positive feedback is not done yet).
- **Split**: every path gets the same input; outputs are added (`sign * level` per path). Any block can go in
  a path. Every block has `children()`; `chain.flatten` walks into feedback loops and split paths, so sag
  targets and unique ids work everywhere. `build_chain` lives in `blocks.py` (Split needs it).
- **Gate**: level = biggest |x| in the last 20 ms (sliding max). Do not use a fast-falling peak follower:
  it dips between the peaks of a low note (low E = 12 ms per cycle) and the gate chatters.
  Opens above threshold, closes below threshold - hysteresis after `hold`; gain moves with attack/release.
  With `apply_after`, the gain goes to that block's `after_gain` (applied in `run_blocks`); the gate's
  own position passes the sound unchanged. Not allowed on a block inside a feedback loop.
- **Presets**: every unknown setting name is an error (`check_keys`), so typos never pass silently.
- `SCALAR_CURVES` (used in the loop) must stay the same as `CURVES` (used with NumPy).

## shared/settings.json: the one place for help text

Every setting has: label, type, unit, min/max (plugin knob range), default, `short` (one line) and
`tooltip` (detailed). The plugin will show these as hover tooltips. **When you add, rename or change the
default of a setting, update this file in the same change.** Names, choices and defaults must match the code:
run `tools/check_settings.py`. Each block class lists its setting names in `SETTINGS`.
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
- Bias shift 1.0, tube curve, 220 Hz: loud part (amplitude 3) then soft (0.8). Soft note 0-40 ms after:
  level 0.47 vs 0.72 alone, 2nd harmonic 15.6% vs 3.1%; back to normal after ~100 ms.
  Loop (tick) vs normal (process): 0.08% apart (the loop tracks the shift at the higher rate).
- Split: two empty paths = 2x input, one inverted = exact silence, nested split with feedback matches theory
  (1.9988), sag reaches a gain inside a path.
- Gate: hiss -70 dB, note -23 dB, threshold -50: hiss -> -150 dB, note unchanged, opens in 0.3 ms.
  High-gain amp: gate placed after the amp leaves hiss at -11.5 dB (never closes); gate at the input with
  apply_after the last stage: -79 dB. Fading note with hiss -70, threshold -55: hysteresis 0 = 4-8 state
  changes (chatter), 3 = 2 (clean), 6 = 1 (never closes: closing point under the hiss peaks).
- After any change: all presets must still equal their `out/*.wav` files when the change should not
  affect them (difference ~1.6e-7 = 24-bit rounding).

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
Step 5 plan (the user wants to build any famous amp style: Plexi, 800-series, Rectifier, Soldano, ENGL,
5150, Fender clean, Vox): bias shift (done), split (done), noise gate (done), tone stack from real
component values, more clipping curves; then famous-amp-style presets with descriptive names
(no brand names in preset names). Push-pull and output transformer are built from existing blocks.
The fake DI uses a triangle pluck shape plus a pickup low-pass. A white-noise pluck was far too bright
(half its energy above 2 kHz) and made the presets look wrong.
Ideas for later: a `tonestack` block built from real component values (the bass/mid/treble circuits of
real amps, where the knobs affect each other); positive feedback; sag targets before the sag block.
The user often can not listen to the output, so report measured numbers, not only "it sounds right".
