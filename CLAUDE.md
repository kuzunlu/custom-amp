# custom-amp

A guitar amp built from a few blocks (EQ, gain stage, sag, feedback, cab IR) that can go in any order,
so it can copy any amp. The idea: every amp is EQ and clipping, plus power supply sag and a feedback loop.

**Final product:** a C++ JUCE plugin (VST3 + standalone app) for live playing. It is in `plugin/` and works.
**Also:** a Python prototype (`ampsim/`) that processes `.wav` files offline, to get the sound design right.
The Python code stays the reference: the plugin must give the same output for the same preset.

## Commands

Use `C:\Python314\python.exe`. Only NumPy is used (no SciPy; filters and resampling are written by hand).

```
python tools/make_test_audio.py                                       # fake DI riff + fake cab IR into audio/
python run.py presets/crunch.json audio/test_di.wav out/crunch.wav    # run a preset
python tools/check_settings.py                                        # settings.json matches the code?
```

`.wav` files are not in git (`.gitignore`). `audio/` test files can always be made again.

C++ (JUCE 9.0.5 as a git submodule in `libs/JUCE`, same commit as stream-bridge; Build Tools 18 is installed):

```
build.bat                                                   # plugin (VST3 + standalone) + test programs, into build/
build.bat --target EngineTest                               # only one target
build\PluginTest_artefacts\Release\PluginTest.exe           # plugin like a DAW: presets, file watching, state, editor
python tools/cpp_check.py                                   # C++ engine vs Python, block math (needs EngineTest)
build\EngineTest_artefacts\Release\EngineTest.exe render presets/crunch.json audio/test_di.wav out/c.wav   # + CPU time
```

Clone with `git submodule update --init` to get JUCE.

## Layout

- `ampsim/blocks.py`: all blocks (`EQ`, `Gain`, `Sag`, `Feedback`, `IRLoader`) and the clipping curves
- `ampsim/chain.py`: builds an amp from a preset, links sag blocks to their targets
- `ampsim/filters.py`: biquad filters (RBJ cookbook), general `IIR`, FFT convolution, resampling, `check_keys`
- `ampsim/circuit.py`: turns a resistor/capacitor netlist into a digital filter (used by the tone stack)
- `ampsim/curves.py`: the `custom` and `triode` clipping curves, and the push-pull power tube pair
- `ampsim/oversample.py`: oversampling for clipping (chunked) and up/down for the feedback loop
- `ampsim/wavio.py`: wav read/write
- `presets/*.json`: one file = one amp
- `shared/settings.json`: help text for every setting (see below)
- `plugin/Source/dsp/`: the C++ engine, one file per Python module (`Blocks`, `Curves`, `Filters`, `Circuit`),
  plus `Convolver` (cab IR) and `Engine` (`Amp` = linked blocks, `Engine` = oversampling + auto level)
- `plugin/Source/PluginProcessor.*`, `PluginEditor.*`: the plugin; `plugin/tests/`: the two test programs
- `CMakeLists.txt`, `build.bat`: like stream-bridge's

## How the blocks work (keep these rules in the C++ version too)

- **Gain**: pre filters -> drive -> `curve(x + bias) - curve(bias)` at 8x -> post filters -> level.
  A 10 Hz high-pass is always added at the end, because uneven clipping leaves a DC offset.
- **Curves**: `soft`, `tube`, `diode`, `hard` (fixed shapes in `CURVES` / `SCALAR_CURVES`), plus
  `custom` and `triode` built in `ampsim/curves.py` from their settings (`Gain.CURVE_DEFAULTS`; a setting
  for one curve used with another curve is an error). `triode`: Koren 1996 model + plate resistor load
  line, solved by bisection into a table at start-up; input = grid swing in volts around `bias_v`;
  output = plate swing / small-signal gain, flipped upright (real stages invert; we do not).
  Grid current above 0 V: slope 2% after a 0.1 V soft corner (fixed numbers in curves.py).
  Each curve has a `top` (where the top starts clipping); bias shift starts there.
  `plate_volts: true` multiplies the output by the stage's real gain (output = plate swing in volts), so
  the next stage's `drive_db` is just the pot or divider between the tubes, as on a schematic.
- **Amp-style presets** (`brit_plexi`, `brit_800`, `us_blackface_clean`, `us_modern_high_gain`,
  `us_hotrod_lead`, `german_high_gain`, `brit_top_boost`): signal in volts all the way (DI peak 0.5 V =
  a guitar), triode stages with `plate_volts`, phase inverter = one triode at -6 dB inside the feedback
  loop with the power block. Each preset's `notes` lists what is from schematics and what is a guess.
  Check the "peak after each block" levels when changing one: the phase inverter input must stay a few
  volts for a clean amp (the blackface preset distorted 26% before its -18 dB "reverb_mixer" loss was added).
  Brand names stay out of preset names. Phase inverter = one triode with `drive_db: -6` (each triode of a
  long-tailed pair sees half the input: twice the headroom, same total gain), NOT `level_db: -6`.
  Open question: the Plexi-style preset distorts ~32% even at 0.05 V input (preamp clean, phase inverter
  gets ~17 V). Needs a real amp recording to decide if that is right.
- **Power** (push-pull): grid A = `bias + sa + x`, grid B = `bias + sb - x`; each current from Koren's
  pentode model (`PENTODES`: kg1/kp FITTED to each datasheet's class A point, which they match exactly;
  my remembered Koren numbers for EL34/6L6 were 3-12% off). Output = (Raa/4)(IA - IB), soft-limited at
  `plate_v * (Raa/4) / (Raa/4 + rsat)`. `rsat` was worked back from known amp outputs (EL34 pair 50 W
  etc.), so the output-power check is a consistency check, not independent proof. Sag scales the supply:
  `I(vg, h*Vs) = h**1.35 * I(vg/h, Vs)` (Koren's model allows this exactly). Each grid has its own bias
  shift. Full output is scaled to |bias_v|. Default bias -45 V: -35/-38 V ran an EL34 at 450 V far
  above its 25 W limit in this model.
- **Bias shift** (in gain): `g = drive*x/h + bias`; the curve gets `g + s`. When `g + s > 1` (past the top of
  the curve; really `g + s > top`), `s` drops by `bias_shift * attack * (g + s - 1)`; every sample it also drifts back to 0 with the
  release time. `s` is worked out at the normal rate in `process` (Python loop) and per sample in `tick`.
  This is a memory effect: EQ + clipping alone can not do it.
- **Sag**: listens where it sits (sound passes through unchanged). Envelope with attack/release, then
  `headroom = 1 - amount * tanh(env * sens)`. Targets clip as `h * curve(x / h + bias)`, so quiet
  sound is not changed. Targets must come AFTER the sag block (a target before it would need a loop).
- **Feedback**: inner chain gets `input - amount * path(output)`. **There is no delay in the loop.**
  Each sample is found by solving the loop equation (`solve_falling`, Illinois method). Do not "simplify"
  this to a 1-sample delay: that oscillates when loop gain is above 1, and real amps run 2-10.
  Filters have `tick(v, commit=False)` to try a value without moving on. Only `eq`, `tonestack`, `gain` and
  `power` can go inside. `amount` must be >= 0 and every inner block must rise with its input (first filter
  coefficient b0 > 0), or the solver can find a wrong answer. Positive feedback is not done yet.
- **Tonestack**: the classic passive circuit as a netlist, solved by `circuit.py` (nodal analysis ->
  H(s) polynomials via FFT on a circle -> bilinear transform -> `IIR`). The MID POT IS A 3-TERMINAL POT:
  bass pot -> mid pot top, mid pot bottom -> ground, C3 -> mid pot wiper. Wiring it as a variable resistor
  was wrong by up to 78 dB against the published formula (Yeh & Smith 2006). Default tapers: treble and
  mid linear, bass log (all-log made "all at 5" far too dark). New tone circuits = a new netlist.
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
- Tonestack: circuit solver vs the Yeh & Smith closed form, 12 knob settings x 2 styles: within 0.0003 dB.
  Digital vs analog: 0.01 dB up to 10 kHz at 48 kHz. All knobs at 5: mid dip at ~700 Hz (british, -4.7 dB)
  and ~760 Hz (american, -6.9 dB). b0 > 0 for all 54 tested settings at 48 and 192 kHz.
- Triode 12AX7, 300 V, 100k, -1.5 V: plate 205 V, 0.95 mA, gain 61 (real tube data: ~190-205 V, ~60).
  At 0.5 V: 2.6% 2nd / 0.1% 3rd harmonic. Cold bias -2.5 V at 2 V: 24% 2nd harmonic.
  Bias shift on a triode starts at grid current (1.2 V peak: none, 1.8 V: yes).
- Power: datasheet points hit exactly (EL34 100 mA 11 mA/V, 6L6GC 72/6, EL84 48/11.3, 6V6GT 45/4.1).
  Max output: EL34 pair 48 W, 6L6GC 45 W, EL84 17 W, 6V6GT 22 W. Matched pair: 0.00% 2nd harmonic;
  10% mismatch: 0.95%. EL34 450 V, 10 V drive: bias -38 V 0.24%, -45 V 1.61% (crossover).
  Bias shift 0.5: soft note after a loud part 5.4% vs 3.1% alone, normal again after ~250 ms.
- After any change: all presets must still equal their `out/*.wav` files when the change should not
  affect them (difference ~1.6e-7 = 24-bit rounding).

C++ checks:
- `tools/cpp_check.py`, 1 s of the test DI, all 10 presets: core at 384 kHz (no oversampling on either side)
  -159 to -210 dB below the output peak (-159 = the feedback solver's 1e-10 tolerance); tail (cab IR) -137 dB
  (the `float` FFT in the convolver).
- Full plugin engine vs Python with its own per-stage oversampling, 220 Hz at 0.05/0.2/0.5 V, no cab: harmonics'
  share within 0.2 points on every preset at 8x and 4x; low/mid/high balance of the riff within 1%.
- CPU (10 s riff, 48 kHz, 8x, 256-sample blocks): 3-13% of one core (feedback presets ~10-13%). Build time
  36-216 ms. Latency 6 samples at 8x (0.13 ms at 48 kHz), 4 samples at 2x.
- Cab IR level: the 48 kHz test IR gives 1 kHz at 9.48 dB in 44.1/48/96 kHz sessions (before the
  `ir_rate / rate` fix: 8.76 / 9.48 / 15.50 dB). C++ vs Python tail at 96 and 44.1 kHz: -143 / -137 dB.
- `PluginTest`: all built-in presets come out at -20 +/- 0.3 dBFS; a typo in a watched preset file shows the error
  and keeps the old amp playing; fixing the file reloads it; quality, saved state and the editor work.

Idea for later: turn the Python checks into a real `tests/` folder.

## The C++ plugin (keep these rules)

- **Same math as Python.** Every C++ block is a line-by-line copy of the Python one (same formulas, same
  order, same error messages). When a Python block changes, change the C++ one in the same commit and run
  `tools/cpp_check.py`. NumPy's `arange` is copied exactly (`numpyArange`) so the tube tables match.
- **Oversampling:** the plugin oversamples the WHOLE amp once (JUCE `Oversampling<double>`, polyphase IIR,
  integer latency) instead of per gain stage. Blocks ignore their own `oversample` setting in the plugin.
  The Quality knob (2x/4x/8x) is for 44.1/48 kHz; at 88.2/96 kHz it uses half, at 176.4/192 kHz a quarter,
  so the amp always runs at about the same rate. All filter state is `double`.
- **Core and tail:** `tailStart()` cuts the chain at the first `ir` of the run of `ir`/`eq` blocks at the end.
  Everything before runs oversampled ("core"), the rest at the normal rate ("tail"). So in the plugin an `ir`
  block must be at the end and not inside split/feedback (Python allows it anywhere); a gate's `apply_after`
  can not name a tail block. These are plugin-only errors.
- **Cab IR:** own zero-latency convolver (`Convolver`: first 128 taps direct, the rest FFT in 128-sample
  blocks), NOT `juce::dsp::Convolution` (it loads on its own thread, so tests and the auto level would
  not be repeatable). IR resampling copies `resample_fft` (Bluestein DFT for any length), then multiplies by
  `ir_rate / rate` (both versions), so a cab is equally loud at every session rate. IR file paths:
  absolute, next to the preset, one folder up, or the current folder; not found -> built-in fake 4x12
  (`makeFakeCabIr`, same as `make_test_audio.py`) with a note in the status. "Load IR" replaces every ir file.
- **Auto level:** after building, the engine plays a short fake riff (0.5 V peak) and sets its output so it
  averages -20 dBFS, then resets. Amp-style presets put out tens of volts, so this is needed.
- **Threads:** engines are built on a background thread (`Builder`), handed to the audio thread through
  `pendingEngine`, and the old one comes back through `retiredEngine` to be deleted on the message thread.
  Nothing is allocated or freed on the audio thread. A failed build keeps the old engine playing and shows the
  error. `prepareToPlay` builds synchronously. A preset loaded from a file is reloaded when the file changes.
- Input is the average of the input channels (mono amp), output goes to all channels. Non-finite output
  resets the engine (should never happen).
- `COMPANY_NAME "Kursat"` and `PLUGIN_MANUFACTURER_CODE Kurs` are the same as stream-bridge (Ableton groups
  plugins by maker). Never change `PLUGIN_CODE Camp`: saved DAW projects find the plugin by it.
- Plugin knobs (Input, Output, Quality) have their help text in the `plugin` section of `shared/settings.json`.
- Not done yet: editing block settings in the plugin window (the chain is shown read-only, with tooltips),
  parameter smoothing for block settings, a real amp UI.

## Release plan (not started; the user said "later")

The user wants releases with an exe installer, following their repo `github.com/kuzunlu/stream-bridge`:
- `.github/workflows/build.yml` on `windows-latest`: builds on every push; pushing a tag like `v0.1.0`
  also runs `gh release create` with the files attached. Actions are pinned by commit SHA.
- `installer/<Name>.iss` (Inno Setup 6, installed in CI with `choco install innosetup`), built with
  `ISCC /DAppVersion=<tag without v>`; output in `build/installer/`. Fixed `AppId` (never change it).
- Version from `git describe --tags --match "v[0-9]*"`; `v0.0.0` when there is no tag.
- stream-bridge is a JUCE VST3 (JUCE as a git submodule, `build.bat` uses the Visual Studio CMake/Ninja,
  static MSVC runtime). Its installer puts the `.vst3` into `{commoncf64}\VST3`.

OPEN DECISION (ask the user before starting):
- A. Release the Python prototype now: `custom-amp.exe` via PyInstaller (exact version pinned), plus
  presets and `shared/settings.json`, in an Inno Setup installer. Recommended first step; the same
  workflow can later ship the VST3.
- B. Build the JUCE VST3 plugin first, then release it like stream-bridge.
Also ask: version (suggested `v0.1.0`) and whether the repo is private (a release on a public repo can
be downloaded by anyone). Pushing the release tag is the publishing step.

## Status

Pushed to `github.com/kuzunlu/custom-amp` (branch `main`).
Done: eq, gain (curves soft/tube/diode/hard/custom/triode, bias shift, `plate_volts`), tonestack (from part
values), power (push-pull EL34/6L6GC/EL84/6V6GT), sag, feedback, split, gate, ir; `tools/check_settings.py`;
simple presets (clean, crunch, high_gain) and 7 amp-style presets (see README).
C++ plugin: the engine (all blocks), VST3 + standalone, preset menu (built-in presets) + open/watch a preset
file, Load IR, Input/Output/Quality, status box, read-only chain view with tooltips. Checked (see above).
Not yet tried in a real DAW by the user.
Next ideas: edit block settings in the plugin window (knobs for each setting, from settings.json ranges);
compare the amp-style presets with recordings of real amps (the Plexi-style one does not clean up
when played softly, see its notes); a top-boost tone circuit and Baxandall/James netlists; KT88/6550 (fit to
their datasheets like the others); a real `tests/` folder; positive feedback; sag targets before the sag
block; the release (see above; option B is now possible).
The fake DI uses a triangle pluck shape plus a pickup low-pass. A white-noise pluck was far too bright
(half its energy above 2 kHz) and made the presets look wrong.
The user often can not listen to the output, so report measured numbers, not only "it sounds right".
