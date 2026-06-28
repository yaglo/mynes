#!/usr/bin/env python3
"""
NTSC Composite Pipeline — Offline Image Processor (numpy-vectorized)
"""
import numpy as np
from PIL import Image
import sys, os, math

FSC = 3.579545e6
FS  = FSC * 12
SPP = 8
SPL = 2048

def design_fir(n, cutoff, ringing=0.0):
    half = n // 2
    k = np.arange(n)
    m = k - half
    sinc = np.where(m == 0, 2.0 * cutoff,
                    np.sin(2 * np.pi * cutoff * m) / (np.pi * m))
    hamming = 0.54 - 0.46 * np.cos(2 * np.pi * k / (n - 1))
    w = hamming * (1 - ringing) + ringing
    taps = sinc * w
    return taps / taps.sum()

def fir_per_line(signal, taps):
    """FIR filter each scanline independently (vectorized via np.convolve)."""
    out = np.zeros_like(signal)
    for y in range(signal.shape[0]):
        out[y] = np.convolve(signal[y], taps, mode='same')
    return out

def rgb_to_composite(img):
    """Vectorized RGB → NTSC composite encoding."""
    h, w = img.shape[:2]
    rgb = img.astype(np.float32) / 255.0

    Y = 0.299 * rgb[:,:,0] + 0.587 * rgb[:,:,1] + 0.114 * rgb[:,:,2]
    I = 0.596 * rgb[:,:,0] - 0.274 * rgb[:,:,1] - 0.322 * rgb[:,:,2]
    Q = 0.211 * rgb[:,:,0] - 0.523 * rgb[:,:,1] + 0.312 * rgb[:,:,2]

    composite = np.zeros((h, SPL), dtype=np.float32)
    sample_idx = np.arange(SPL)
    dp = 2 * np.pi * FSC / FS

    for y in range(h):
        line_phase = y * np.pi
        phase = line_phase + dp * sample_idx
        # Map each sample to a source pixel
        px = np.clip(sample_idx // SPP, 0, w - 1)
        composite[y] = Y[y, px] + I[y, px] * np.cos(phase) + Q[y, px] * np.sin(phase)

    return composite

def demod_chroma(composite, chroma_bw=0.6e6):
    h = composite.shape[0]
    sample_idx = np.arange(SPL)
    dp = 2 * np.pi * FSC / FS

    I_raw = np.zeros_like(composite)
    Q_raw = np.zeros_like(composite)
    for y in range(h):
        phase = y * np.pi + dp * sample_idx
        I_raw[y] = composite[y] * np.cos(phase) * 2.0
        Q_raw[y] = composite[y] * np.sin(phase) * 2.0

    c_cutoff = chroma_bw / (FS / 2)
    c_fir = design_fir(49, c_cutoff)
    return fir_per_line(I_raw, c_fir), fir_per_line(Q_raw, c_fir)

def matrix_decode(Y, I, Q, color_temp=6500, r_drive=1.0, g_drive=1.0, b_drive=1.0,
                  saturation=1.0, chroma_gain=1.2, hue_offset=0.0):
    t = color_temp / 6500.0
    wr, wg, wb = 1.0 + (1.0 - t) * 0.3, 1.0, 1.0 - (1.0 - t) * 0.3
    h = math.radians(hue_offset)
    ch, sh = math.cos(h), math.sin(h)
    cg = chroma_gain * saturation
    Ir = I * ch - Q * sh
    Qr = I * sh + Q * ch
    R = Y + (0.956 * Ir + 0.621 * Qr) * cg * r_drive * wr
    G = Y + (-0.272 * Ir - 0.647 * Qr) * cg * g_drive * wg
    B = Y + (-1.106 * Ir + 1.703 * Qr) * cg * b_drive * wb
    return np.clip(R, 0, 1), np.clip(G, 0, 1), np.clip(B, 0, 1)

def apply_beam(R, G, B, rps=4, sigma_narrow=0.20, sigma_wide=0.65,
               h_blur=5.0, bloom_gamma=1.5, noise=0.0, black_floor=0.02,
               conv_r_x=0, conv_b_x=0):
    """Vectorized beam profile."""
    h, w = R.shape
    out_w = 768  # 3x native width, ~4:3 with rps=3 (768x672)
    out_h = h * rps
    result = np.zeros((out_h, out_w, 3), dtype=np.float32)

    # H-blur at signal resolution
    if h_blur > 0.5:
        rad = int(math.ceil(h_blur * 2))
        kernel = np.exp(-np.arange(-rad, rad+1)**2 / (2 * h_blur**2))
        kernel /= kernel.sum()
        R = fir_per_line(R, kernel)
        G = fir_per_line(G, kernel)
        B = fir_per_line(B, kernel)

    # Precompute x mapping
    sx_map = np.clip((np.arange(out_w) + 0.5) * w / out_w, 0, w - 1).astype(int)
    r_sx_map = np.clip(sx_map + int(conv_r_x), 0, w - 1)
    b_sx_map = np.clip(sx_map + int(conv_b_x), 0, w - 1)

    for oy in range(out_h):
        sy = oy // rps
        sub = oy % rps
        if sy >= h:
            continue
        d = (sub + 0.5) / rps - 0.5

        row_r = np.zeros(out_w, dtype=np.float32)
        row_g = np.zeros(out_w, dtype=np.float32)
        row_b = np.zeros(out_w, dtype=np.float32)

        for soff in [-1, 0, 1]:
            line = min(max(sy + soff, 0), h - 1)
            lr = R[line, r_sx_map]
            lg = G[line, sx_map]
            lb = B[line, b_sx_map]

            lY = np.clip(0.299 * lr + 0.587 * lg + 0.114 * lb, 0, 1)
            bt = np.power(lY, bloom_gamma)
            sv = sigma_narrow + (sigma_wide - sigma_narrow) * bt
            inv2s = 1.0 / (2.0 * sv * sv)
            vd = d - soff
            gw = np.exp(-(vd * vd) * inv2s)

            row_r += lr * gw
            row_g += lg * gw
            row_b += lb * gw

        if noise > 0:
            luma = 0.299 * row_r + 0.587 * row_g + 0.114 * row_b
            ns = noise * (1 - 0.8 * np.clip(luma, 0, 1))
            row_r += (np.random.random(out_w).astype(np.float32) - 0.5) * ns
            row_g += (np.random.random(out_w).astype(np.float32) - 0.5) * ns
            row_b += (np.random.random(out_w).astype(np.float32) - 0.5) * ns

        result[oy, :, 0] = np.maximum(row_r, black_floor)
        result[oy, :, 1] = np.maximum(row_g, black_floor)
        result[oy, :, 2] = np.maximum(row_b, black_floor)

    return np.clip(result, 0, 1)

PRESETS = {
    "PVM-20M2": dict(
        luma_bw=5.5e6, chroma_bw=1.3e6, luma_taps=37, ringing=0.0,
        color_temp=6500, saturation=1.0, chroma_gain=1.3,
        r_drive=1.0, g_drive=1.0, b_drive=1.0, hue_offset=0,
        sigma_narrow=0.15, sigma_wide=0.55, h_blur=3.0, bloom_gamma=1.8,
        noise=0.0, black_floor=0.005, conv_r_x=0, conv_b_x=0,
        ghost_delay=0, ghost_level=0, rps=3,
    ),
    "Living_Room_1988": dict(
        luma_bw=4.2e6, chroma_bw=0.4e6, luma_taps=37, ringing=0.15,
        color_temp=6500, saturation=1.05, chroma_gain=1.2,
        r_drive=1.0, g_drive=0.98, b_drive=0.95, hue_offset=0,
        sigma_narrow=0.25, sigma_wide=0.65, h_blur=5.0, bloom_gamma=1.5,
        noise=0.02, black_floor=0.02, conv_r_x=2.5, conv_b_x=-2.0,
        ghost_delay=0, ghost_level=0, rps=3,
    ),
    "Basement_TV": dict(
        luma_bw=2.8e6, chroma_bw=0.5e6, luma_taps=37, ringing=0.45,
        color_temp=6500, saturation=1.0, chroma_gain=0.8,
        r_drive=1.02, g_drive=0.96, b_drive=0.93, hue_offset=8,
        sigma_narrow=0.35, sigma_wide=0.75, h_blur=8.0, bloom_gamma=1.3,
        noise=0.06, black_floor=0.04, conv_r_x=6.0, conv_b_x=-5.0,
        ghost_delay=32, ghost_level=0.12, rps=3,
    ),
}

def process_preset(img, name, p):
    print(f"  {name}...")

    composite = rgb_to_composite(img)

    if p['ghost_delay'] > 0 and p['ghost_level'] > 0:
        d = p['ghost_delay']
        for y in range(composite.shape[0]):
            composite[y, d:] += composite[y, :-d] * p['ghost_level']

    y_cutoff = p['luma_bw'] / (FS / 2)
    y_fir = design_fir(p['luma_taps'], y_cutoff, p['ringing'])
    Y = fir_per_line(composite, y_fir)
    I, Q = demod_chroma(composite, chroma_bw=p['chroma_bw'])

    R, G, B = matrix_decode(Y, I, Q,
        color_temp=p['color_temp'], r_drive=p['r_drive'],
        g_drive=p['g_drive'], b_drive=p['b_drive'],
        saturation=p['saturation'], chroma_gain=p['chroma_gain'],
        hue_offset=p['hue_offset'])

    result = apply_beam(R, G, B, rps=p['rps'],
        sigma_narrow=p['sigma_narrow'], sigma_wide=p['sigma_wide'],
        h_blur=p['h_blur'], bloom_gamma=p['bloom_gamma'],
        noise=p['noise'], black_floor=p['black_floor'],
        conv_r_x=p['conv_r_x'], conv_b_x=p['conv_b_x'])

    return (result * 255).astype(np.uint8)

def main():
    input_path = sys.argv[1] if len(sys.argv) > 1 else "/Users/stas/Desktop/top_snes-shot001.png"
    output_dir = sys.argv[2] if len(sys.argv) > 2 else "/tmp/ntsc_output"
    os.makedirs(output_dir, exist_ok=True)

    print(f"Loading {input_path}...")
    img_full = np.array(Image.open(input_path).convert('RGB'))
    img = np.array(Image.fromarray(img_full).resize((256, 224), Image.NEAREST))
    print(f"  Scaled to 256x224")

    Image.fromarray(img).save(os.path.join(output_dir, "00_original.png"))

    for i, (name, p) in enumerate(PRESETS.items(), 1):
        result = process_preset(img, name, p)
        out = os.path.join(output_dir, f"{i:02d}_{name}.png")
        Image.fromarray(result).save(out)
        print(f"    → {out} ({result.shape[1]}x{result.shape[0]})")

if __name__ == '__main__':
    main()
