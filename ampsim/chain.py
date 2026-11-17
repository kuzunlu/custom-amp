"""Build an amp from a preset file."""
import json
from pathlib import Path

from .blocks import Feedback, Gain, Gate, Power, Sag, build_chain, run_blocks
from .filters import check_keys


def flatten(blocks):
    """All blocks in the order they run. Blocks inside a feedback loop or split come right after it."""
    flat = []
    for block in blocks:
        flat.append(block)
        flat.extend(flatten(block.children()))
    return flat


def link_blocks(blocks):
    """Connect sag blocks to their targets and gate blocks to their apply_after block."""
    blocks = flatten(blocks)
    in_loops = {id(inner) for loop in blocks if isinstance(loop, Feedback) for inner in flatten(loop.children())}

    position = {}
    for i, block in enumerate(blocks):
        if block.id is not None:
            if block.id in position:
                raise ValueError(f"two blocks use the id '{block.id}'")
            position[block.id] = i

    def find(kind, i, target_id):
        if target_id not in position:
            raise ValueError(f"{kind}: no block has the id '{target_id}'")
        j = position[target_id]
        if j < i:
            raise ValueError(f"{kind}: '{target_id}' must come after the {kind} block in the chain")
        return blocks[j]

    for i, block in enumerate(blocks):
        if isinstance(block, Sag):
            for target_id in block.target_ids:
                target = find("sag", i, target_id)
                if not isinstance(target, (Gain, Power)):
                    raise ValueError(f"sag: '{target_id}' is not a gain or power block")
                block.targets.append(target)
        elif isinstance(block, Gate) and block.apply_after is not None:
            target = find("gate", i, block.apply_after)
            if id(target) in in_loops:
                raise ValueError(f"gate: '{block.apply_after}' is inside a feedback loop; use the feedback block's id")
            block.target = target


class Amp:
    def __init__(self, preset, sr):
        check_keys(preset, {"name", "notes", "chain"}, "preset")
        self.name = preset.get("name", "")
        self.blocks = build_chain(preset["chain"], sr)
        link_blocks(self.blocks)

    def process(self, x):
        return run_blocks(self.blocks, x)


def load_amp(path, sr):
    return Amp(json.loads(Path(path).read_text(encoding="utf-8")), sr)
