# custom-amp

A guitar amp built from a few simple blocks that you can put in any order:

| Block | What it does |
|---|---|
| `eq` | Boost or cut tones (high-pass, low-pass, shelves, peaks) |
| `gain` | One gain stage: filter, then clip, then filter. Optional bias shift: hard hits push the stage "colder" for a moment |
| `sag` | Lowers the clipping ceiling of later gain blocks when you play hard |
| `feedback` | Feeds the output of a group of blocks back to its input (like a power amp's feedback loop) |
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

- Only `eq` and `gain` blocks can go inside. Inner gain blocks can not set `oversample`: the whole loop runs at the feedback block's `oversample` rate.
- The loop has no delay. Each sample is found by solving the loop equation, so strong feedback is stable.
- It runs one sample at a time, so it is slow in Python (about 1 s of work per 1 s of audio).

## Setting help

Every setting is explained in `shared/settings.json` (range, default, a one-line help and a detailed tooltip).
The plugin version will show these as tooltips.
