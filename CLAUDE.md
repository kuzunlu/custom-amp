# custom-amp

A guitar amp built from a few blocks (EQ, gain stage, sag, feedback, cab IR) that can go in any order,
so it can copy any amp. The idea: every amp is EQ and clipping, plus power supply sag and a feedback loop.

**Final product:** a C++ JUCE plugin (VST3 + standalone app) for live playing. It is in `plugin/` and works.
**Also:** a Python prototype (`ampsim/`) that processes `.wav` files offline. It was used to get the sound design right.

**Python is FROZEN (decided 2026-11-24).** It is never part of the plugin; it only costs development time (every
feature written twice, slow checks, and its whole-file design blocks features like sag before the sag block).
So: the blocks that exist today stay in Python as a check (`cpp_check.py` keeps comparing them with C++; keep
them working). NEW features are written in C++ only, with their tests on the C++ engine (EngineTest/PluginTest).
The user's goal: the final plugin as fast and plain as possible.

## Commands

Use `C:\Python314\python.exe`. Only NumPy is used (no SciPy; filters and resampling are written by hand).

```
python tools/make_test_audio.py                                       # fake DI riff + fake cab IR into audio/
python run.py presets/crunch.json audio/test_di.wav out/crunch.wav    # run a preset
python tools/check_settings.py                                        # settings.json matches the code?
python tools/check_panel.py                                           # front panel rules (shared cases) + every preset's panel
python -m unittest discover -s tests -t .                             # all Python tests (~40 s; tests/, standard unittest)
check.bat                                                             # EVERY check: Python tests, build, PluginTest, cpp_check
```

Run `check.bat` before pushing: GitHub only builds release tags. A Python test file per subject in `tests/`
(oversample, sag, feedback, bias_shift, split, gate, tonestack, curves, power, presets); `tests/common.py` has the
tone measuring helpers. `test_presets` compares the first 0.4 s of every preset with its `out/*.wav` (scaled, 24-bit
rounding), so a change that is MEANT to change the sound needs the files made again with `run.py`.

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
- `ampsim/panel.py`: checks a preset's front panel (`panel` list) against `settings.json`
- `presets/*.json`: one file = one amp
- `shared/settings.json`: help text for every setting (see below)
- `plugin/Source/dsp/`: the C++ engine, one file per Python module (`Blocks`, `Curves`, `Filters`, `Circuit`),
  plus `Convolver` (cab IR) and `Engine` (`Amp` = linked blocks, `Engine` = oversampling + auto level)
- `plugin/Source/PluginProcessor.*`, `PluginEditor.*`: the plugin; `PanelView` (front panel), `Panel` (reads/edits the
  panel list), `ChainView` + `Inspector` (chain editor); `plugin/tests/`: the two test programs and `panel_cases.json`
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
  KT88 (JJ sheet: 250/250/-15 V, 140 mA, 11.5 mA/V, mu 8) and 6550 (250/250/-14 V, 140 mA, 11 mA/V; mu 8.2 because
  with 8 this model can not reach 11 mA/V at 140 mA) were fitted the same way; their `rsat` comes from the
  Svetlana sheets' fixed-bias rows: 100 W at 560 V into 4.5k (KT88), 100 W at 600 V into 5k (6550C).
  Safe bias (70% of max plate heat, screen = plate): KT88 at 560 V -66 V, 6550 at 600 V -76 V.
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
  coefficient b0 > 0), or the solver can find a wrong answer.
  Positive feedback = a NEGATIVE `amount`. The loop equation has exactly one answer only while the loop can never
  gain 1 or more within one sample, so the block checks |amount| x first_gain(path) x the product of every inner
  block's `max_slope()` < 1 (else an error naming the value). `max_slope` = the product of the filters' b0 x drive
  x level x the steepest point of the curve (`steepest()`: the same 200001 points on -100..100 in Python and C++)
  or, for power, of `unlimited` (the pair without its soft limit at full supply: sag and the limit only flatten
  it). The C++ loop runs at the core rate, Python at its `oversample` rate, so the b0s and the value differ a little.
- **Tonestack**: the classic passive circuit as a netlist, solved by `circuit.py` (nodal analysis ->
  H(s) polynomials via FFT on a circle -> bilinear transform -> `IIR`). The MID POT IS A 3-TERMINAL POT:
  bass pot -> mid pot top, mid pot bottom -> ground, C3 -> mid pot wiper. Wiring it as a variable resistor
  was wrong by up to 78 dB against the published formula (Yeh & Smith 2006). Default tapers: treble and
  mid linear, bass log (all-log made "all at 5" far too dark). New tone circuits = a new netlist.
  Each style names its circuit (`CIRCUITS`): `british`/`american`/`top_boost` = "fmv", `james` = a passive
  Baxandall (designed here: 10:1 ratios, log pots, flat within 0.8 dB at 5/5). `top_boost` = fmv without a mid pot
  (`fixed` mid 10 with R3 = 10k: that 10k is a guess), 50p/22n/22n/100k/1M log pots from published summaries.
  A knob or taper the style does not have is an error. settings.json: `style.circuits`, `parts.names` per
  circuit, `styles` on mid/mid_taper; the inspector hides what the style lacks and a style change removes it.
  brit_top_boost uses it (treble 8, bass 3: the eq it replaced was brighter at 6/4).
- **Split**: every path gets the same input; outputs are added (`sign * level` per path). Any block can go in
  a path. Every block has `children()`; `chain.flatten` walks into feedback loops and split paths, so sag
  targets and unique ids work everywhere. `build_chain` lives in `blocks.py` (Split needs it).
- **Gate**: level = biggest |x| in the last 20 ms (sliding max). Do not use a fast-falling peak follower:
  it dips between the peaks of a low note (low E = 12 ms per cycle) and the gate chatters.
  Opens above threshold, closes below threshold - hysteresis after `hold`; gain moves with attack/release.
  With `apply_after`, the gain goes to that block's `after_gain` (applied in `run_blocks`); the gate's
  own position passes the sound unchanged. Not allowed on a block inside a feedback loop.
- **Presets**: every unknown setting name is an error (`check_keys`), so typos never pass silently.
- **Front panel** (`panel`, top-level preset key, at most 8 knobs): `{label, block, setting, range}`. `block` = an id
  (found anywhere: main chain, feedback chains, split paths); `setting` = a NUMBER setting from `settings.json`, or a
  dot path into a band list or split paths (`path.0.gain_db`, `bands.1.gain_db`, `paths.0.level_db`); `range` = the
  values at knob 0 and 10 (may run backwards: Presence `[0, -12]`), default = the setting's min/max in settings.json.
  It does not change the sound. Python only checks it (`ampsim/panel.py`, after the chain is built); the plugin uses
  it, with the same checks and messages (`Panel.cpp`); `plugin/tests/panel_cases.json` holds the shared cases.
  Built-in presets: signal-flow order (Gain, Bass, Middle, Treble, Presence, Master); gain `drive_db` knobs use
  `[0, 40]`, level knobs `[-40, 0]`, eq-band tone knobs `[-12, 12]`.
- `SCALAR_CURVES` (used in the loop) must stay the same as `CURVES` (used with NumPy).

## shared/settings.json: the one place for help text

Every setting has: label, type, unit, min/max (plugin knob range), default, `short` (one line) and
`tooltip` (detailed). The plugin will show these as hover tooltips. **When you add, rename or change the
default of a setting, update this file in the same change.** Names, choices and defaults must match the code:
run `tools/check_settings.py`. Each block class lists its setting names in `SETTINGS`.
Write the text in plain, simple English (the user is not a native English speaker).

## Checks done so far (redo them after changing the DSP)

These are now tests in `tests/` (each limit is written next to the measured value), comparing results with theory:

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
  and ~760 Hz (american, -6.9 dB). b0 > 0 for all tested settings of every style at 48 and 192 kHz.
  James at 5/5: flat within 0.8 dB (40 Hz-8 kHz); bass/treble beyond +-12 dB; top_boost = the fmv netlist at mid 10.
- Triode 12AX7, 300 V, 100k, -1.5 V: plate 205 V, 0.95 mA, gain 61 (real tube data: ~190-205 V, ~60).
  At 0.5 V: 2.6% 2nd / 0.1% 3rd harmonic. Cold bias -2.5 V at 2 V: 24% 2nd harmonic.
  Bias shift on a triode starts at grid current (1.2 V peak: none, 1.8 V: yes).
- Power: datasheet points hit exactly (EL34 100 mA 11 mA/V, 6L6GC 72/6, EL84 48/11.3, 6V6GT 45/4.1).
  Max output: EL34 pair 48 W, 6L6GC 45 W, EL84 17 W, 6V6GT 22 W, KT88 and 6550 100 W (their sheets). Matched pair: 0.00% 2nd harmonic;
  10% mismatch: 0.95%. EL34 450 V, 10 V drive: bias -38 V 0.24%, -45 V 1.61% (crossover).
  Bias shift 0.5: soft note after a loud part 5.4% vs 3.1% alone, normal again after ~250 ms.
- After any change: all presets must still equal their `out/*.wav` files when the change should not
  affect them (difference ~1.6e-7 = 24-bit rounding).

C++ checks:
- `tools/cpp_check.py` (leaves the `panel` out of the core/tail parts; fails above -120 dB), 1 s of the test DI, all 10 presets: core at 384 kHz (no oversampling on either side)
  -159 to -210 dB below the output peak (-159 = the feedback solver's 1e-10 tolerance); tail (cab IR) -137 dB
  (the `float` FFT in the convolver).
  It also runs `tests/cpp_presets/` (test-only presets for parts no built-in preset uses): KT88 + james -131 dB,
  6550 + top_boost + positive feedback -180 dB. The james circuit alone is -132 dB: a 4th-order filter at 384 kHz
  is very sensitive to the last digits of its coefficients (not a wiring difference: that would be near 0 dB), and it
  makes last-digit differences in its input about 67 dB bigger.
  A preset whose loop RINGS can not be compared: tiny differences grow with the ringing (about 3 dB per 100 ms).
  kt88_james had feedback 0.05 and rang at 10 Hz on its own after 1 s (35 V rms in silence, Python the same): the two
  10 Hz high-passes in its loop (each a 2nd-order filter) turn the phase by 180 degrees at 10 Hz. It rings from 0.04
  up, so it now uses 0.02. Every built-in preset goes quiet (about 3e-11) after the input stops.
- Full plugin engine vs Python with its own per-stage oversampling, 220 Hz at 0.05/0.2/0.5 V, no cab: harmonics'
  share within 0.2 points on every preset at 8x and 4x; low/mid/high balance of the riff within 1%.
- CPU (10 s riff, 48 kHz, 8x, 256-sample blocks): 2.6-8.7% of one core (feedback presets 5.7-8.7%; they were
  10-13.8% before the faster solver of 2026-11-24, then 6.8-9.8% before the divides came out of the loop).
  brit_800 without its loop: 4.3%; with the feedback amount at 0 the loop still costs its 2 tries per sample. Build time
  36-216 ms. Latency 6 samples at 8x (0.13 ms at 48 kHz), 4 samples at 2x.
- Cab IR level: the 48 kHz test IR gives 1 kHz at 9.48 dB in 44.1/48/96 kHz sessions (before the
  `ir_rate / rate` fix: 8.76 / 9.48 / 15.50 dB). C++ vs Python tail at 96 and 44.1 kHz: -143 / -137 dB.
- `PluginTest`: all built-in presets come out at -20 +/- 0.3 dBFS; a typo in a watched preset file shows the error
  and keeps the old amp playing; fixing the file reloads it; quality, saved state and the editor work; all 120
  blocks/paths of the built-in presets open in the inspector; moving the treble slider changes the preset and
  keeps the level; Save as writes the change. Undo: a drag of 6 slider moves is one step (fails with the merge
  window at 0 ms), the selection and the slider come back, a preset load comes back with its level, a new edit
  clears redo, undo to an old file version is marked edited. Copy/paste: duplicating the feedback loop gives
  nfb_2/pi_2/pa_2 and still builds, paste = same block, cut + undo, paste with nothing selected goes at the end,
  id rules (inner sag targets follow, outside targets stay, nfb_2 -> nfb_3, block/path mix-ups refused). The test
  saves and restores the clipboard text. A gain stage pasted at the end lands before the cab IR and builds; a sag
  can not be pasted into the feedback loop; duplicating `pi` inside it gives `pi_3`. Front panel: the 19 shared
  cases give Python's exact errors; every built-in preset has its own panel and builds; brit_800's knobs show
  the preset (Gain 7.00 = -12 dB of -40..0, Master 5.75); moving knob parameters from the DAW writes master 0 dB and
  presence -12 dB (backwards range) with the level kept; 5 knob moves 150 ms apart are one undo step; a preset without
  a panel gets 4 guessed knobs that work; Put on the front panel adds ids, stops at 8; a wrong panel shows
  "panel knob 1: no block has the id 'v2'". (PluginTest prints every line at once, so a crash shows where it was.)
- Click test (two plugins, the same plucked note in real time, one gets an edit you can not hear): biggest
  difference -57 dB below the peak. Without warm-up and fade: -0.8 dB (a full click); warm-up alone: -27 dB.
  (A first version of this test, "biggest step between samples" under a steady note, passed even without
  warm-up: always check that a test fails when the thing it tests is switched off.)
- `PluginTest --snapshot <folder>` saves pictures of the window (used for the walkthrough page).


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
- **Editing in the window:** click a block (or a split path) in the chain (`ChainView`); the `Inspector` shows
  every setting of that block from `settings.json` (set ones bright, defaults dim, `x` = back to default), with
  a control per type (number = slider with the doc's range and `scale: log`, choice, toggle, text, id list,
  band list = one line per band, tone stack parts typed like `470p`/`33k`). Only the settings of the chosen
  `curve` are shown; changing the curve removes the other curve's settings. Unknown keys (typos) are listed so
  they can be removed. Buttons: up, down, remove, add after, add inside (feedback/path), add path.
  The editor edits its parsed copy of the preset in place (`juce::var` arrays and objects are shared), then
  sends it as text (`setEditedPreset`). Rebuilding the chain view or inspector is always done later
  (`AsyncUpdater`): the click that asked for it may come from a component that the rebuild deletes.
- **Edits keep the output level** (only a preset load, a new IR or the Level button measure it again), so you
  hear what an edit does to the volume. In-window edits make the source `edited:...`: the file is no longer
  watched until **Save as...** (`saveEditedPreset`), and Reload goes back to the saved version.
- **Undo/redo** lives in the processor (survives closing the window; cleared when a DAW project loads): every
  `setPresetText` (load, reload, file change, edit) pushes {source, text, level}; undo restores the level too.
  Undoing to a `file:` step whose text is no longer the file makes it `edited:` (or the watcher would replace it).
  The editor merges small edits (no inspector rebuild) to the same block within 0.8 s into one step (a slider
  drag). After undo or a reload, the editor finds the selected list's place in the new preset (`findList` /
  `followPath`) and keeps the selection if the block type there is the same.
- **Copy/paste:** the system clipboard holds the block (or split path) as JSON text, so it works between plugin
  instances and with text editors. Paste goes after the selection (no selection: end of the main chain), through
  `makePastedItem`: a block only into a chain, a path only after a path; ids that are taken get a number
  (`v2` -> `v2_2`, `v2_2` -> `v2_3`), and `targets`/`apply_after` inside the pasted item follow the new names
  (references to blocks outside it stay). Keys: Ctrl+C/X/V/D, Delete (removes without copying).
  Windows lets one program at a time open the clipboard and JUCE tries once; other programs (clipboard history)
  read every new copy at once. So copy waits 20 ms and checks (up to 5 times), and paste falls back to the
  plugin's own last copy when the clipboard has no block. (Before this, the copy test failed 2 runs out of 3.)
- **Where new blocks go:** "Add block..." and paste with nothing selected go at `endInsertIndex`: before the
  cab (`amp::tailStart`), since only eq/ir can follow it; an eq or ir goes at the very end. Inside a feedback
  loop (`isLoopChain`), "Add after..." offers only eq/tonestack/gain/power and paste refuses other blocks.
- **Feedback solver (C++ only, faster than Python's, same answer within 1e-10):** `solveFallingFast` first guesses
  from the last three answers (3y1 - 3y2 + y3), then jumps with the slope the last sample ended with (Newton with an
  old slope, at most 2 jumps), else falls back to the plain bracketing search. 2.48 tries per sample on brit_800
  (was about 5). The answer is always the LAST value tried, so `commitTried()` moves every filter and tube on from
  what its last `peek()` remembered (`triedIn/triedOut`, `triedG`, `triedU`) instead of running the loop again:
  bit-identical output. Keep that rule if the solver changes.
  Inside a loop every step waits for the one before, and a divide takes 3-4 times as long as a multiply. So the
  tries only multiply: the tables keep `1 / step` (`tablePerVolt`, `perVolt`), `gridClamp` multiplies by `1 / gridKneeV`,
  the gain stage works out `drive / h` once per sample (`beginSample`), and the power pair works out everything that
  depends on the sag headroom only when it changes (`PushPull::supply`). 12-18% less CPU on the feedback presets; the
  results move only in the last digit. (Measured and NOT kept: `fa * (1 / slope)` in the solver, continuing the last
  two slopes for the first jump, a looser tolerance: 1e-8 saved only 3-8%.)
- **Swapping engines without a click:** the new engine is warmed up with the last 0.25 s of input (a history
  ring the audio thread writes), keeps catching up on the builder thread until less than one block is missing,
  the audio thread plays it the last few samples just before the swap, then old and new cross-fade over 30 ms.
  Triode tables are cached (they took most of the build time).
- **Front panel** (`PanelView`, shown first; "Edit chain" switches, the processor remembers the view): the preset's
  `panel` knobs, or guessed ones without it (the first main-chain tone stack's bass/mid/treble, the last main-chain
  eq's level as Master; guessed knobs point at blocks by place, so they need no ids). The knobs are 8 plugin
  parameters `knob_1..knob_8` (0-10, the DAW only sees "Knob N"). The processor polls them in its 100 ms timer
  (`applyKnobChanges`): a knob more than 0.01 from where the preset puts it is written into the preset as an edit
  (level kept, undo, click-free swap); moves of the same knob within 0.8 s are one undo step. Every preset change
  (`updatePanel`) moves the parameters to the preset's values. A wrong panel is a preset error (checked in
  `buildEngine` after the engine, like Python). Right-click a number slider or band slider in the inspector:
  "Put on the front panel" (`MenuSlider`); a guessed panel is first written into the preset (blocks get ids like
  `tonestack`). Right-click a knob: rename (or double-click its name), remove, show in the chain editor.
  The block keys (Ctrl+C/X/V/D, Delete) only work in the chain editor.

## Releases (option B chosen: the JUCE plugin)

Like the user's `github.com/kuzunlu/stream-bridge`:
- `.github/workflows/build.yml` on `windows-latest`: only a pushed `v*` tag (or "Run workflow" on the Actions page)
  builds (`build.bat`), runs `PluginTest` and, for a tag, runs `gh release create` with the installer and two zips
  (VST3, app). Normal pushes do not build: the user ran low on Actions minutes (2026-11-24), so run the tests
  locally before pushing.
  Actions are pinned by commit SHA (the same ones as stream-bridge). The repo is private: a release is only
  visible to people with access, and Actions minutes come from the user's allowance (Windows counts double).
- `installer/CustomAmp.iss` (Inno Setup 6, installed in CI with `choco install innosetup`; not installed on
  this machine), built with `ISCC /DAppVersion=<tag without v>`, output in `build/installer/`. Fixed `AppId`
  `42251C65-E078-4E35-A11F-D050236631F0` (never change it). Installs the VST3 into `{commoncf64}\VST3`, the
  standalone app into `{autopf}\Custom Amp` (Start menu entry), and the presets as editable files into
  `{commondocs}\Custom Amp\Presets` (Public Documents: writable by every user; never overwritten by an
  update, kept on uninstall). **Open...** starts in that folder when the current preset is a built-in one.
- Version from `git describe --tags --match "v[0-9]*"` (CMake sets `CUSTOM_AMP_VERSION`, shown at the end of
  the status box); `dev` / `v0.0.0` when there is no tag.
- `.gitattributes` keeps `.bat` files CRLF (cmd misreads `goto` labels with LF).
- Pushing the tag is the publishing step: always ask the user first.

## Status

Pushed to `github.com/kuzunlu/custom-amp` (branch `main`). Releases: tags `v0.1.0` and `v0.1.1` pushed on 2026-11-23
(the build before it passed on GitHub; the user checks the Actions page, `gh` is not installed here).
Done: eq, gain (curves soft/tube/diode/hard/custom/triode, bias shift, `plate_volts`), tonestack (from part
values; styles british, american, top_boost, james), power (push-pull EL34/6L6GC/EL84/6V6GT/KT88/6550), sag,
feedback (also positive), split, gate, ir; `tools/check_settings.py`;
simple presets (clean, crunch, high_gain) and 7 amp-style presets (see README).
C++ plugin: the engine (all blocks), VST3 + standalone, preset menu (built-in presets) + open/watch a preset
file, Load IR, Input/Output/Quality, status box, chain view + inspector to edit every setting and add/move/remove
blocks, Save as, Level, undo/redo, copy/paste/duplicate, front panel (8 automatable knobs), click-free engine swaps. Checked (see above). Not yet tried in a real DAW by the user.
Next ideas: compare the amp-style presets with recordings of real amps (the Plexi-style one does not clean up
when played softly, see its notes); sag targets before the sag block (real amps: the power stage's current
sags the preamp supply too). NOT done because Python runs each block over the whole file before the next one, and
its per-stage oversampling can not run in small pieces: an earlier target would need the sag value from later in the
chain, sample by sample. It needs Python's oversampling made streaming first (then both versions run the chain in
small fixed pieces, the earlier targets using the sag of the piece before).
The fake DI uses a triangle pluck shape plus a pickup low-pass. A white-noise pluck was far too bright
(half its energy above 2 kHz) and made the presets look wrong.
The user often can not listen to the output, so report measured numbers, not only "it sounds right".
