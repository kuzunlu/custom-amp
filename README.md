# custom-amp

A guitar amp built from a few simple blocks that you can put in any order:

| Block | What it does |
|---|---|
| `eq` | Boost or cut tones (high-pass, low-pass, shelves, peaks) |
| `tonestack` | The real bass/mid/treble circuit of a tube amp, from its part values. The knobs affect each other like in the real amp |
| `gain` | One gain stage: filter, then clip, then filter. Curves: `soft`, `tube`, `diode`, `hard`, `custom` (top and bottom set separately), `triode` (a real 12AX7/12AT7/12AU7 from tube equations). Optional bias shift: hard hits push the stage "colder" for a moment |
| `power` | Push-pull power amp: two power tubes (EL34, 6L6GC, EL84, 6V6GT, KT88, 6550, fitted to their datasheets) into an output transformer |
| `sag` | Lowers the clipping ceiling of later gain and power blocks when you play hard |
| `feedback` | Feeds the output of a group of blocks back to its input (like a power amp's feedback loop) |
| `split` | Parallel paths: the same sound through several chains side by side, then mixed |
| `gate` | Noise gate. Can listen at the clean input but mute after a later block (`apply_after`) |
| `ir` | Cabinet sound from an impulse response `.wav` file |
| `room` | Acoustic feedback: the amp's sound goes back into the guitar's strings, as in front of a loud amp (last block) |
| `strings` | Sympathetic strings: extra strings tuned to fixed notes ring along with your playing, like a sitar's |
| `restring` | Play your guitar, hear a virtual string with physics you choose (lower, stiffer, longer, bending by itself) |
| `bow` | Your playing bows a virtual string: endless, swelling notes, with a real bow's stick and slip |

The idea: in the end, every amp is EQ and clipping, plus some feedback and power supply sag.

It is a plugin (`plugin/`, C++ with JUCE): a VST3 and a standalone app for live playing. (A Python prototype was
used to work out the sound design; it was removed on 2026-11-24, after the plugin matched it to -131 dB or better.
Its checks against theory now run on the plugin's engine.)

## Plugin

**Install:** download `Custom-Amp-Setup-<version>.exe` from the repo's Releases page and run it. It installs the
VST3 (your DAW lists it under **Kursat**), the standalone app (Start menu: **Custom Amp**), and the presets as files
you can edit in `Public Documents\Custom Amp\Presets`. Then rescan plugins in your DAW.

**Or build it yourself** on Windows with Visual Studio (or the free Build Tools) and its "Desktop development with C++" part:

```
git submodule update --init
build.bat
```

The plugin is `build\CustomAmp_artefacts\Release\VST3\Custom Amp.vst3` (copy it to
`C:\Program Files\Common Files\VST3\`); the standalone app is next to it in `Standalone\`.

- **Front panel:** the window opens on the amp's knobs (Gain, Bass, Middle, Treble, Presence, Master...). They read 0
  to 10 like a real amp, with the real value under each one. Your DAW can automate and MIDI-map them as Knob 1 to Knob 8.
  Right-click a knob to rename or remove it. **Edit chain** opens the block editor; there, right-click any slider and
  pick **Put on the front panel** to make your own knob.
- Pick a built-in preset, or **Open...** to load your own `.json` preset. The plugin watches that file:
  edit it in any text editor, save, and the amp reloads by itself. A mistake in the file is shown in the
  status box, and the last working amp keeps playing.
- **Edit in the window:** click a block in the chain. The panel on the right shows all its settings; change
  them with the sliders and menus and you hear it right away. Add, move and remove blocks with its buttons
  (or **Add block...**). **Save as...** writes your amp to a `.json` file. While you edit, the volume is not
  corrected; press **Level** to make the amp as loud as the others again.
- **Undo** / **Redo** (Ctrl+Z, Ctrl+Y) take back any change: an edit (a whole slider drag is one step), a
  loaded preset (it comes back with its level) or a reload.
- **Copy**, **Paste after** and **Duplicate** (Ctrl+C, Ctrl+V, Ctrl+D; Ctrl+X cuts, Delete removes) work on the
  selected block with everything inside it. The copy is JSON text, so you can paste it into another plugin or a
  preset file. Ids that are already used get a number (`v2` becomes `v2_2`).
- **Load IR...** loads a cab `.wav`. Without one, the built-in presets use a simple built-in test cab.
- **Input**: hard strums should peak around -6 dBFS. **Output**: your own level on top of the automatic one
  (every preset is set to about the same loudness when it loads). **Quality**: oversampling, 8x is best.
- **Signals** (in the chain editor): see what every block does. Its own copy of the amp plays a test signal (a sine,
  a plucked note, a chord, loud-then-soft, or your last 0.6 s of playing) and shows each block's waveform or spectrum,
  with its peak in volts and, for a sine, the harmonics it adds. Change a setting and watch every stage change.
- **X-ray listening** (right-click a block's name in the chain editor): hear **only what that block adds**, or the amp
  without it, live while you play. A second copy of the amp with that block switched off (a clipping stage made
  linear, sag or feedback at 0, any other block left out) runs beside it; the plugin plays the difference. Uses a second
  amp's CPU while it is on.
- Hover over anything for help. The amp adds 6 samples of delay at 8x (0.13 ms at 48 kHz).

A cab IR must be at the end of the chain (only eq and ir blocks after it).

## Test audio and rendering files

```
build\EngineTest_artefacts\Release\EngineTest.exe make-audio      # audio/test_di.wav (fake riff) and audio/fake_4x12.wav (fake cab)
build\EngineTest_artefacts\Release\EngineTest.exe render presets\crunch.json audio\test_di.wav out\crunch.wav
```

`render` runs the whole plugin engine on a file and prints the CPU time it took. File paths inside presets (like the
IR file) are found next to the preset, one folder up, or in the folder you run from.

## Presets

A preset is a `.json` file with a `chain` list. Blocks run from top to bottom.
Misspelled settings stop the run with a message that lists the allowed ones.

| Preset | Sound | Distortion of a 220 Hz note: soft / medium / hard playing |
|---|---|---|
| `clean.json` | Bright, scooped clean with lots of headroom | 0.1% / 0.2% / 2.1% |
| `crunch.json` | Mid-forward rock crunch, cleans up when you play softly | 8% / 38% / 45% |
| `high_gain.json` | Tight modern high gain, three preamp stages | 43% / 44% / 44% |

### Amp-style presets

These copy the signal path of famous amp styles stage by stage, with part values from published
schematics where known. Each preset's `notes` says which values are from schematics and which are
guesses. They are starting points: they have not been compared with recordings of real amps yet.
The signal is in volts all the way (the test guitar peaks at 0.5 V).

Harmonics' share of the sound for a 220 Hz note at 0.05 / 0.2 / 0.5 V (soft / medium / hard playing), no cab
(measured before their sag block was replaced by a rectifier supply on the power block, 2026-11-24; the signal view
shows today's numbers):

| Preset | Style | Power | Feedback | Soft / medium / hard |
|---|---|---|---|---|
| `us_blackface_clean.json` | American blackface-style clean | 2x 6L6GC | 8.5 dB | 0.6% / 2.5% / 7.2% |
| `brit_top_boost.json` | British top-boost style, chime | 2x EL84 | none | 26% / 23% / 40% |
| `brit_plexi.json` | British Plexi-style 50 W, jumpered | 2x EL34 | 7.2 dB | 32% / 36% / 38% |
| `brit_800.json` | British 800-style master volume | 2x EL34 | 7.2 dB | 39% / 47% / 66% |
| `german_high_gain.json` | German high gain, tight | 2x EL34 | 8.4 dB | 36% / 38% / 60% |
| `us_hotrod_lead.json` | American hot-rod lead (SLO-style) | 2x 6L6GC | 6.3 dB | 50% / 72% / 79% |
| `us_modern_high_gain.json` | American modern high gain (rectifier-style) | 2x 6L6GC | 5.2 dB | 52% / 75% / 86% |

Known limit: the Plexi-style preset barely cleans up when played softly (see its notes).

### Front panel

```json
"panel": [
  { "label": "Gain",     "block": "gain_pot", "setting": "level_db",       "range": [-40, 0] },
  { "label": "Bass",     "block": "tone",     "setting": "bass" },
  { "label": "Presence", "block": "nfb",      "setting": "path.0.gain_db", "range": [0, -12] }
]
```

The knobs the plugin shows (at most 8). Each one turns one number setting of the block with that `id`. A dot path
reaches into a list: `path.0.gain_db` is the gain of the first filter band in the feedback loop's path. `range` is the
value at 0 and at 10 on the knob (without it, the setting's own range); it can run backwards. The panel does not
change the sound. Without a panel, the plugin guesses a few knobs.

### Sag

```json
{ "type": "sag", "amount": 0.3, "sens_db": 0, "attack_ms": 5, "release_ms": 150, "targets": ["v2"] }
```

The sag block listens to the signal where it sits. The sound passes through it unchanged.
When the signal gets loud, the gain blocks in `targets` get a lower clipping ceiling, so they clip earlier.

- `amount`: the most the ceiling can drop (0.3 = down to 70%). Must be below 1.
- `sens_db`: how easily it reacts. Higher = reacts to softer playing.
- `attack_ms` / `release_ms`: how fast the ceiling drops, and how fast it comes back.
- `targets`: ids of gain blocks. They must come after the sag block (a gain block inside a later feedback block is fine).

For a power block, its own rectifier supply (`supply_ohms`, `supply_uf`, see Power amp) is closer to a real amp:
it sags with the current the tubes really draw.

### Feedback

```json
{
  "type": "feedback", "amount": 0.3,
  "path": [{ "kind": "highshelf", "freq": 3000, "gain_db": -10 }],
  "chain": [{ "type": "gain", "id": "v2", "drive_db": 16, "curve": "soft" }]
}
```

The inner `chain` gets: input minus `amount` times the `path`-filtered output.
More `amount` = less gain, tighter and cleaner clipping. A NEGATIVE `amount` is positive feedback: more gain and a
harder feel. It is only allowed while the loop can never gain 1 or more (the error says what loop gain it found).
The `path` filters choose which tones get less feedback, so those come out louder:
a high shelf cut works like a **presence** knob, a low shelf cut works like a **resonance** knob.

- Only `eq`, `tonestack`, `gain` and `power` blocks can go inside.
- The loop has no delay. Each sample is found by solving the loop equation, so strong feedback is stable.
- Too much feedback can make the loop ring by itself at a low note (about 10 Hz), like a real amp "motorboating".

### Split

```json
{
  "type": "split", "level_db": -6,
  "paths": [
    { "chain": [{ "type": "gain", "drive_db": 12 }] },
    { "chain": [{ "type": "gain", "drive_db": 12, "pre": [{ "kind": "highshelf", "freq": 1500, "gain_db": 8 }] }], "level_db": -3 },
    { "chain": [], "level_db": -20, "invert": true }
  ]
}
```

Every path gets the same input; the outputs are added. Each path has its own `chain` (any blocks except a cab IR,
even another split), `level_db` and `invert` (flips the path upside down before mixing).
An empty chain passes the input through untouched (a clean blend).

### Triode curve

```json
{ "type": "gain", "curve": "triode", "tube": "12AX7", "plate_v": 300, "plate_ohms": 100e3, "bias_v": -1.5, "drive_db": 6 }
```

A real preamp tube stage, from Koren's tube equations. The input is in volts at the grid (a guitar gives
about 0.1-1 V), so copy the values from a schematic. `bias_v` sets the character: about -1.5 V is a normal
stage (the top clips first, at grid current); -2.5 to -4 V is a "cold clipper" (the bottom squashes early,
lots of 2nd harmonic). Small signals come out the same size; only the clipping shape is the tube's.

### Power amp

```json
{ "type": "power", "id": "pa", "tube": "EL34", "plate_v": 450, "bias_v": -45, "raa_ohms": 3400, "drive_db": 33, "level_db": -33 }
```

Two power tubes driven with opposite signals; the output transformer passes on the difference of their
currents, so even harmonics cancel (unless `mismatch` is set) and a cold bias gives crossover distortion.
The input is in volts at the grids: full power needs about as many volts as the bias. Full output is
scaled to `|bias_v|`. Safe bias values (about 70% of the tube's maximum heat): EL34 at 450 V: -46 V,
6L6GC at 460 V: -47 V, EL84 at 320 V: -13 V, 6V6GT at 420 V: -40 V, KT88 at 560 V: -66 V, 6550 at 600 V: -76 V.

Rectifier sag: `"supply_ohms": 170, "supply_uf": 47` gives it a real power supply. The tubes' current drains a filter
capacitor charged through that resistance (the rectifier plus the transformer winding), so the supply drops when you
play hard and recovers when you play softly. Rough values: silicon rectifier 50-100 ohm, GZ34 120-170, 5U4GB 250-300.
`plate_v` stays the voltage at idle. 0 ohm (the default) = a perfectly stiff supply. A sag block can target it too.

### Tone stack

```json
{ "type": "tonestack", "style": "british", "treble": 6, "mid": 4, "bass": 7, "parts": { "R4": 47e3 } }
```

The classic passive tone circuit used by most tube amps, solved from its part values (`plugin/Source/dsp/Circuit.cpp`).
`style` picks the circuit and its starting values; `parts` changes single parts (farads and ohms:
470 pF = `470e-12`, 22 nF = `22e-9`, 33k = `33e3`). The knobs go from 0 to 10. Like the real circuit,
it loses volume (about 5-20 dB), so raise `level_db` or the next stage's drive.

| style | circuit | knobs |
|---|---|---|
| `british`, `american` | the classic bass/mid/treble circuit | treble, mid, bass |
| `top_boost` | the 60s British top-boost unit: the classic circuit without a mid pot (a fixed 10k, unsure) | treble, bass |
| `james` | a passive Baxandall-style circuit (C3/C4 and R3/R4 add parts): flat with both knobs at 5, about -21 dB | treble, bass |

Setting `mid` with `top_boost` or `james` is an error.

### Room (acoustic feedback)

```json
{ "type": "room", "distance_m": 0.5, "volume_db": -30, "sustain_s": 3, "string_tone_hz": 1000 }
```

Put it last, after the cab. The amp's sound travels `distance_m` / 343 m/s back to the guitar, gets weaker with
distance, and shakes the string at the note you play and its harmonics; that goes round the amp again. Turn
`volume_db` up until notes sustain. Where you stand decides which harmonic the feedback blooms into (measured with the
800-style preset: the octave at 0.2-0.5 m, a high squeal at 0.8 m, the note itself at 1.2-2 m). A pitch tracker
follows the note you play (one note), and the strings count as muted when your input falls fast or is very quiet:
those two parts stand in for you, the rest is the physics of the loop.

### Sympathetic strings

```json
{ "type": "strings", "notes": ["D2", "A2", "D3", "A3", "D4"], "coupling_db": -30, "sustain_s": 4, "mix_db": 0 }
```

Strings that nobody plays, tuned to `notes` (names like `F#3` or numbers in Hz; without a list: a guitar's open strings).
Your playing shakes them; each one takes up only what matches its own note and harmonics and rings on after you stop
(measured: a string answers 25 dB louder to its own note than to one a semitone away). Before the amp it is like an
acoustic instrument with extra strings, after it like a reverb that only rings in tune.

### Restring

```json
{ "type": "restring", "transpose": -12, "stretch_cents": 10, "sustain_s": 6, "string_tone_hz": 3000,
  "bend_semitones": 0.5, "bend_s": 0.1, "mix": 1 }
```

Put it first. Every pick attack plucks a virtual string with your DI's last period, stretched to the new string's
length (so where and how hard you pick carries over: a DI without a 3rd harmonic gives a string without one). Then it
rings on its own physics: `transpose` semitones from your note (-12 = a bass), its harmonics stretched sharp like a
stiff string (`stretch_cents` = how sharp the 10th is: a guitar 1-9, a piano 17-80, more = towards a bell), its own
`sustain_s` (it goes on after your note fades), highs that fade 10 times as fast at `string_tone_hz`, and a pitch that
glides back from `bend_semitones` after each pluck. It follows your note while it rings (slides, hammer-ons), and a
hand on the strings stops it in 50 ms. A pitch tracker stands in for you: the new string starts 15-30 ms after the
pick (30 ms on a low E, like a guitar synth), and it follows one note.

### Bow

```json
{ "type": "bow", "transpose": -12, "bow_position": 0.12, "bow_force_n": 1.5, "bow_speed": 0.2,
  "swell_s": 0.05, "release_s": 2, "bridge_loss_db": 0.55, "mix": 1 }
```

Put it first. Your playing level is the bow's speed and force (both together, so softer playing stays in the same kind
of motion), your note sets the string's length, a hand on the strings lifts the bow. The friction at the bow is solved
exactly every sample: the string sticks to the bow, then slips back. With the right force that happens once a period
(Helmholtz motion, what a violin does: measured, the string is stuck 0.877 of each period with the bow at 0.12 of the
string; theory 0.88). Too little force: it slips several times a period (the "surface sound": the demo's strongest
frequency is the octave). Too much: it grinds and goes flat (Schelleng's limit, 2.0 N with the defaults; measured: clean
up to 1.9 N). The loss is per trip to the bridge, like a real bowed instrument's body, so low notes ring longer.

### Gate

```json
{ "type": "gate", "threshold_db": -55, "hysteresis_db": 3, "hold_ms": 50, "release_ms": 100, "apply_after": "v3" }
```

Put the gate first and set `apply_after` to the last gain stage: it decides on the clean input signal
(where hiss is quiet) but mutes after the high-gain stages (where hiss is loud). A gate placed after a
high-gain amp hears the amplified hiss as playing and does not close. Set the threshold above the hiss
peaks (about 10 dB above the average hiss level).

## Tests

```
check.bat                                                     # everything (about 5 minutes): build, engine checks, plugin test
build\EngineTest_artefacts\Release\EngineTest.exe check .     # only the engine checks
build\EngineTest_artefacts\Release\EngineTest.exe record .    # record the presets again (after a change MEANT to change the sound)
```

The engine checks compare the blocks with theory and published data (the tone stack's published formula, a 12AX7's
operating point and harmonics, the feedback loop's equation, power tube datasheet points and output power), check that
`shared/settings.json` describes the blocks as they are (names, defaults, choices), and play every preset against its
recording in `tests/reference/` (a change that is not meant to change the sound fails it). The plugin test drives the
plugin like a DAW: presets, file watching, saved state, the editor, undo, copy/paste, the front panel, the signal view.

## Setting help

Every setting is explained in `shared/settings.json` (range, default, a one-line help and a detailed tooltip).
The plugin shows these as tooltips.
