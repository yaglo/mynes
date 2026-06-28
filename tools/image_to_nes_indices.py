#!/usr/bin/env python3
"""
Convert a reference PNG/JPG (a CROP of an NES frame, not the full frame)
to a 256x240 NES palette-index framebuffer.

The source is pixel art rendered at integer scale with SQUARE NES
pixels, so preserving aspect matters. We:

  1. Detect the integer pixel-block scale N of the source (largest N
     under the JPG-noise tolerance) and downsample by N — this recovers
     the native NES-pixel content at its real pixel count.
  2. Fit that content inside a 256x240 NES frame by centering it and
     letterboxing with palette black (index 0x0F) on whichever axis
     is short. No anisotropic stretching; no blur.

Each pixel is quantized to the nearest 2C02 palette entry using
luma-weighted RGB distance in gamma-2 linear space.

Output: raw binary 256*240 = 61440 bytes (uint8 palette indices, 0..63).
"""
import sys
from PIL import Image
import numpy as np

PPU_W = 256
PPU_H = 240

PAL_2C02 = [
    (84,84,84),    (0,30,116),    (8,16,144),    (48,0,136),
    (68,0,100),    (92,0,48),     (84,4,0),      (60,24,0),
    (32,42,0),     (8,58,0),      (0,64,0),      (0,60,0),
    (0,50,60),     (0,0,0),       (0,0,0),       (0,0,0),
    (152,150,152), (8,76,196),    (48,50,236),   (92,30,228),
    (136,20,176),  (160,20,100),  (152,34,32),   (120,60,0),
    (84,90,0),     (40,114,0),    (8,124,0),     (0,118,40),
    (0,102,120),   (0,0,0),       (0,0,0),       (0,0,0),
    (236,238,236), (76,154,236),  (120,124,236), (176,98,236),
    (228,84,236),  (236,88,180),  (236,106,100), (212,136,32),
    (160,170,0),   (116,196,0),   (76,208,32),   (56,204,108),
    (56,180,204),  (60,60,60),    (0,0,0),       (0,0,0),
    (236,238,236), (168,204,236), (188,188,236), (212,178,236),
    (236,174,236), (236,174,212), (236,180,176), (228,196,144),
    (204,210,120), (180,222,120), (168,226,144), (152,226,180),
    (160,214,228), (160,162,160), (0,0,0),       (0,0,0),
]

USABLE = [i for i in range(64) if (i & 0x0F) not in (0x0E, 0x0F)]
BLACK_IDX = 0x0F  # $0F decodes to (0,0,0); safe letterbox fill

def detect_block_scale(img):
    """Return the largest integer N such that the image downsamples
    uniformly by N and upsamples back with NEAREST within tolerance —
    i.e. the native pixel-block size of the source. Pixel art scaled
    by an integer gives near-zero reconstruction error at that N."""
    arr = np.asarray(img, dtype=np.int16)
    W, H = img.size
    TOL = 8.0
    best = 1
    for n in range(1, 8):
        nw, nh = W // n, H // n
        if nw < 32 or nh < 32:
            continue
        small = img.resize((nw, nh), Image.BOX)
        big = small.resize((nw*n, nh*n), Image.NEAREST)
        a = np.asarray(big, dtype=np.int16)
        err = float(np.mean(np.abs(arr[:nh*n, :nw*n] - a)))
        if err <= TOL:
            best = n
    return best

def srgb_to_linear(u8):
    v = u8 / 255.0
    return v * v

def nearest_nes_array(rgb_arr):
    pal = np.array(PAL_2C02, dtype=np.float32) / 255.0
    pal_lin = pal * pal
    px = rgb_arr.astype(np.float32).reshape(-1, 3) / 255.0
    px_lin = px * px
    out = np.zeros(px.shape[0], dtype=np.uint8)
    # Weighted dist: emphasize G for luminance sensitivity.
    w = np.array([2.0, 4.0, 3.0], dtype=np.float32)
    for i, p in enumerate(px_lin):
        d = ((pal_lin[USABLE] - p) ** 2 * w).sum(axis=1)
        j = int(np.argmin(d))
        out[i] = USABLE[j]
    return out

def main():
    if len(sys.argv) < 3:
        print("Usage: image_to_nes_indices.py <input> <output.bin> [preview.png]")
        sys.exit(1)
    src = Image.open(sys.argv[1]).convert('RGB')
    W, H = src.size
    n = detect_block_scale(src)
    nw, nh = W // n, H // n
    print(f"Source: {W}x{H}, native pixel size {n}×{n} → content {nw}x{nh} NES px")

    # Recover native NES-pixel content at square aspect.
    content = src.resize((nw, nh), Image.BOX)
    # Scale down if native content exceeds 256×240 (the source was at a
    # larger source resolution than one NES frame could ever render).
    if nw > PPU_W or nh > PPU_H:
        s = min(PPU_W / nw, PPU_H / nh)
        nw2, nh2 = max(1, int(round(nw * s))), max(1, int(round(nh * s)))
        content = content.resize((nw2, nh2), Image.BOX)
        nw, nh = nw2, nh2
        print(f"Content exceeded NES frame; scaled to {nw}x{nh} preserving aspect")

    # Letterbox-center inside 256x240.
    canvas = Image.new('RGB', (PPU_W, PPU_H), (0, 0, 0))
    off_x, off_y = (PPU_W - nw) // 2, (PPU_H - nh) // 2
    canvas.paste(content, (off_x, off_y))
    print(f"Letterboxed in 256x240 at offset ({off_x},{off_y})")

    arr = np.asarray(canvas, dtype=np.uint8)
    idx = nearest_nes_array(arr).reshape(PPU_H, PPU_W)
    # Force letterbox cells to flat black ($0F) — avoids quantized
    # near-blacks turning into noisy composite colors.
    if off_x > 0:
        idx[:, :off_x] = BLACK_IDX
        idx[:, off_x + nw:] = BLACK_IDX
    if off_y > 0:
        idx[:off_y, :] = BLACK_IDX
        idx[off_y + nh:, :] = BLACK_IDX

    with open(sys.argv[2], 'wb') as f:
        f.write(idx.tobytes())
    print(f"Wrote {idx.size} bytes to {sys.argv[2]}")

    if len(sys.argv) >= 4:
        pal = np.array(PAL_2C02, dtype=np.uint8)
        preview = pal[idx]
        Image.fromarray(preview).save(sys.argv[3])
        print(f"Preview: {sys.argv[3]}")

if __name__ == '__main__':
    main()
