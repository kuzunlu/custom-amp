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
- `ampsim/filters.py`: biquad filters (RBJ cookbook), general `IIR`, FFT convolution, resampling, `check_keys`
- `ampsim/circuit.py`: turns a resistor/capacitor netlist into a digital filter (used by the tone stack)
- `ampsim/curves.py`: the `custom` and `triode` clipping curves, and the push-pull power tube pair
- `ampsim/oversample.py`: oversampling for clipping (chunked) and up/down for the feedback loop
- `ampsim/wavio.py`: wav read/write
- `presets/*.json`: one file = one amp
- `shared/settings.json`: help text for every setting (see below)

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

Idea for later: turn these into a real `tests/` folder.

## Plan for the C++ plugin

- JUCE, exact version pinned. Needs Visual Studio and CMake (not installed on this machine yet).
- Latency target: ~0.2 ms for the amp itself. Oversample the WHOLE amp once (up at the input, down at
  the end) with minimum-phase filters, instead of per gain stage like the Python version.
  EQ, sag and feedback add no latency. Use zero-latency convolution for the IR.
- Smooth parameter changes over a few ms (no zipper noise). Use `double` for filter state
  (low frequencies at 8x sample rate are not precise enough in `float`).
- Load `shared/settings.json` as JUCE BinaryData for tooltips and knob ranges.

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
Next ideas: compare the amp-style presets with recordings of real amps (the Plexi-style one does not clean up
when played softly, see its notes); a top-boost tone circuit and Baxandall/James netlists; KT88/6550 (fit to
their datasheets like the others); a real `tests/` folder; positive feedback; sag targets before the sag
block; then the C++ plugin and the release (see above).
The fake DI uses a triangle pluck shape plus a pickup low-pass. A white-noise pluck was far too bright
(half its energy above 2 kHz) and made the presets look wrong.
The user often can not listen to the output, so report measured numbers, not only "it sounds right".
