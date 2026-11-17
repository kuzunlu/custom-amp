"""Read and write .wav files with NumPy only."""
import struct
from pathlib import Path

import numpy as np


def read_wav(path):
    """Return (samples, sample_rate). Samples are float64, shape (frames, channels), range -1..1."""
    data = Path(path).read_bytes()
    if data[:4] != b"RIFF" or data[8:12] != b"WAVE":
        raise ValueError(f"{path}: not a .wav file")

    fmt = raw = None
    pos = 12
    while pos + 8 <= len(data):
        chunk_id = data[pos : pos + 4]
        size = int.from_bytes(data[pos + 4 : pos + 8], "little")
        body = data[pos + 8 : pos + 8 + size]
        if chunk_id == b"fmt ":
            tag, channels, sr = struct.unpack("<HHI", body[:8])
            bits = struct.unpack("<H", body[14:16])[0]
            if tag == 0xFFFE:  # "extensible" format: the real tag is further in
                tag = struct.unpack("<H", body[24:26])[0]
            fmt = (tag, channels, sr, bits)
        elif chunk_id == b"data":
            raw = body
        pos += 8 + size + (size & 1)

    if fmt is None or raw is None:
        raise ValueError(f"{path}: missing fmt or data chunk")
    tag, channels, sr, bits = fmt
    frame_bytes = channels * bits // 8
    raw = raw[: len(raw) - len(raw) % frame_bytes]

    if tag == 1 and bits == 8:
        x = (np.frombuffer(raw, np.uint8).astype(np.float64) - 128) / 128
    elif tag == 1 and bits == 16:
        x = np.frombuffer(raw, "<i2") / 2**15
    elif tag == 1 and bits == 24:
        b = np.frombuffer(raw, np.uint8).reshape(-1, 3).astype(np.int32)
        v = b[:, 0] | (b[:, 1] << 8) | (b[:, 2] << 16)
        x = np.where(v >= 2**23, v - 2**24, v) / 2**23
    elif tag == 1 and bits == 32:
        x = np.frombuffer(raw, "<i4") / 2**31
    elif tag == 3 and bits == 32:
        x = np.frombuffer(raw, "<f4").astype(np.float64)
    elif tag == 3 and bits == 64:
        x = np.frombuffer(raw, "<f8").copy()
    else:
        raise ValueError(f"{path}: unsupported wav format (tag {tag}, {bits} bit)")
    return x.reshape(-1, channels), sr


def write_wav(path, x, sr):
    """Write 24-bit PCM. Values outside -1..1 are clipped."""
    x = np.asarray(x, dtype=np.float64)
    if x.ndim == 1:
        x = x[:, None]
    channels = x.shape[1]
    ints = np.round(np.clip(x, -1.0, 1.0) * (2**23 - 1)).astype("<i4")
    raw = ints.view(np.uint8).reshape(-1, 4)[:, :3].tobytes()
    pad = len(raw) & 1
    header = struct.pack(
        "<4sI4s4sIHHIIHH4sI",
        b"RIFF", 36 + len(raw) + pad, b"WAVE",
        b"fmt ", 16, 1, channels, sr, sr * channels * 3, channels * 3, 24,
        b"data", len(raw),
    )
    Path(path).write_bytes(header + raw + b"\0" * pad)
