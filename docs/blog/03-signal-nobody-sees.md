# The Signal Nobody Sees

*What actually comes out of the NES composite video pin*

---

There is a widespread misconception about the NES: that it outputs RGB. It does not. The 2C02 PPU outputs a composite waveform -- a single analog signal on a single wire that encodes both brightness and color simultaneously through phase modulation. No red channel. No green channel. No blue channel. Just one signal that varies between approximately 0.35V and 1.55V, changing shape 3.58 million times per second.

Most emulators decode the 2C02's 64-entry palette to RGB via a lookup table and never touch the actual signal. This is convenient and fast, but it means composite artifacts -- dot crawl, chroma bleed, rainbow shimmer on sharp edges -- can only be faked as post-effects. You cannot simulate what a real TV does to the NES signal because you never had the signal.

To reproduce what the NES actually looked like on a CRT, you need to generate the waveform first.

## What Composite Video Actually Is

NTSC composite video is a luminance (Y) base signal plus a chrominance (C) signal modulated onto a 3.579545 MHz subcarrier. The color information is encoded as the phase and amplitude of this subcarrier relative to a reference burst. Phase determines hue. Amplitude determines saturation. This is quadrature amplitude modulation (QAM) -- the same technique used in WiFi, cellular radio, and digital TV. The 1953 NTSC committee did not invent a weird analog hack; they used a standard modulation scheme that happened to be compatible with existing black-and-white sets.

The composite signal is the sum of two components at any given moment:

- **Luma (Y):** The brightness, varying slowly (DC to about 4.2 MHz). A black-and-white TV just displays this.
- **Chroma (C):** A burst of 3.579545 MHz oscillation whose instantaneous phase and amplitude encode hue and saturation. The TV's demodulator multiplies this by reference cosine and sine to extract the I (in-phase, orange-cyan axis) and Q (quadrature, green-magenta axis) color difference signals.

A TV has to separate Y and C from the combined signal. This is the comb filter's job, and it is the primary source of composite video's characteristic look. The separation is never perfect, so some luma leaks into chroma (rainbow shimmer on sharp horizontal edges) and some chroma leaks into luma (dot patterns on saturated color fields).

## The Bisqwit Voltage Model

The 2C02 does not internally produce RGB and then encode it. It generates the composite waveform directly from its palette decoder, using voltage levels selected by the 6-bit palette index and 3-bit emphasis bits.

The PPU's output circuit selects between two voltage levels per subcarrier phase slot based on a comparison between the palette color value (0-13) and the current phase position (0-11). The result is a shaped waveform: grey entries produce a flat line (no chroma), saturated colors produce a square-ish wave whose phase offset relative to the colorburst encodes the hue.

The signal table is precomputed from Bisqwit's voltage model. Here is the actual precomputation from `signal_precompute.h`:

```c
static inline void signal_precompute_ntsc(SignalPrecompute *sp) {
    static const float levels[8] = {
        0.350f, 0.518f, 0.962f, 1.550f,   /* signal low,  luma 0..3 */
        1.094f, 1.506f, 1.962f, 1.962f,   /* signal high, luma 0..3 */
    };
    const int emph_oct = 0264513;
    const float blacklo = levels[1];
    const float whitehi = levels[7];
    const float norm = 1.0f / (whitehi - blacklo);

    for (int pal_idx = 0; pal_idx < 64; pal_idx++) {
        int color = pal_idx & 0x0F;
        int level = (pal_idx >> 4) & 0x03;
        if (color > 13) level = 1;

        for (int emph = 0; emph < 8; emph++) {
            int entry = (emph << 6) | pal_idx;
            for (int p = 0; p < 12; p++) {
                int in_hi = (color < 13) && (((color + p) % 12) < 6);
                if (color == 0) in_hi = 1;
                float sig = levels[level + (in_hi ? 4 : 0)];
                int octant = (p % 12) >> 1;
                int mask = (emph_oct >> (3 * octant)) & 0x07;
                if (emph & mask) sig *= 0.746f;
                float norm_sig = (sig - blacklo) * norm;
                sp->table[entry][p] = norm_sig;
                sp->table[entry][p + 12] = norm_sig;
            }
        }
    }
}
```

Eight voltage levels. A comparison that determines whether each phase slot is "high" or "low." Emphasis bits that attenuate specific phase octants by a factor of 0.746. That is the complete 2C02 video output model. Everything else -- every color the NES displays, every dot crawl pattern, every rainbow shimmer -- is a consequence of these numbers and the downstream signal processing.

The table has 512 entries (64 palette values times 8 emphasis combinations), each producing 12 phase slots. The slots are duplicated to 24 so that an 8-sample read starting at any offset stays in bounds without a modulo.

Consider a few example waveforms:

- **Palette $0F (black):** All 12 slots at the minimum level. Flat line, no chroma. The TV sees pure low-luminance signal.
- **Palette $30 (white):** All 12 slots at the maximum level. Flat line again, but high. Pure high luminance.
- **Palette $16 (red):** Six slots high, six slots low, phased to align with the red axis of the subcarrier. The TV's demodulator sees strong I-channel energy at the red hue angle.
- **Palette $12 (blue):** Same square-wave pattern, but phase-shifted 180 degrees from red. Strong negative-I, positive-Q.
- **Palette $16 with emphasis bits $40:** The red waveform, but slots in the attenuated octant are multiplied by 0.746. Lower amplitude at certain phases shifts the decoded color slightly and reduces saturation.

## The Sampling Relationship

The NES emits 8 waveform samples per pixel at a sample rate derived from the master oscillator. The subcarrier completes one full cycle every 12 phase slots. So each NES pixel spans 8/12 = 2/3 of a subcarrier cycle.

This is not an arbitrary choice. The NTSC standard defines the relationship between the pixel clock and the colorburst frequency, and the NES's master oscillator produces both from the same crystal. The ratio 2/3 is the real NTSC relationship.

PAL is different: 10 samples per pixel at 12 phase slots per cycle gives 10/12 = 5/6 cycles per pixel. Both regions use the same 12-slot color wheel -- they just sample it at different rates.

On the GPU, the DAC shader converts the 256x240 palette index buffer into a 2048x240 (NTSC) or 2560x240 (PAL) float waveform:

```glsl
void main() {
    uint px = gl_LocalInvocationID.x;     // pixel 0..255
    uint sy = gl_WorkGroupID.y;           // scanline 0..239

    // Read palette + emphasis from packed uint16 buffer
    uint flat_idx = sy * 256 + px;
    uint packed = index_data[flat_idx / 2];
    uint pixel_val = (flat_idx & 1u) == 0u
                     ? (packed & 0xFFFFu)
                     : (packed >> 16u);
    uint entry = pixel_val & 0x1FFu;

    // Compute per-pixel subcarrier phase
    uint line_phase = (phase_base + sy * phase_line_adv) % 12u;
    uint pixel_phase = (line_phase + px * samples_per_pixel) % 12u;

    // Look up signal table and emit samples
    uint table_base = entry * 24u + pixel_phase;
    uint wave_base = sy * samples_per_line + px * samples_per_pixel;

    for (uint s = 0; s < samples_per_pixel; s++) {
        waveform[wave_base + s] = signal_table[table_base + s];
    }
}
```

256 threads per workgroup (one per NES pixel), 240 workgroups (one per scanline). 61,440 threads total. Each thread reads one palette index, computes the subcarrier phase for that pixel position, and writes 8 (or 10) float samples. The entire 2048x240 waveform is generated in a single dispatch.

## Dot Crawl: Not a Bug

The subcarrier phase advances between frames. The `phase_base` uniform tracks this, advancing by `phase_field_adv` slots per frame. Because the phase relationship between the pixel grid and the subcarrier changes frame-to-frame, the visible chroma artifacts shift position. On sharp color transitions -- where palette index $16 (red) sits next to $30 (white) -- the imperfect Y/C separation in the TV's comb filter creates visible dots at the chroma frequency. These dots crawl across the screen as the phase cycles.

This is not a bug in the NES or in the TV. It is inherent to NTSC. The subcarrier frequency was chosen to be an odd multiple of half the line rate specifically so that the phase would alternate between frames, making the chroma artifacts less visible through temporal averaging. Dot crawl is the visible evidence of this deliberate design choice.

On a real TV, if you stare at a static NES screen, you see the dots slowly shift over a 2-3 frame cycle. Some TVs with 3D comb filters or frame buffers cancel it entirely. Cheap TVs with no comb filter show aggressive crawling. The GPU pipeline reproduces this naturally because the phase offset is tracked per frame and fed into the DAC shader.

PAL has a different dot crawl pattern because the V-phase inverts per scanline (the "Phase Alternating Line" that gives PAL its name). The CPU composite path handles this by maintaining two signal tables -- one for even scanlines and one for odd -- with the V-component flipped:

```c
float signal_table[COMP_SIGNAL_ENTRIES][COMP_TABLE_STRIDE];
float signal_table_alt[COMP_SIGNAL_ENTRIES][COMP_TABLE_STRIDE];
```

The emission loop picks the appropriate table based on scanline parity, modeling the 2C07's per-line V-phase inversion at the encoder.

## Why This Matters

To faithfully reproduce what the NES looked like on a real TV, you cannot start from RGB and work backwards. There is no "dot crawl filter" you can apply to an RGB image that produces the right pattern, because the pattern depends on the subcarrier phase at each specific pixel position, which depends on where the pixel sits within the 12-slot color wheel, which depends on the scanline and the frame counter.

The artifacts are not decorative. They are information. Experienced NES players learned to read them -- the slight color fringing that tells you a sprite is one pixel away from a background tile, the dot pattern that indicates a specific palette combination. Game artists used them deliberately, placing specific palette indices next to each other knowing that the composite signal would blend them into colors that do not exist in the NES palette.

The composite waveform is the ground truth. Everything downstream -- the comb filter, the demodulator, the CRT beam -- processes this signal. Generate it wrong and every subsequent stage produces wrong artifacts. Generate it right and the artifacts emerge naturally from the math, without any special-case code. That is what the GPU pipeline does: 14 stages of signal processing, starting from this waveform, ending at the phosphor screen.
