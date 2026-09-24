/*
 * 2C02 Video DAC — GPU Compute Shader
 * =====================================
 *
 * Converts NES PPU palette+emphasis indices into composite waveform
 * voltages using the uploaded measured DAC rails. This is Stage 1 of
 * the physical signal chain — the point where digital palette indices
 * become an analog composite signal.
 *
 * Input:  256×240 uint16 palette index buffer
 *         (bits 0..5 = palette index, bits 6..8 = emphasis)
 *         + precomputed signal table (512 entries × 24 floats)
 *
 * Output: 2048×240 (NTSC) or 2560×240 (PAL) float waveform buffer
 *         (8 or 10 samples per NES pixel × 256 pixels per scanline), which
 *         the raster stage places on the full line, followed by each raster
 *         line's border waveforms (border_table); an RGB PPU writes the
 *         decode window's RGB instead (below)
 *
 * Each thread handles one NES pixel: reads palette+emphasis, looks up
 * the signal table, and writes `samples_per_pixel` float samples to
 * the output waveform at the correct phase offset.
 *
 * Dispatch: 256 threads per workgroup, 240 workgroups (one per scanline).
 * Total: 256 × 240 = 61,440 threads. An RGB PPU (source_mode 2) writes the
 * decode window instead (decode_window.h), one thread per window dot:
 * ceil(window_dots / 256) × window_lines workgroups.
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
    uint source_mode;          /* 0=composite, 1=Y/C, 2=ideal component/RGB modification */
    vec4 rgb_row_r, rgb_row_g, rgb_row_b; /* matrix rows and bias for RGB input */
    /* RGB PPU: the backdrop's 9-bit entry (the border's now come after the
     * picture's codes), the decode window and the region (0 NTSC, 1 PAL). */
    uint backdrop_entry, window_dots, window_lines, window_width;
    int start_dot, picture_dot, picture_row;
    uint region;
};

uint code_at(uint flat_idx) {
    uint packed = index_data[flat_idx / 2u];
    return ((flat_idx & 1u) == 0u ? (packed & 0xFFFFu) : (packed >> 16u)) & 0x1FFu;
}
uint pixel_entry(uint sy, uint px) { return code_at(sy * 256u + px); }
/* After the picture's codes, each raster line's border: side 0 left of the
 * picture (dots 49 to 64), side 1 right of it (and across lines 240, 241). */
uint border_entry(uint line, uint side) { return code_at(256u * 240u + line * 2u + side); }

// An RGB PPU: the 2C03's palette, three bits per gun (NESdev, PPU palettes),
// as gun voltages in sevenths. An emphasis bit drives its gun to full. The
// monitor's white drive, gun gains, contrast and brightness apply as the
// decoder's luma column and bias would; there is no chroma to take hue or
// saturation. Around the picture the PPU puts out the backdrop wherever the
// 2C02 does (NTSC lines 0 to 241, dots 50 to 331; the 2C02's hue-0 pulse at
// dot 49 is a composite colour generator's, not a palette entry); the 2C07
// blanks its border, and the rest of the line is blanking.
void rgb_ppu() {
    const uint rgb2c03[64]=uint[64](
        0x333u,0x014u,0x006u,0x326u,0x403u,0x503u,0x510u,0x420u,0x320u,0x120u,0x031u,0x040u,0x022u,0x000u,0x000u,0x000u,
        0x555u,0x036u,0x027u,0x407u,0x507u,0x704u,0x700u,0x630u,0x430u,0x140u,0x040u,0x053u,0x044u,0x000u,0x000u,0x000u,
        0x777u,0x357u,0x447u,0x637u,0x707u,0x737u,0x740u,0x750u,0x660u,0x360u,0x070u,0x276u,0x077u,0x000u,0x000u,0x000u,
        0x777u,0x567u,0x657u,0x757u,0x747u,0x755u,0x764u,0x772u,0x773u,0x572u,0x473u,0x276u,0x467u,0x000u,0x000u,0x000u);
    uint dot = gl_GlobalInvocationID.x, row = gl_GlobalInvocationID.y;
    if (dot >= window_dots || row >= window_lines) return;
    int line = int(row) - picture_row, raster_dot = start_dot + int(dot), px = raster_dot - picture_dot;
    bool lit = true;
    uint entry = 0u;
    if (line >= 0 && line < 240 && px >= 0 && px < 256) entry = pixel_entry(uint(line), uint(px));
    else if (region == 0u && line >= 0 && line < 242 && raster_dot >= 50 && raster_dot < 332)
        entry = border_entry(uint(line), raster_dot < picture_dot ? 0u : 1u);
    else lit = false;
    vec3 gun = vec3(0.0);
    if (lit) {
        uint code=rgb2c03[entry&63u], emphasis=(entry>>6u)&7u;
        gun=vec3(float((code>>8u)&15u),float((code>>4u)&15u),float(code&15u))/7.0;
        if((emphasis&1u)!=0u) gun.r=1.0;
        if((emphasis&2u)!=0u) gun.g=1.0;
        if((emphasis&4u)!=0u) gun.b=1.0;
    }
    vec3 rgb=vec3(rgb_row_r.x*gun.r+rgb_row_r.w,rgb_row_g.x*gun.g+rgb_row_g.w,rgb_row_b.x*gun.b+rgb_row_b.w);
    uint base=(row*window_width+dot*samples_per_pixel)*3u;
    for(uint s=0u;s<samples_per_pixel;s++) {
        waveform[base+s*3u]=rgb.r;
        waveform[base+s*3u+1u]=rgb.g;
        waveform[base+s*3u+2u]=rgb.b;
    }
}

/* Each raster line's border waveforms for the raster stage, after the
 * picture's samples: 36 floats per line, the left border's 12 carrier
 * phases, its hue-0 pulse at dot 49 (the entry in greyscale) and the right
 * border's, from the entries the frontend gave (border_entry). */
void border_table(uint line, uint k) {
    uint side = k < 24u ? 0u : 1u;
    uint entry = border_entry(line, side);
    if (k >= 12u && k < 24u) entry &= 0x1F0u;
    waveform[240u * samples_per_line + line * 36u + k] = signal_table[entry * 24u + k % 12u];
}

void main() {
    if (source_mode == 2u) { rgb_ppu(); return; }

    /* Each thread handles one NES pixel.
     * gl_WorkGroupID.y = scanline (0..239)
     * gl_LocalInvocationID.x = pixel within scanline (0..255) */
    uint px = gl_LocalInvocationID.x;
    uint sy = gl_WorkGroupID.y;

    if (px >= 256 || sy >= 240) return;
    if (px < 36u) border_table(sy, px);
    else if (px < 72u && sy < 2u) border_table(240u + sy, px - 36u);

    /* 9-bit entry: bits 0..5 = palette, bits 6..8 = emphasis. */
    uint entry = pixel_entry(sy, px);

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
    if (source_mode == 1u) {
        for (uint p=0u; p<12u; p++)
            dc += (alt ? signal_table_alt[entry*24u+p] : signal_table[entry*24u+p]) / 12.0;
    }
    for (uint s = 0; s < samples_per_pixel; s++) {
        if (source_mode == 1u) source_y[wave_base+s] = dc;
        waveform[wave_base + s] = alt
            ? signal_table_alt[table_base + s]
            : signal_table[table_base + s];
    }
}
