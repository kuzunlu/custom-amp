# custom-amp

A guitar amp built from a few simple blocks that you can put in any order:

| Block | What it does |
|---|---|
| `eq` | Boost or cut tones (high-pass, low-pass, shelves, peaks) |
| `gain` | One gain stage: filter, then clip, then filter |
| `sag` | Lowers the clipping ceiling of later gain blocks when you play hard |
| `feedback` | *(coming)* Feeds the output of a group of blocks back to its input |
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
See `presets/crunch.json`. Misspelled settings stop the run with a message that lists the allowed ones.

### Sag

```json
{ "type": "sag", "amount": 0.3, "sens_db": 0, "attack_ms": 5, "release_ms": 150, "targets": ["v2"] }
```

The sag block listens to the signal where it sits. The sound passes through it unchanged.
When the signal gets loud, the gain blocks in `targets` get a lower clipping ceiling, so they clip earlier.

- `amount`: the most the ceiling can drop (0.3 = down to 70%). Must be below 1.
- `sens_db`: how easily it reacts. Higher = reacts to softer playing.
- `attack_ms` / `release_ms`: how fast the ceiling drops, and how fast it comes back.
- `targets`: ids of gain blocks. They must come after the sag block.
