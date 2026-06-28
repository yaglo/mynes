#!/usr/bin/env python3
"""
Process an RGB image through the real GPU NTSC pipeline.

Step 1 (this script): RGB → NTSC composite waveform (raw float binary)
Step 2 (C tool):      waveform → GPU pipeline → beam RGBA output
Step 3 (this script): beam output → PNG

Usage:
  python3 process_image.py encode <input.png> <output.raw> [lines]
  python3 process_image.py decode <input.raw> <output.png> <width> <height> [bpp]
"""

import numpy as np
from PIL import Image
import sys, os, math, struct

FSC = 3.579545e6
FS  = FSC * 12   # ~42.95 MHz
SPP = 8          # samples per pixel
SPL = 2048       # samples per line

def encode_composite(img_path, out_path, target_lines=240):
    """Convert RGB PNG → NTSC composite waveform (raw float32 binary)."""
    img = np.array(Image.open(img_path).convert('RGB'))
    h_orig, w_orig = img.shape[:2]

    # Scale to 256 wide, target_lines tall (but output always 240 lines for NES compat)
    img = np.array(Image.fromarray(img).resize((256, target_lines), Image.NEAREST))
    h, w = img.shape[:2]
    # Pad to 240 lines if needed (pipeline expects 240)
    if h < 240:
        pad = np.zeros((240 - h, w, 3), dtype=img.dtype)
        img = np.vstack([img, pad])
        h = 240
    print(f"Encoding {w}x{h} → {SPL}x{h} composite waveform")

    rgb = img.astype(np.float32) / 255.0
    Y = 0.299 * rgb[:,:,0] + 0.587 * rgb[:,:,1] + 0.114 * rgb[:,:,2]
    I = 0.596 * rgb[:,:,0] - 0.274 * rgb[:,:,1] - 0.322 * rgb[:,:,2]
    Q = 0.211 * rgb[:,:,0] - 0.523 * rgb[:,:,1] + 0.312 * rgb[:,:,2]

    composite = np.zeros((h, SPL), dtype=np.float32)
    sample_idx = np.arange(SPL)
    dp = 2 * np.pi * FSC / FS

    for y in range(h):
        line_phase = y * np.pi  # 180° per line (NTSC)
        phase = line_phase + dp * sample_idx
        px = np.clip(sample_idx // SPP, 0, w - 1)
        composite[y] = Y[y, px] + I[y, px] * np.cos(phase) + Q[y, px] * np.sin(phase)

    composite.tofile(out_path)
    print(f"Saved {os.path.getsize(out_path)} bytes ({h} lines × {SPL} samples × 4 bytes)")

def decode_beam(raw_path, out_path, width, height, bpp=8):
    """Convert raw beam output (RGBA float16 or RGBA8) → PNG."""
    data = np.fromfile(raw_path, dtype=np.uint8)

    if bpp == 8:
        # Float16x4: 8 bytes per pixel, packed as two uint32 (packHalf2x16)
        # Each pixel = 4 half-floats: R, G, B, A
        halfs = np.frombuffer(data, dtype=np.float16).reshape(height, width, 4)
        rgb = np.clip(halfs[:,:,:3].astype(np.float32) * 255, 0, 255).astype(np.uint8)
    else:
        # RGBA8: 4 bytes per pixel
        rgb = data.reshape(height, width, 4)[:,:,:3]

    Image.fromarray(rgb).save(out_path)
    print(f"Saved {out_path} ({width}x{height})")

if __name__ == '__main__':
    if len(sys.argv) < 2:
        print("Usage:")
        print("  encode <input.png> <output.raw> [lines=240]")
        print("  decode <input.raw> <output.png> <width> <height> [bpp=8]")
        sys.exit(1)

    cmd = sys.argv[1]
    if cmd == 'encode':
        lines = int(sys.argv[4]) if len(sys.argv) > 4 else 240
        encode_composite(sys.argv[2], sys.argv[3], lines)
    elif cmd == 'decode':
        bpp = int(sys.argv[6]) if len(sys.argv) > 6 else 8
        decode_beam(sys.argv[2], sys.argv[3], int(sys.argv[4]), int(sys.argv[5]), bpp)
