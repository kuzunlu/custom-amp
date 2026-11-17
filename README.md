# custom-amp

A guitar amp built from a few simple blocks that you can put in any order:

| Block | What it does |
|---|---|
| `eq` | Boost or cut tones (high-pass, low-pass, shelves, peaks) |
| `tonestack` | The real bass/mid/treble circuit of a tube amp, from its part values. The knobs affect each other like in the real amp |
| `gain` | One gain stage: filter, then clip, then filter. Curves: `soft`, `tube`, `diode`, `hard`, `custom` (top and bottom set separately), `triode` (a real 12AX7/12AT7/12AU7 from tube equations). Optional bias shift: hard hits push the stage "colder" for a moment |
| `sag` | Lowers the clipping ceiling of later gain blocks when you play hard |
| `feedback` | Feeds the output of a group of blocks back to its input (like a power amp's feedback loop) |
| `split` | Parallel paths: the same sound through several chains side by side, then mixed |
| `gate` | Noise gate. Can listen at the clean input but mute after a later block (`apply_after`) |
| `ir` | Cabinet sound from an impulse response `.wav` file |

The idea: in the end, every amp is EQ and clipping, plus some feedback and power supply sag.

This is a Python prototype. It works on `.wav` files, not live audio.
A real-time plugin will come later.

## Setup

Needs Python 3.14 and NumPy (see `requirements.txt`).

## Use

Make test audio (a fake guitar riff and a fake cab IR) into `audio/`:

```
python tools/make_test_audio.py
```

Run a recording through a preset:

```
python run.py presets/crunch.json audio/test_di.wav out/crunch.wav
```

The output is set to peak at -1 dBFS. Add `--no-normalize` to keep the raw level.
File paths inside presets (like the IR file) are relative to the folder you run from.

## Presets

A preset is a `.json` file with a `chain` list. Blocks run from top to bottom.
Misspelled settings stop the run with a message that lists the allowed ones.

| Preset | Sound | Distortion of a 220 Hz note: soft / medium / hard playing |
|---|---|---|
| `clean.json` | Bright, scooped clean with lots of headroom | 0.1% / 0.2% / 2.1% |
| `crunch.json` | Mid-forward rock crunch, cleans up when you play softly | 8% / 38% / 45% |
| `high_gain.json` | Tight modern high gain, three preamp stages | 43% / 44% / 44% |

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

### Feedback

```json
{
  "type": "feedback", "amount": 0.3, "oversample": 4,
  "path": [{ "kind": "highshelf", "freq": 3000, "gain_db": -10 }],
  "chain": [{ "type": "gain", "id": "v2", "drive_db": 16, "curve": "soft" }]
}
```

The inner `chain` gets: input minus `amount` times the `path`-filtered output.
More `amount` = less gain, tighter and cleaner clipping.
The `path` filters choose which tones get less feedback, so those come out louder:
a high shelf cut works like a **presence** knob, a low shelf cut works like a **resonance** knob.

- Only `eq`, `tonestack` and `gain` blocks can go inside. Inner gain blocks can not set `oversample`: the whole loop runs at the feedback block's `oversample` rate.
- The loop has no delay. Each sample is found by solving the loop equation, so strong feedback is stable.
- It runs one sample at a time, so it is slow in Python (about 1 s of work per 1 s of audio).

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

Every path gets the same input; the outputs are added. Each path has its own `chain` (any blocks, even another
split or a cab IR), `level_db` and `invert` (flips the path upside down before mixing).
An empty chain passes the input through untouched (a clean blend).

### Triode curve

```json
{ "type": "gain", "curve": "triode", "tube": "12AX7", "plate_v": 300, "plate_ohms": 100e3, "bias_v": -1.5, "drive_db": 6 }
```

A real preamp tube stage, from Koren's tube equations. The input is in volts at the grid (a guitar gives
about 0.1-1 V), so copy the values from a schematic. `bias_v` sets the character: about -1.5 V is a normal
stage (the top clips first, at grid current); -2.5 to -4 V is a "cold clipper" (the bottom squashes early,
lots of 2nd harmonic). Small signals come out the same size; only the clipping shape is the tube's.

### Tone stack

```json
{ "type": "tonestack", "style": "british", "treble": 6, "mid": 4, "bass": 7, "parts": { "R4": 47e3 } }
```

The classic passive tone circuit used by most tube amps, solved from its part values (`ampsim/circuit.py`).
`style` picks the starting values (`british` or `american`); `parts` changes single parts (farads and ohms:
470 pF = `470e-12`, 22 nF = `22e-9`, 33k = `33e3`). The knobs go from 0 to 10. Like the real circuit,
it loses volume (about 5-20 dB), so raise `level_db` or the next stage's drive.

### Gate

```json
{ "type": "gate", "threshold_db": -55, "hysteresis_db": 3, "hold_ms": 50, "release_ms": 100, "apply_after": "v3" }
```

Put the gate first and set `apply_after` to the last gain stage: it decides on the clean input signal
(where hiss is quiet) but mutes after the high-gain stages (where hiss is loud). A gate placed after a
high-gain amp hears the amplified hiss as playing and does not close. Set the threshold above the hiss
peaks (about 10 dB above the average hiss level).

## Setting help

Every setting is explained in `shared/settings.json` (range, default, a one-line help and a detailed tooltip).
The plugin version will show these as tooltips.
