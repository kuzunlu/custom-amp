"""Check a preset's front panel: the knobs the plugin shows, each one turning a number setting of a block.

The panel does not change the sound, so Python only checks it (the C++ plugin uses it, with the same messages).
"""
import json
from pathlib import Path

from .filters import check_keys

MAX_KNOBS = 8
KNOB_KEYS = {"label", "block", "setting", "range"}
SETTINGS_FILE = Path(__file__).resolve().parent.parent / "shared" / "settings.json"
_doc = None


def settings_doc():
    global _doc
    if _doc is None:
        _doc = json.loads(SETTINGS_FILE.read_text(encoding="utf-8"))
    return _doc


def is_number(v):
    return isinstance(v, (int, float)) and not isinstance(v, bool)


def find_block(chain, block_id):
    """The block with this id: in the chain, inside feedback loops and inside split paths."""
    for block in chain:
        if block.get("id") == block_id:
            return block
        for inner in [block.get("chain", [])] + [p.get("chain", []) for p in block.get("paths", [])]:
            found = find_block(inner, block_id)
            if found is not None:
                return found
    return None


def find_setting(block, setting, doc, where):
    """The settings.json entry of the number setting that `setting` names ("level_db" or "path.0.gain_db")."""
    parts = setting.split(".")
    entries = doc["blocks"][block["type"]]["settings"]
    node = block
    i = 0
    not_a_number = ValueError(f"{where}: '{setting}' is not a number setting of block '{block['id']}'")
    while True:
        entry = entries.get(parts[i])
        if entry is None:
            raise not_a_number
        if entry["type"] == "number":
            if i != len(parts) - 1:
                raise not_a_number
            return entry
        if entry["type"] not in ("band-list", "path-list") or i + 2 >= len(parts):
            raise not_a_number
        items = node.get(parts[i], [])
        index = parts[i + 1]
        if not index.isdigit() or int(index) >= len(items):
            raise ValueError(f"{where}: block '{block['id']}' has no item {index} in '{parts[i]}'")
        node = items[int(index)]
        entries = doc["band" if entry["type"] == "band-list" else "split_path"]["settings"]
        i += 2


def check_panel(panel, chain):
    """Raises ValueError with a clear message when a knob points at nothing or at something that is not a number."""
    doc = settings_doc()
    if not isinstance(panel, list):
        raise ValueError("panel must be a list of knobs")
    if len(panel) > MAX_KNOBS:
        raise ValueError(f"panel has {len(panel)} knobs; at most {MAX_KNOBS} fit")
    for n, knob in enumerate(panel, 1):
        where = f"panel knob {n}"
        if not isinstance(knob, dict):
            raise ValueError(f"{where}: must be an object with 'block' and 'setting'")
        check_keys(knob, KNOB_KEYS, where)
        if not isinstance(knob.get("block"), str) or not isinstance(knob.get("setting"), str):
            raise ValueError(f"{where}: needs 'block' (a block id) and 'setting' (its name)")
        if "label" in knob and not isinstance(knob["label"], str):
            raise ValueError(f"{where}: 'label' must be text")
        if "range" in knob:
            r = knob["range"]
            if not isinstance(r, list) or len(r) != 2 or not all(is_number(v) for v in r) or r[0] == r[1]:
                raise ValueError(f"{where}: 'range' must be two different numbers, like [-40, 0]")
        block = find_block(chain, knob["block"])
        if block is None:
            raise ValueError(f"{where}: no block has the id '{knob['block']}'")
        find_setting(block, knob["setting"], doc, where)
