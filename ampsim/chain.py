"""Build an amp from a preset file."""
import json
from pathlib import Path

from .blocks import BLOCK_TYPES, Gain, Sag
from .filters import check_keys


def build_chain(cfgs, sr):
    blocks = []
    for i, cfg in enumerate(cfgs, start=1):
        kind = cfg.get("type")
        if kind not in BLOCK_TYPES:
            raise ValueError(f"block {i}: unknown type '{kind}'. Use one of: {', '.join(BLOCK_TYPES)}")
        blocks.append(BLOCK_TYPES[kind](cfg, sr))
    return blocks


def link_sag(blocks):
    """Connect each sag block to the gain blocks named in its targets."""
    position = {}
    for i, block in enumerate(blocks):
        if block.id is not None:
            if block.id in position:
                raise ValueError(f"two blocks use the id '{block.id}'")
            position[block.id] = i

    for i, block in enumerate(blocks):
        if not isinstance(block, Sag):
            continue
        for target_id in block.target_ids:
            if target_id not in position:
                raise ValueError(f"sag: no block has the id '{target_id}'")
            j = position[target_id]
            if not isinstance(blocks[j], Gain):
                raise ValueError(f"sag: '{target_id}' is not a gain block")
            if j < i:
                raise ValueError(f"sag: '{target_id}' must come after the sag block in the chain")
            block.targets.append(blocks[j])


class Amp:
    def __init__(self, preset, sr):
        check_keys(preset, {"name", "notes", "chain"}, "preset")
        self.name = preset.get("name", "")
        self.blocks = build_chain(preset["chain"], sr)
        link_sag(self.blocks)

    def process(self, x):
        for block in self.blocks:
            x = block.process(x)
        return x


def load_amp(path, sr):
    return Amp(json.loads(Path(path).read_text(encoding="utf-8")), sr)
