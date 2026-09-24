"""Pixel work done outside ffmpeg: Y'CbCr to 16-bit R'G'B' for HDR frames,
1:1 crops, 16-bit PNG output and HDR light levels.

Pillow has no 16-bit RGB mode (it opens a 48-bit PNG as 8-bit RGB), so the
HDR side works on numpy arrays. ffmpeg hands over an HDR frame as the raw
Y'CbCr planes its decoder produces, and yuv_to_rgb48 converts them here.
swscale's YCbCr to rgb48 conversion maps limited-range white to 65280 where
65535 is correct, which lowers every PQ code by 256/257. Nothing here
changes a picture's size: every file is cut 1:1 from a render.
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


# Kr and Kb of each Y'CbCr matrix, by the swscale names recipes.sws_matrix uses.
LUMA_WEIGHTS = {"bt2020": (0.2627, 0.0593), "bt709": (0.2126, 0.0722), "bt601": (0.299, 0.114)}


def read_yuv444(path: Path | str, size: Sequence[int]) -> np.ndarray:
    """A raw planar 4:4:4 frame of 10 to 16 bits (yuv444p10le, yuv444p12le,
    yuv444p16le) as a (3, h, w) uint16 array: Y', Cb, Cr."""
    w, h = size
    data = np.fromfile(path, dtype="<u2")
    if data.size != w * h * 3:
        raise ValueError(f"{path}: {data.size * 2} bytes, expected {w * h * 6} for {w}x{h} planar 4:4:4")
    return data.reshape(3, h, w)


def _weights(matrix: str) -> tuple[float, float, float]:
    if matrix not in LUMA_WEIGHTS:
        raise ValueError(f"no Y'CbCr weights for matrix {matrix!r}")
    kr, kb = LUMA_WEIGHTS[matrix]
    return kr, 1.0 - kr - kb, kb


def yuv_to_rgb48(yuv: np.ndarray, depth: int, matrix: str = "bt2020", full_range: bool = False) -> np.ndarray:
    """Y'CbCr samples of ``depth`` bits to 16-bit R'G'B' codes, (h, w, 3) uint16.

    Limited range (ITU-R BT.2100) puts Y' black and white at 16 and 235 times
    2^(depth-8) and Cb and Cr at 128 plus or minus 112 times 2^(depth-8), so a
    12-bit Y' of 3760 with neutral chroma is 1.0 and becomes 65535. Full range
    uses 0 to 2^depth-1 with chroma centred on 2^(depth-1). Values outside
    0..1 are clipped, and each is rounded to the nearest code."""
    kr, kg, kb = _weights(matrix)
    y, cb, cr = (p.astype(np.float32) for p in yuv)
    if full_range:
        top = float((1 << depth) - 1)
        y, cb, cr = y / top, (cb - (1 << (depth - 1))) / top, (cr - (1 << (depth - 1))) / top
    else:
        step = float(1 << (depth - 8))
        y, cb, cr = (y - 16 * step) / (219 * step), (cb - 128 * step) / (224 * step), (cr - 128 * step) / (224 * step)
    rgb = np.empty(y.shape + (3,), np.float32)
    rgb[..., 0] = y + 2 * (1 - kr) * cr
    rgb[..., 1] = y - (2 * kb * (1 - kb) / kg) * cb - (2 * kr * (1 - kr) / kg) * cr
    rgb[..., 2] = y + 2 * (1 - kb) * cb
    np.clip(rgb, 0.0, 1.0, out=rgb)
    return np.floor(rgb * 65535 + 0.5).astype(np.uint16)


def rgb48_to_yuv(rgb: np.ndarray, depth: int = 16, matrix: str = "bt2020") -> np.ndarray:
    """16-bit R'G'B' codes to limited-range Y'CbCr of ``depth`` bits, (3, h, w)
    uint16: the inverse of yuv_to_rgb48 and the way the recorder writes HDR
    frames (the tests draw their HDR renders with it)."""
    kr, kg, kb = _weights(matrix)
    e = rgb.astype(np.float64) / 65535.0
    y = kr * e[..., 0] + kg * e[..., 1] + kb * e[..., 2]
    cb = (e[..., 2] - y) / (2 * (1 - kb))
    cr = (e[..., 0] - y) / (2 * (1 - kr))
    step = float(1 << (depth - 8))
    planes = (16 * step + 219 * step * y, 128 * step + 224 * step * cb, 128 * step + 224 * step * cr)
    return np.stack([np.floor(p + 0.5) for p in planes]).astype(np.uint16)


def crop(rgb: np.ndarray, rect: Rect) -> np.ndarray:
    h, w = rgb.shape[:2]
    if rect.x < 0 or rect.y < 0 or rect.x + rect.w > w or rect.y + rect.h > h:
        raise ValueError(f"crop {rect} leaves the {w}x{h} frame")
    return rgb[rect.y:rect.y + rect.h, rect.x:rect.x + rect.w]


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


def srgb_info():
    """The chunk an SDR PNG carries: sRGB with the perceptual intent, no ICC
    profile, so browsers show the renderer's values as sRGB."""
    from PIL.PngImagePlugin import PngInfo
    info = PngInfo()
    info.add(b"sRGB", b"\x00")
    return info


def sdr_png(src: Path | str, out: Path | str) -> tuple[int, int]:
    """An 8-bit RGB PNG rewritten lossless with the sRGB chunk and nothing
    else (ffmpeg's PNGs have no colour chunk)."""
    from PIL import Image
    with Image.open(src) as im:
        rgb = im.convert("RGB")
        rgb.save(out, optimize=True, pnginfo=srgb_info())
        return rgb.size


def sdr_crop(src: Path | str, out: Path | str, rect: Rect) -> tuple[int, int]:
    """1:1 crop of an 8-bit PNG, saved lossless with the sRGB chunk."""
    from PIL import Image
    with Image.open(src) as im:
        part = im.convert("RGB").crop((rect.x, rect.y, rect.x + rect.w, rect.y + rect.h))
        part.save(out, optimize=True, pnginfo=srgb_info())
        return part.size


def png_chunk_types(path: Path | str) -> list[str]:
    """The chunk types of a PNG file, in order."""
    import struct
    out = []
    with open(path, "rb") as f:
        if f.read(8) != b"\x89PNG\r\n\x1a\n":
            raise ValueError(f"{path}: not a PNG")
        while True:
            head = f.read(8)
            if len(head) < 8:
                break
            length, kind = struct.unpack(">I4s", head)
            out.append(kind.decode("latin-1"))
            f.seek(length + 4, 1)
    return out
