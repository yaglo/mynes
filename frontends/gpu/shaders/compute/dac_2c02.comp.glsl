/*
 * 2C02 Video DAC — GPU Compute Shader
 * =====================================
 *
 * Converts NES PPU palette+emphasis indices into composite waveform
 * voltages using Bisqwit's 2C02 voltage model. This is Stage 1 of
 * the physical signal chain — the point where digital palette indices
 * become an analog composite signal.
 *
 * Input:  256×240 uint16 palette index buffer
 *         (bits 0..5 = palette index, bits 6..8 = emphasis)
 *         + precomputed signal table (512 entries × 12 phases)
 *
 * Output: 2048×240 (NTSC) or 2560×240 (PAL) float waveform buffer
 *         (8 or 10 samples per NES pixel × 256 pixels per scanline)
 *
 * Each thread handles one NES pixel: reads palette+emphasis, looks up
 * the signal table, and writes `samples_per_pixel` float samples to
 * the output waveform at the correct phase offset.
 *
 * Dispatch: 256 threads per workgroup, 240 workgroups (one per scanline).
 * Total: 256 × 240 = 61,440 threads.
 */

#version 450

layout(local_size_x = 256) in;

/* Palette index buffer: 256×240 uint16 (packed as uint32 pairs). */
layout(set = 0, binding = 0) readonly buffer IndexBuf {
    uint index_data[];  /* uint16 packed: 2 pixels per uint32 */
};

/* Precomputed signal table: 512 entries × 24 floats per entry.
 * Entries indexed by (emph << 6) | palette_idx.
 * 24 floats = 12 phases duplicated (so 8- or 10-sample reads
 * starting at any phase 0..11 stay in bounds). */
layout(set = 0, binding = 1) readonly buffer SignalTable {
    float signal_table[];  /* [entry * 24 + phase] */
};

/* Optional alt signal table: PAL uses it on odd scanlines to model
 * the 2C07's per-line V-phase inversion. NTSC never reads it (shader
 * gates on use_alt_table), but the binding is always present so the
 * descriptor layout stays constant across regions. */
layout(set = 0, binding = 2) readonly buffer SignalTableAlt {
    float signal_table_alt[];
};

/* Output waveform buffer: samples_per_line × 240 floats. */
layout(set = 1, binding = 0) writeonly buffer WaveformBuf {
    float waveform[];
};

layout(set = 1, binding = 1) writeonly buffer LumaBuf { float source_y[]; };

layout(set = 2, binding = 0) uniform Params {
    uint  samples_per_pixel;   /* 8 (NTSC) or 10 (PAL) */
    uint  samples_per_line;    /* 2048 (NTSC) or 2560 (PAL) */
    uint  phase_base;          /* starting phase for this frame (dot crawl) */
    uint  phase_line_adv;      /* phase advance per scanline */
    uint  phase_field_adv;     /* phase advance per frame (not used here, CPU tracks) */
    uint  frame_field;         /* field index within dot crawl cycle */
    uint  use_alt_table;       /* 1 = PAL (alternate odd lines), 0 = NTSC */
    uint separate_yc;
};

void main() {
    /* Each thread handles one NES pixel.
     * gl_WorkGroupID.y = scanline (0..239)
     * gl_LocalInvocationID.x = pixel within scanline (0..255) */
    uint px = gl_LocalInvocationID.x;
    uint sy = gl_WorkGroupID.y;

    if (px >= 256 || sy >= 240) return;

    /* Read palette index + emphasis from the uint16 buffer.
     * Stored as uint32 with 2 pixels packed — extract the right half. */
    uint flat_idx = sy * 256 + px;
    uint packed = index_data[flat_idx / 2];
    uint pixel_val = (flat_idx & 1u) == 0u
                     ? (packed & 0xFFFFu)
                     : (packed >> 16u);

    /* 9-bit entry: bits 0..5 = palette, bits 6..8 = emphasis. */
    uint entry = pixel_val & 0x1FFu;

    /* Compute phase offset for this pixel on this scanline.
     * Phase advances by samples_per_pixel per NES pixel (8 NTSC, 10 PAL).
     * The subcarrier has 12 slots per cycle, so phase wraps mod 12. */
    uint line_phase = (phase_base + sy * phase_line_adv) % 12u;
    uint pixel_phase = (line_phase + px * samples_per_pixel) % 12u;

    /* Look up the signal table and emit samples.
     * The table stores 24 floats per entry (12 phases × 2 for wraparound),
     * so reading `samples_per_pixel` values starting at pixel_phase is safe. */
    uint table_base = entry * 24u + pixel_phase;
    uint wave_base = sy * samples_per_line + px * samples_per_pixel;

    /* PAL sampling uses the alt table on odd scanlines: the 2C07
     * inverts the chroma V-phase every other line, so odd lines must
     * decode a hue remapping that was baked into signal_table_alt at
     * precompute time. NTSC sets use_alt_table=0 and always samples
     * signal_table, keeping the hot loop branch-free on that path. */
    bool alt = (use_alt_table != 0u) && ((sy & 1u) == 1u);
    // Ideal separated-output modification: DC and AC components of the
    // same DAC code. This is a voltage-cycle mean, never an RGB palette.
    float dc = 0.0;
    if (separate_yc != 0u) {
        for (uint p=0u; p<12u; p++)
            dc += (alt ? signal_table_alt[entry*24u+p] : signal_table[entry*24u+p]) / 12.0;
    }
    for (uint s = 0; s < samples_per_pixel; s++) {
        if (separate_yc != 0u) source_y[wave_base+s] = dc;
        waveform[wave_base + s] = alt
            ? signal_table_alt[table_base + s]
            : signal_table[table_base + s];
    }
}
