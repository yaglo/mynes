"""Pixel work done outside ffmpeg: 1:1 crops of 16-bit PQ frames, the exact
2x2 box average for the @1x variants, 16-bit PNG output and HDR light levels.

Pillow has no 16-bit RGB mode (it opens a 48-bit PNG as 8-bit RGB), so the
HDR side works on numpy arrays of the raw rgb48le frames ffmpeg writes. The
SDR side uses Pillow's Image.reduce(2); box_average_2x2 computes the same
rounded mean, (a + b + c + d + 2) // 4, on 16-bit values.
"""
from __future__ import annotations

import struct
import zlib
from pathlib import Path
from typing import Sequence

import numpy as np

from .recipes import Rect

# SMPTE ST 2084 constants.
PQ_M1 = 2610 / 16384
PQ_M2 = 2523 / 4096 * 128
PQ_C1 = 3424 / 4096
PQ_C2 = 2413 / 4096 * 32
PQ_C3 = 2392 / 4096 * 32
PQ_PEAK = 10000.0


def read_rgb48(path: Path | str, size: Sequence[int]) -> np.ndarray:
    """A raw rgb48le frame as an (h, w, 3) uint16 array."""
    w, h = size
    data = np.fromfile(path, dtype="<u2")
    if data.size != w * h * 3:
        raise ValueError(f"{path}: {data.size * 2} bytes, expected {w * h * 6} for {w}x{h} rgb48le")
    return data.reshape(h, w, 3)


def crop(rgb: np.ndarray, rect: Rect) -> np.ndarray:
    h, w = rgb.shape[:2]
    if rect.x < 0 or rect.y < 0 or rect.x + rect.w > w or rect.y + rect.h > h:
        raise ValueError(f"crop {rect} leaves the {w}x{h} frame")
    return rgb[rect.y:rect.y + rect.h, rect.x:rect.x + rect.w]


def box_average_2x2(rgb: np.ndarray) -> np.ndarray:
    """Each output pixel is the rounded mean of a 2x2 block, as Image.reduce(2)."""
    h, w = rgb.shape[:2]
    if h % 2 or w % 2:
        raise ValueError(f"the 2x2 average needs even dimensions, got {w}x{h}")
    blocks = rgb.astype(np.uint32).reshape(h // 2, 2, w // 2, 2, -1)
    return ((blocks.sum(axis=(1, 3)) + 2) // 4).astype(rgb.dtype)


def _chunk(tag: bytes, data: bytes) -> bytes:
    return struct.pack(">I", len(data)) + tag + data + struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF)


def write_png16(path: Path | str, rgb: np.ndarray, *, cicp: Sequence[int] | None = (9, 16, 0, 1),
                level: int = 3) -> None:
    """16-bit RGB PNG. The cICP chunk (BT.2020, PQ, RGB, full range by
    default) lets viewers that read it show the file as HDR."""
    h, w = rgb.shape[:2]
    rows = np.zeros((h, 1 + w * 6), dtype=np.uint8)  # filter byte 0 (None) per row
    rows[:, 1:] = np.ascontiguousarray(rgb, dtype=">u2").view(np.uint8).reshape(h, w * 6)
    png = b"\x89PNG\r\n\x1a\n" + _chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 16, 2, 0, 0, 0))
    if cicp:
        png += _chunk(b"cICP", bytes(cicp))
    png += _chunk(b"IDAT", zlib.compress(rows.tobytes(), level)) + _chunk(b"IEND", b"")
    Path(path).write_bytes(png)


def pq_to_nits(code: np.ndarray) -> np.ndarray:
    """PQ code values (uint16 full range) to absolute luminance in nits."""
    e = np.clip(code.astype(np.float64) / 65535.0, 0.0, 1.0) ** (1.0 / PQ_M2)
    return PQ_PEAK * (np.maximum(e - PQ_C1, 0.0) / (PQ_C2 - PQ_C3 * e)) ** (1.0 / PQ_M1)


def nits_to_pq(nits: np.ndarray | float) -> np.ndarray:
    """Absolute luminance to PQ signal 0..1 (the tests draw HDR frames with it)."""
    y = (np.clip(np.asarray(nits, dtype=np.float64), 0.0, PQ_PEAK) / PQ_PEAK) ** PQ_M1
    return ((PQ_C1 + PQ_C2 * y) / (1.0 + PQ_C3 * y)) ** PQ_M2


def light_levels(rgb: np.ndarray) -> tuple[int, int]:
    """MaxCLL and MaxFALL (CTA-861.3) of one PQ frame: the brightest pixel's
    largest component, and the frame mean of each pixel's largest component."""
    lut = pq_to_nits(np.arange(65536, dtype=np.uint32))
    peak = lut[rgb.max(axis=2)]
    return int(np.ceil(peak.max())), int(np.ceil(peak.mean()))


def sdr_crop(src: Path | str, out: Path | str, rect: Rect) -> tuple[int, int]:
    """1:1 crop of an 8-bit PNG, saved lossless."""
    from PIL import Image
    with Image.open(src) as im:
        part = im.convert("RGB").crop((rect.x, rect.y, rect.x + rect.w, rect.y + rect.h))
        part.save(out, optimize=True)
        return part.size


def sdr_reduce(src: Path | str, out: Path | str) -> tuple[int, int]:
    """The @1x variant: Image.reduce(2), the exact 2x2 box average."""
    from PIL import Image
    with Image.open(src) as im:
        if im.width % 2 or im.height % 2:
            raise ValueError(f"{src}: the 2x2 average needs even dimensions, got {im.width}x{im.height}")
        small = im.convert("RGB").reduce(2)
        small.save(out, optimize=True)
        return small.size
