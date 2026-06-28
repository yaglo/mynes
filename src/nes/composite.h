/*
 * NTSC Composite Simulation
 *
 * Shared pipeline for all frontends. Converts a 256x240 RGB888 framebuffer
 * into a scaled RGB888 buffer with NTSC composite artifacts:
 *   - YIQ color space conversion
 *   - Bandwidth-limiting lowpass filters
 *   - Asymmetric chroma bleed (colors smear right)
 *   - Dot crawl (animated chroma-to-luma leakage)
 *   - Rainbow shimmer on high-contrast edges
 *   - Scanline darkening (1.0, 0.75, 0.0, 0.75 pattern)
 *
 * Output resolution = input (256x240) × scale factor.
 * The scale factor is set at init time based on the window size.
 */

#ifndef NES_COMPOSITE_H
#define NES_COMPOSITE_H

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

/* Explicit SIMD intrinsics for comp_fir_symmetric. The auto-vectorizer
 * only produces scattered `fmul.4s` for the scalar 4-way-ILP loop
 * (checked in the built object file), so we pull in the ISA-specific
 * intrinsic headers here and select at compile time inside the hot
 * function. Non-NEON/non-AVX2 platforms fall back to the scalar
 * implementation. */
#if defined(__ARM_NEON)
#  include <arm_neon.h>
#elif defined(__AVX2__) && defined(__FMA__)
#  include <immintrin.h>
#endif

/* Vendored LMP88959/PAL-CRT — authentic 2C07 encoder + decoder used on
 * the PAL branch of comp_process. Distributed under a permissive custom
 * license (attribution requested). See src/nes/palcrt/README.md. */
#include "palcrt/pal_core.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* Max output width (for stack-allocated scratch buffers) */
#define COMP_MAX_W 2560

/* Region enum — selects NTSC (2C02) vs PAL (2C07) signal model. */
#define COMP_REGION_NTSC 0
#define COMP_REGION_PAL  1

/* Waveform pipeline constants. Bisqwit's canonical NES NTSC model uses
 * 12 phase slots per subcarrier cycle and emits 8 samples per NES pixel
 * (8/12 = 2/3 cycles per pixel = real NTSC). We store 24 slots per
 * palette entry (12 phases, duplicated) so an N-sample read at any
 * phase offset 0..11 stays within bounds without a modulo.
 *
 * PAL shares the same 12-slot color wheel: at 10 samples/NES-pixel,
 * 10/12 = 5/6 cycles/pixel ≡ real PAL subcarrier ratio. So both regions
 * run the same 12-phase demod table and the same signal-table stride;
 * they just emit a different number of samples per NES pixel:
 *   NTSC: 8 samples/pixel → 2048 samples/scanline → 2/3 cyc/pixel
 *   PAL:  10 samples/pixel → 2560 samples/scanline → 5/6 cyc/pixel
 * COMP_NTSC_SAMPLES_PER_PIXEL/COMP_NTSC_SIGNAL_W remain as the NTSC defaults; the
 * runtime value is held in CompositeSignal.samples_per_pixel / .signal_w and
 * picked based on the region. Scratch buffers are sized to the PAL max
 * (COMP_SIGNAL_MAX) so either region fits. */
#define COMP_NTSC_SAMPLES_PER_PIXEL 8
#define COMP_TABLE_PHASES      12
#define COMP_TABLE_STRIDE      (COMP_TABLE_PHASES * 2)  /* 24 */
#define COMP_SIGNAL_ENTRIES    512   /* 64 colors × 8 emphasis states */
#define COMP_NTSC_SIGNAL_W          (256 * COMP_NTSC_SAMPLES_PER_PIXEL)  /* 2048 */
#define COMP_PAL_SAMPLES_PER_PIXEL 10
#define COMP_SIGNAL_MAX       (256 * COMP_PAL_SAMPLES_PER_PIXEL)  /* 2560 */

/* ============================================================================
 * Waveform signal state
 * ============================================================================ */

typedef struct {
    /* Per-(color, emphasis) waveform: 12-phase composite signal slots
     * normalized so that black = 0.0 and white = 1.0. Indexed via
     *   signal_table[(emph << 6) | color_idx][phase0..phase0+7]
     * where phase0 is the current chroma wheel offset (mod 12).
     * Slots [0..11] hold the 12 phases; slots [12..23] duplicate them
     * so an 8-sample read at any phase 0..11 stays in bounds.
     *
     * NTSC: all scanlines use signal_table, signal_table_alt unused.
     * PAL:  signal_table is the "even" (alter=false) table and
     *       signal_table_alt is the V-flipped "odd" (alter=true) table.
     *       The emission loop picks based on scanline parity. This
     *       models 2C07's per-line V-phase inversion at the encoder,
     *       which our downstream V-averaging delay line relies on. */
    float signal_table[COMP_SIGNAL_ENTRIES][COMP_TABLE_STRIDE];
    float signal_table_alt[COMP_SIGNAL_ENTRIES][COMP_TABLE_STRIDE];

    /* 12-slot reference carrier for synchronous Y/I/Q demodulation.
     * One full subcarrier cycle spans 12 phase units
     * (= COMP_NTSC_SAMPLES_PER_PIXEL × 12/COMP_NTSC_SAMPLES_PER_PIXEL). */
    float demod_cos[COMP_TABLE_PHASES];
    float demod_sin[COMP_TABLE_PHASES];

    /* Hamming-windowed sinc FIR taps, symmetric. fir_y rejects the
     * subcarrier in Y decode; fir_c bandwidth-limits I/Q demod.
     * Cutoffs are runtime-tunable — lowering y_cutoff reduces dot-crawl
     * residue at the cost of softening sharp luma edges.
     * Buffer sized for the maximum tap count allowed by
     * comp_redesign_firs (63 for Y, 63 for C). */
    float fir_y[64];
    float fir_c[64];
    int   fir_y_n;
    int   fir_c_n;
    float y_cutoff;   /* normalized cycles/sample, 0.03..0.10 sensible */
    float c_cutoff;   /* normalized cycles/sample, 0.01..0.06 sensible */

    /* Runtime-tunable chroma phase parameters. These control dot crawl
     * and color accuracy — both need to match the real 2C02 output for
     * correct colors. Defaults are first-order approximations; use the
     * SDL frontend hotkeys to dial them in against reference ROMs.
     *
     *   phase_base        — starting slot of field 0 (0..11)
     *   phase_line_adv    — slots advanced per scanline (signed)
     *   phase_field_adv   — slots advanced per frame (signed)
     *   phase_num_fields  — number of distinct fields in the dot-crawl cycle
     *   demod_rotate      — extra rotation applied to the demod reference
     *                       carrier (slots in 1/12ths). Shifts all hues
     *                       uniformly — useful for matching the "colorburst
     *                       reference" convention of an external palette.
     *   chroma_gain       — scalar multiplied into the I/Q demod output.
     *                       Default 2.0 (exact amplitude recovery); lower
     *                       values tame saturation. Bisqwit's reference
     *                       uses 1.7 for slightly softer colors. */
    int   phase_base;
    int   phase_line_adv;
    int   phase_field_adv;
    int   phase_num_fields;
    int   demod_rotate;
    float chroma_gain;

    /* Color killer — a classic NTSC TV feature that mutes the chroma
     * signal when its amplitude drops below a threshold. On whites
     * and greys (which should have zero chroma but pick up cross-
     * color from the imperfect Y/C separation) this forces them back
     * to achromatic. The threshold is in the same units as filtI/Q
     * after the chroma FIR — 0.05 is a gentle killer, 0.15 is very
     * aggressive, 0 disables it entirely. */
    float color_killer;

    /* 2D comb filter (vertical 1-line delay) — exploits the fact
     * that the chroma subcarrier phase inverts between scanlines
     * (when phase_line_adv = 6, i.e. 180°). Adjacent lines have the
     * same luma but opposite chroma phases, so:
     *   Y = (curr + prev) / 2   → chroma cancels, pure luma
     *   C = (curr - prev) / 2   → luma cancels, pure chroma
     * This gives much cleaner Y/C separation than any filter alone —
     * it's how high-end 80s/90s TVs killed cross-color. */
    int   comb_filter;

    /* Region-specific signal generation parameters. Picked at comp_init
     * based on the region argument (or runtime region toggle from the
     * OSD). See COMP_REGION_* for enum values.
     *
     *   region            — NTSC (0) or PAL (1)
     *   samples_per_pixel — samples emitted per NES pixel (8=NTSC, 10=PAL)
     *   signal_w          — 256 × samples_per_pixel, cached for hot loops */
    int   region;
    int   samples_per_pixel;
    int   signal_w;

    /* S-Video dispatch flag. When set, comp_process routes to the
     * comp_process_svideo path (flat palette LUT → scanline + CRT effects
     * with NO composite artifacts: no dot crawl, no subcarrier, no
     * chroma bleed). The RGB framebuffer argument to comp_process is
     * used as the source since it's already decoded via the palette LUT.
     * PAL + svideo falls back through PAL-CRT (which always runs its
     * own composite chain), so svideo only meaningfully applies to NTSC. */
    int   svideo;
} CompositeSignal;

/* ============================================================================
 * State
 * ============================================================================ */

typedef struct {
    float luma_lowpass;      /* Y box-filter width scales with (1 - value) */
    float chroma_lowpass;    /* I/Q box-filter width scales with (1 - value) */
    float luma_afterglow;    /* Rightward phosphor decay trail (0.0 = off) */
    float luma_ringing;      /* Video amplifier overshoot on sharp edges */
    float chroma_ringing;    /* Chroma amplifier overshoot on I/Q edges (0.0 = off) */
    float bloom;             /* Phosphor bloom: bright areas glow outward (0.0-1.0) */
    float bloom_2d;          /* Full-frame 2D RGB halo bloom (0.0 = off) */
    float aperture_grille;   /* RGB stripe phosphor mask (0.0 = off, 1.0 = max) */
    float vignette;          /* Corner/edge darkening (0.0 = off, 1.0 = strong) */
    float chromatic_conv;    /* Chromatic convergence error: R/B horizontal shift
                              * (0.0 = perfect, 1.0 = ~2 output px offset) */
    float gamma;             /* Display gamma. 1.0 = identity; 1.1-1.2 gives
                              * the characteristic CRT midtone darkening */
    float persistence;       /* Vertical phosphor persistence (prev-frame
                              * max-blend decay, 0.0 = off, 1.0 = full hold) */
    float barrel;            /* Barrel distortion (CRT glass curvature),
                              * 0.0 = flat, 1.0 = strong pincushion */

    /* Signal-path analog effects (NES → RF/composite → TV chain). */
    float hsync_wobble;      /* H-sync instability: per-scanline sub-pixel
                              * horizontal jitter from composite sync noise
                              * (0.0 = stable, 1.0 = ~3 output px wobble) */
    float ghosting;          /* Multipath reflection: attenuated horizontal
                              * ghost at ghost_offset px delay (0.0 = off) */
    int   ghost_offset;      /* Ghost horizontal delay in output pixels */
    float snow;              /* RF reception noise: per-pixel luma grain
                              * (0.0 = clean, 1.0 = heavy static) */
    float hum_bar;           /* 60 Hz AC hum: slowly rolling brightness
                              * band across the screen (0.0 = off) */
    int   black_floor;       /* Ambient room-light on CRT glass: raise
                              * minimum black level by this many units
                              * (0 = true black, 8-12 = "lit room") */

    /* User color controls — applied at the YIQ → RGB stage. Defaults are
     * neutral (no-op) so the out-of-box look is unchanged. */
    float hue_deg;           /* Hue rotation in degrees, [-30, +30] typical */
    float saturation;        /* Chroma scale, [0.0, 2.0], default 1.0 */
    float brightness;        /* Y offset, [-0.5, +0.5], default 0.0 */
    float contrast;          /* Y scale around 0.5, [0.5, 1.5], default 1.0 */

    /* GPU offload: skip display-domain post-passes that the GPU handles.
     * Set by the GPU frontend to avoid double-processing. When 0,
     * comp_post_process runs all passes (SDL2 frontend behavior). */
    int skip_display_effects;
#define COMP_SKIP_BLOOM2D      (1 << 0)   /* comp_apply_bloom2d */
#define COMP_SKIP_BARREL       (1 << 1)   /* comp_apply_barrel */
#define COMP_SKIP_DISPLAY_FX   (1 << 2)   /* comp_apply_display_effects (mask+conv+gamma+floor) */

    int overscan_x;          /* NES pixels cropped from each horizontal edge */
    int overscan_y;          /* NES pixels cropped from each vertical edge */

    int out_w;              /* Output width */
    int rows_per_scanline;  /* Output rows per NES scanline (1-4, determines out_h) */
    int frame_count;

    uint8_t *output;        /* Output framebuffer (out_w * 240 * 3, heap-allocated) */
    uint8_t *output_prev;   /* Previous frame, for vertical persistence blend */
    uint8_t *output_tmp;    /* Scratch buffer for post-processing (bloom, barrel) */
    uint8_t  gamma_lut[256];/* 256-entry LUT for display gamma curve */

    /* 3×3 decode matrix + bias for the output stage. The row-major form
     * is RGB_row = m[row] · (Y, C1, C2) + bias[row], then clamp to 255.
     *
     * For NTSC (region = COMP_REGION_NTSC) the C1/C2 axes are YIQ-style
     * (cos/sin of the NTSC 33°-rotated reference carrier); for PAL they
     * are YUV-style (standard 0°/90° U/V). In both cases the matrix
     * coefficients pre-fold:
     *   - saturation boost (1.15×)         — warmth sell
     *   - warm tint (CRT phosphor ~5500K)  — R×1.03, G×1.01, B×0.97
     *   - black level                      — +0.015/0.015/0.018
     *   - final ×255 scale for direct uint8 output
     *
     * Populated by comp_apply_color_matrix() at init and whenever the
     * region changes via comp_set_region(). Defaults to NTSC. */
    float color_matrix[3][3];   /* m[row=R,G,B][col=Y,C1,C2] */
    float color_bias[3];        /* pre-multiplied [R, G, B] bias */

    /* Waveform pipeline state (signal table + FIR taps + demod carrier).
     * Populated by comp_init; used by comp_process when called with a
     * non-NULL index framebuffer. */
    CompositeSignal signal;

    /* ---- PAL-CRT state (PAL region only) ----
     * When region == COMP_REGION_PAL, comp_process dispatches to the
     * vendored LMP88959/PAL-CRT pipeline instead of our own 2C02-style
     * waveform path. PAL-CRT is an end-to-end palette→output pipeline:
     * it takes the NES palette index framebuffer, runs a real 2C07
     * encoder (per-line V-phase inversion, HardWareMan's oscilloscope-
     * measured voltage levels) through a real PAL decoder (burst-
     * extracted phase reference, 1H delay-line U/V averaging), and
     * writes RGB DIRECTLY into n->output — no intermediate buffer,
     * no post-processing stage. That's the right model: the signal
     * is the palette, and the output is what the TV shows. Our own
     * bloom/afterglow/ringing/vignette/beam-profile effects are the
     * NTSC path's CRT character layer, not a PAL concern.
     *
     * palcrt          — PAL-CRT state (analog signal buffer, sync locks,
     *                   burst accumulator). Re-pointed at palcrt_rgb
     *                   via pal_resize on comp_resize; internal state
     *                   persists across frames.
     * palcrt_settings — per-frame PAL-CRT config (source data pointer,
     *                   source size, hue/xoffset/yoffset, ua6538 flag).
     *                   Persistent internal state (field_initialized,
     *                   altline[]) is preserved frame-to-frame.
     * palcrt_rgb      — stage buffer (out_w × 240 × 3). PAL-CRT writes
     *                   one RGB row per NES scanline here; the PAL
     *                   branch of comp_process then iterates those 240
     *                   rows through comp_emit_output_row_rgb, which
     *                   replicates each row `rows_per_scanline` times
     *                   with the CRT beam profile. Matches NTSC's
     *                   output character (scanline darkening, vignette,
     *                   HDR highlight knee, aperture grille). This is
     *                   NOT an intermediate palette-decode stage — the
     *                   palette→signal→RGB transform is entirely inside
     *                   PAL-CRT. This buffer is the "decoded phosphor
     *                   row before the CRT beam traces it out", which
     *                   is a different pipeline stage. */
    struct PAL_CRT       palcrt;
    struct PAL_SETTINGS  palcrt_settings;
    uint8_t             *palcrt_rgb;
} Composite;

/* ============================================================================
 * Waveform precompute helpers
 * ============================================================================ */

/* Precompute the (color, emphasis) → waveform table using Bisqwit's
 * canonical NES NTSC model. Reference:
 *   tests/nes-test-roms/ppu_read_buffer/source/gfx/nes_palette.php
 *
 * Each entry gets all 12 phase slots so runtime phase rotation is a
 * free index wrap. Normalizes the Bisqwit voltage range [0.518, 1.962]
 * to [0.0, 1.0] so the downstream YIQ conversion matches the legacy
 * (RGB/255) scaling and we can reuse comp_decode_to_rgb unchanged.
 */
static void comp_precompute_signal_table_ntsc(CompositeSignal *s) {
    /* Voltage levels relative to sync level, from Bisqwit's reference. */
    static const float levels[8] = {
        0.350f, 0.518f, 0.962f, 1.550f,   /* signal low,  luma 0..3 */
        1.094f, 1.506f, 1.962f, 1.962f,   /* signal high, luma 0..3 */
    };
    /* Octal bitmask selecting which phase octants each emphasis bit
     * attenuates. (R=bit0, G=bit1, B=bit2 of each 3-bit group). */
    const int emph_oct = 0264513;
    const float blacklo = levels[1];
    const float whitehi = levels[7];
    const float norm = 1.0f / (whitehi - blacklo);

    for (int pal_idx = 0; pal_idx < 64; pal_idx++) {
        int color = pal_idx & 0x0F;
        int level = (pal_idx >> 4) & 0x03;
        if (color > 13) level = 1;   /* colors $xE/$xF: level forced */

        for (int emph = 0; emph < 8; emph++) {
            int entry = (emph << 6) | pal_idx;
            for (int p = 0; p < COMP_TABLE_PHASES; p++) {
                /* Square wave: high when (color+phase)%12 is in the
                 * first half of the cycle. color==0 is always "high"
                 * (flat white luma); color>=13 is always "low" (black). */
                int in_hi = (color < 13) && (((color + p) % 12) < 6);
                /* color==0 is a special "always bright" case in Bisqwit:
                 * it selects the high luma level unconditionally. */
                if (color == 0) in_hi = 1;
                float sig = levels[level + (in_hi ? 4 : 0)];

                /* Emphasis attenuation: phase octants (p%12)>>1 are
                 * indexed into the octal bitfield; a set bit means
                 * this octant is attenuated when the corresponding
                 * emphasis bit is asserted. */
                int octant = (p % 12) >> 1;
                int mask = (emph_oct >> (3 * octant)) & 0x07;
                if (emph & mask) sig *= 0.746f;

                float norm_sig = (sig - blacklo) * norm;
                s->signal_table[entry][p] = norm_sig;
                s->signal_table[entry][p + COMP_TABLE_PHASES] = norm_sig;
            }
        }
    }
}

/* Precompute the PAL 2C07 (color, emphasis) → waveform tables from
 * the physical 2C07 signal model.
 *
 * Uses HardWareMan's oscilloscope-measured RP2C07 voltage levels and
 * the `alter` color-remapping V-flip algorithm, imported from
 * LMP88959/PAL-CRT as the canonical 2C07 signal model. PAL-CRT
 * is distributed under a permissive custom license (attribution
 * requested, no warranty).
 *
 * Generates TWO tables:
 *   - signal_table       — "even" scanlines (alter=false)
 *   - signal_table_alt   — "odd" scanlines (alter=true, V phase flipped
 *                          by remapping hue to 0x0d - ((ohue + 2) & 0x0f))
 * The emission loop picks based on scanline parity to model PAL's
 * per-line V-phase inversion at the signal level — the defining feature
 * of PAL. Downstream, the decoder flips V on odd lines (v_sign) and
 * averages V between adjacent lines via a 1H delay, giving PAL its
 * soft vertical chroma and phase-error cancellation.
 *
 * Key 2C07 differences from 2C02 modeled here:
 *   1. Measured voltage levels (not borrowed from 2C02 — actual mV)
 *   2. Color $0D = flat black (not sub-black)
 *   3. Colors $0E/$0F = signal 0 (actual 0, not level-1 low)
 *   4. R↔G emphasis bit swap: PPUMASK bit 5 is GREEN on 2C07 and RED
 *      on 2C02. Handled via a bit swap before indexing active[].
 *   5. Per-octant emphasis mask uses the active[6] table from PAL-CRT.
 *
 * The decoded colors will NOT match the hardcoded ppu_palette_2c07
 * RGB LUT byte-for-byte — that LUT is an artistic/empirical palette
 * whose values don't correspond to a clean YUV decoder round-trip.
 * Instead, the composite pipeline produces colors from honest physics:
 * what a real PAL TV would decode from a real 2C07's square waves.
 * That's a better notion of authenticity than "match the FBX LUT."
 */
static void comp_precompute_signal_table_pal(CompositeSignal *s) {
    /* HardWareMan's RP2C07 voltage measurements (from PAL-CRT/pal_nes.c),
     * in "mV × 1024 × 110 / (white - black)" fixed-point units. Indexed
     * as [(high << 3) | (emphasized << 2) | luma]:
     *
     *        LOW / LOW_EMPH          HIGH / HIGH_EMPH
     *  L0   -175 / -266              609 /  334
     *  L1      0 / -133             1000 /  642
     *  L2    475 /  225             1467 / 1000
     *  L3   1067 /  700             1467 / 1000
     *
     * White = 1467 mV, black = 0 mV. 1 IRE unit = 13.3 mV.
     */
    static const int ire_pal[16] = {
        /* low, no emph:   L0     L1     L2     L3 */
                         -13437,  0,     36472, 81927,
        /* low, emph:      L0     L1     L2     L3 */
                         -20424, -10212, 17276, 53748,
        /* high, no emph:  L0     L1     L2     L3 */
                          46761,  76783, 112640, 112640,
        /* high, emph:     L0     L1     L2     L3 */
                          25645,  49294, 76783,  76783,
    };
    /* Normalize so black = 0.0 and white = 1.0. Sub-black voltages
     * (L0 low at -13437) become small negatives; the downstream
     * pipeline handles them via clamping in comp_decode_to_rgb. */
    const float norm = 1.0f / 112640.0f;

    /* Per-octant emphasis mask from PAL-CRT's active[] table, translated
     * to our "emph bit 0 = R, 1 = G, 2 = B" convention. Identical to the
     * NTSC Bisqwit emph_oct = 0264513 when unpacked, which is reassuring
     * — the physics of which octants each emphasis bit darkens is a
     * property of the color wheel, not the region. */
    static const int active[6] = { 0x3, 0x1, 0x5, 0x4, 0x6, 0x2 };

    for (int alter = 0; alter < 2; alter++) {
        float (*dst)[COMP_TABLE_STRIDE] =
            alter ? s->signal_table_alt : s->signal_table;

        for (int pal_idx = 0; pal_idx < 64; pal_idx++) {
            int ohue = pal_idx & 0x0F;
            int luma = (pal_idx >> 4) & 0x03;

            /* Colors $xE and $xF: flat zero signal (PAL-CRT special case). */
            int is_forced_zero = (ohue == 0x0E || ohue == 0x0F);

            /* Apply alter's V-flip color remapping (from PAL-CRT square_sample).
             * This is the "encoder V-phase inversion" that happens every other
             * scanline on real 2C07. For colors 0..12 (hue wheel + flat bright)
             * and $0D (flat black), ohue is still used for special-case
             * dispatch; only the square-wave hue is remapped.
             *
             *   ohue=0x00: flat bright (l=1), NOT remapped (no V to flip)
             *   ohue=0x0D: flat black (l=0), NOT remapped
             *   ohue 1..12: hue = 0x0d - ((ohue + 2) & 0x0f) on alter */
            int hue_for_square = ohue;
            if (alter && ohue >= 1 && ohue <= 12) {
                hue_for_square = 0x0d - ((ohue + 2) & 0x0f);
            }

            for (int emph_ntsc = 0; emph_ntsc < 8; emph_ntsc++) {
                int entry = (emph_ntsc << 6) | pal_idx;

                /* PAL PPUMASK emphasis bits: bit 5 = GREEN, bit 6 = RED
                 * (swapped vs NTSC). Our `emph` field comes out of
                 * ppu_render_pixel as {bit0=mask[5], bit1=mask[6], bit2=mask[7]},
                 * so for PAL we swap bits 0 and 1 to get the normalized
                 * "R/G/B" ordering that active[] expects. */
                int emph = (emph_ntsc & 0x4) |
                           ((emph_ntsc & 0x2) >> 1) |
                           ((emph_ntsc & 0x1) << 1);

                for (int p = 0; p < COMP_TABLE_PHASES; p++) {
                    if (is_forced_zero) {
                        dst[entry][p] = 0.0f;
                        dst[entry][p + COMP_TABLE_PHASES] = 0.0f;
                        continue;
                    }

                    /* Square wave high/low based on (possibly V-flipped) hue. */
                    int v = (((hue_for_square + p) % 12 + 12) % 12) < 6;
                    int l;
                    if (ohue == 0x00)      l = 1;  /* flat bright */
                    else if (ohue == 0x0D) l = 0;  /* flat black  */
                    else                   l = v;

                    /* Emphasis octant lookup. The octant index is
                     * (p >> 1) % 6 — 6 octants per subcarrier cycle. */
                    int octant = (p >> 1) % 6;
                    int e = ((emph & active[octant]) != 0) ? 1 : 0;

                    int ire_idx = (l << 3) | (e << 2) | luma;
                    float sig = (float)ire_pal[ire_idx] * norm;

                    dst[entry][p] = sig;
                    dst[entry][p + COMP_TABLE_PHASES] = sig;
                }
            }
        }
    }
}

/* Design a Hamming-windowed sinc lowpass FIR, symmetric, normalized
 * to unit DC gain. `cutoff` is the normalized cutoff frequency in
 * cycles/sample (0..0.5). `n` is the tap count (must be odd ≥ 3). */
static void comp_design_fir(float *taps, int n, float cutoff) {
    int half = n / 2;
    float sum = 0.0f;
    for (int k = 0; k < n; k++) {
        int m = k - half;
        float sinc = (m == 0)
            ? 2.0f * cutoff
            : sinf(2.0f * (float)M_PI * cutoff * (float)m)
              / ((float)M_PI * (float)m);
        float w = 0.54f - 0.46f * cosf(2.0f * (float)M_PI * (float)k / (float)(n - 1));
        taps[k] = sinc * w;
        sum += taps[k];
    }
    float inv = 1.0f / sum;
    for (int k = 0; k < n; k++) taps[k] *= inv;
}

/* Populate demod carrier tables for the 12-slot chroma wheel. */
static void comp_precompute_demod(CompositeSignal *s) {
    for (int k = 0; k < COMP_TABLE_PHASES; k++) {
        float a = 2.0f * (float)M_PI * (float)k / (float)COMP_TABLE_PHASES;
        s->demod_cos[k] = cosf(a);
        s->demod_sin[k] = sinf(a);
    }
}

/* Rebuild Y/C FIR taps after the caller adjusts y_cutoff or c_cutoff
 * (or tap counts). Clamps cutoffs to sane ranges first. */
static void comp_redesign_firs(CompositeSignal *s) {
    if (s->y_cutoff < 0.005f) s->y_cutoff = 0.005f;
    if (s->y_cutoff > 0.200f) s->y_cutoff = 0.200f;
    if (s->c_cutoff < 0.005f) s->c_cutoff = 0.005f;
    if (s->c_cutoff > 0.200f) s->c_cutoff = 0.200f;
    if (s->fir_y_n < 3)  s->fir_y_n = 3;
    if (s->fir_y_n > 63) s->fir_y_n = 63;
    if (!(s->fir_y_n & 1)) s->fir_y_n++;
    if (s->fir_c_n < 3)  s->fir_c_n = 3;
    if (s->fir_c_n > 63) s->fir_c_n = 63;
    if (!(s->fir_c_n & 1)) s->fir_c_n++;
    comp_design_fir(s->fir_y, s->fir_y_n, s->y_cutoff);
    comp_design_fir(s->fir_c, s->fir_c_n, s->c_cutoff);
}

/* Populate the output color matrix + bias for the given region.
 *
 * NTSC (region = COMP_REGION_NTSC) uses the legacy empirically-tuned
 * YIQ→RGB coefficients — these are pre-folded with saturation boost
 * (~1.15×), warm-phosphor tint (R×1.03, G×1.01, B×0.97), CRT black
 * level (+0.015/0.015/0.018), and the final ×255 scale for direct
 * uint8 output. Matches the pre-matrix hardcoded values bit-exactly.
 *
 * PAL (region = COMP_REGION_PAL) uses the standard BT.470 YUV→RGB
 * matrix:
 *     R = Y                + 1.140 V
 *     G = Y - 0.395 U      - 0.581 V
 *     B = Y + 2.032 U
 * In our demod naming Ip = cos-demod = V (phase-flipped) and Qp =
 * sin-demod = U, so column 1 (C1) multiplies V and column 2 (C2)
 * multiplies U. Same warm-tint / black-level / sat-boost folding
 * as NTSC so both regions share the CRT character. */
static inline void comp_apply_color_matrix(Composite *n, int region) {
    const float scale = 255.0f;
    const float sat   = 1.15f;
    const float warm_r = 1.03f, warm_g = 1.01f, warm_b = 0.97f;
    const float bias_r = 0.015f, bias_g = 0.015f, bias_b = 0.018f;

    if (region == COMP_REGION_PAL) {
        /* YUV decoder. Column 1 = V (= Ip), Column 2 = U (= Qp).
         * Standard YUV→RGB:
         *   R = Y           + 1.140 V
         *   G = Y - 0.395 U - 0.581 V
         *   B = Y + 2.032 U */
        n->color_matrix[0][0] = warm_r * scale;
        n->color_matrix[0][1] = 1.140f * sat * scale;     /* V */
        n->color_matrix[0][2] = 0.0f;                     /* U */

        n->color_matrix[1][0] = warm_g * scale;
        n->color_matrix[1][1] = -0.581f * sat * scale;    /* V */
        n->color_matrix[1][2] = -0.395f * sat * scale;    /* U */

        n->color_matrix[2][0] = warm_b * scale;
        n->color_matrix[2][1] = 0.0f;                     /* V */
        n->color_matrix[2][2] = 2.032f * sat * scale;     /* U */
    } else {
        /* YIQ decoder — matches the legacy hardcoded coefficients
         * (pre-renaming, comp_decode_to_rgb was ntsc_yiq_to_rgb with
         * literal 1.1222/0.7391/etc coefficients) bit-exactly so NTSC
         * output is unchanged. */
        n->color_matrix[0][0] = warm_r * scale;
        n->color_matrix[0][1] = 1.1222f * scale;    /* I */
        n->color_matrix[0][2] = 0.7391f * scale;    /* Q */

        n->color_matrix[1][0] = warm_g * scale;
        n->color_matrix[1][1] = -0.3192f * scale;   /* I */
        n->color_matrix[1][2] = -0.7384f * scale;   /* Q */

        n->color_matrix[2][0] = warm_b * scale;
        n->color_matrix[2][1] = -1.2374f * scale;   /* I */
        n->color_matrix[2][2] = 1.9058f * scale;    /* Q */
    }
    n->color_bias[0] = bias_r * scale;
    n->color_bias[1] = bias_g * scale;
    n->color_bias[2] = bias_b * scale;
}

/* Switch the signal pipeline between NTSC (2C02) and PAL (2C07). Rebuilds
 * the signal table, resets samples_per_pixel and signal_w to the region's
 * values, re-applies FIR coefficients, and switches the output color
 * matrix (YIQ ↔ YUV).
 *
 * Demod tables are shared (both regions use the same 12-slot chroma wheel);
 * the decoder-side V flip for PAL is applied per-scanline inside the
 * waveform pipeline, not at precompute time.
 *
 * Safe to call at runtime (e.g. from an OSD region toggle). */
static inline void comp_set_region(Composite *n, int region) {
    if (region != COMP_REGION_NTSC && region != COMP_REGION_PAL)
        region = COMP_REGION_NTSC;
    CompositeSignal *s = &n->signal;
    s->region = region;

    /* Set decoder-axis tuning per region. */
    if (region == COMP_REGION_PAL) {
        s->samples_per_pixel = COMP_PAL_SAMPLES_PER_PIXEL;
        s->signal_w          = 256 * COMP_PAL_SAMPLES_PER_PIXEL;
        /* PAL decoder axis tuning. demod_rotate = +3 (= 90°) aligns
         * the 2C07 square wave's color phases with the YUV matrix's
         * V/U axes — analytically the best-fit single rotation for
         * matching the FBX Nostalgia 2C07 palette at luma 1 across
         * all 12 hues. Individual colors and luma tiers need slightly
         * different rotations (the FBX palette has per-entry artistic
         * adjustments), so residual errors exist but +3 minimizes the
         * mean.
         *
         * chroma_gain stays at 1.3 (NTSC default). At that gain,
         * color $11 (blue luma 1) decodes to approximately
         * (7, 106, 243), very close to the target (7, 100, 232).
         * Color $16 (red luma 1) to (185, 54, 45) vs target
         * (175, 37, 4) — recognizably red with slight mute. */
        s->demod_rotate = 3;
        s->chroma_gain  = 1.3f;
    } else {
        s->samples_per_pixel = COMP_NTSC_SAMPLES_PER_PIXEL;
        s->signal_w          = COMP_NTSC_SIGNAL_W;
        s->demod_rotate = 4;
        s->chroma_gain  = 1.3f;
    }

    /* Apply the output color matrix BEFORE the signal-table precompute.
     * The PAL precompute needs to invert this matrix to work backwards
     * from target RGB to signal (Y, V, U). */
    comp_apply_color_matrix(n, region);

    /* Generate the signal table. PAL uses a palette-driven precompute
     * that synthesizes sinusoidal signals which decode to the target
     * ppu_palette_2c07 RGB values via the just-configured color matrix
     * and demod_rotate. NTSC uses a Bisqwit-style square-wave model. */
    if (region == COMP_REGION_PAL) {
        comp_precompute_signal_table_pal(s);
    } else {
        comp_precompute_signal_table_ntsc(s);
    }

    /* FIR cutoffs are in normalized cycles/sample — both regions share
     * the 12-samples-per-subcarrier-cycle property, so the same cutoffs
     * reject the subcarrier equally well and no re-tuning is needed. */
    comp_redesign_firs(s);
}

/* Rebuild the 256-entry gamma lookup table for the output stage. Values
 * are applied to each final RGB byte in the emit pass so the curve
 * affects ALL channels uniformly (which is correct for display gamma,
 * which models the CRT's voltage→brightness nonlinearity). gamma=1.0
 * is a pass-through identity; gamma>1.0 darkens midtones (classic CRT
 * feel); gamma<1.0 brightens midtones. Typical CRT simulation: 1.10-1.20. */
static inline void comp_build_gamma_lut(Composite *n) {
    float g = n->gamma;
    if (g < 0.1f) g = 0.1f;
    if (g > 4.0f) g = 4.0f;
    if (g == 1.0f) {
        for (int i = 0; i < 256; i++) n->gamma_lut[i] = (uint8_t)i;
        return;
    }
    float inv_g = 1.0f / g;
    for (int i = 0; i < 256; i++) {
        float x = (float)i / 255.0f;
        float y = powf(x, inv_g);
        int out = (int)(y * 255.0f + 0.5f);
        if (out < 0) out = 0; if (out > 255) out = 255;
        n->gamma_lut[i] = (uint8_t)out;
    }
}

/* ============================================================================
 * Lifecycle
 * ============================================================================ */

static inline void comp_init(Composite *n, int out_w, int rows_per_scanline) {
    memset(n, 0, sizeof(Composite));
    if (rows_per_scanline < 1) rows_per_scanline = 1;
    if (rows_per_scanline > 12) rows_per_scanline = 12;
    n->rows_per_scanline = rows_per_scanline;
    n->luma_lowpass   = 0.78f;
    n->chroma_lowpass = 0.30f;
    n->luma_afterglow = 0.25f;
    n->luma_ringing   = 0.40f;
    n->chroma_ringing = 0.0f;
    n->bloom          = 0.40f;
    n->bloom_2d       = 0.0f;
    n->aperture_grille = 0.0f;
    n->vignette       = 0.35f;
    n->chromatic_conv = 0.0f;
    n->gamma          = 1.0f;
    n->persistence    = 0.0f;
    n->barrel         = 0.0f;
    n->hsync_wobble   = 0.0f;
    n->ghosting       = 0.0f;
    n->ghost_offset   = 8;     /* typical ghost delay, in output pixels */
    n->snow           = 0.0f;
    n->hum_bar        = 0.0f;
    n->black_floor    = 0;     /* true black */
    /* Tuned defaults from live A/B against reference ROMs: slight
     * desaturation tames the over-bright 2C02 waveform colors. */
    n->hue_deg        = 0.0f;
    n->saturation     = 0.90f;
    n->brightness     = 0.0f;
    n->contrast       = 1.0f;
    n->overscan_x     = 0;   /* no horizontal crop — wider viewport */
    n->overscan_y     = 0;   /* no vertical crop (no vertical filter artifacts) */
    n->out_w = out_w > COMP_MAX_W ? COMP_MAX_W : out_w;
    /* Each NES scanline produces `rows_per_scanline` output rows with
     * a brightness curve (bright at top, dimmer toward bottom = scanline
     * gap). Render at display resolution to avoid scaling moire. */
    int out_h = 240 * n->rows_per_scanline;
    n->output      = (uint8_t *)calloc(n->out_w * out_h * 3, 1);
    /* Post-processing scratch buffers:
     *   output_prev — previous-frame snapshot for vertical persistence
     *                 max-blend. Zero-initialized so the first frame
     *                 doesn't ghost off stale garbage.
     *   output_tmp  — in-place post-pass source (for barrel warp and
     *                 2D bloom reads) so we can read the un-modified
     *                 frame while writing the warped/bloomed version. */
    n->output_prev = (uint8_t *)calloc(n->out_w * out_h * 3, 1);
    n->output_tmp  = (uint8_t *)calloc(n->out_w * out_h * 3, 1);
    comp_build_gamma_lut(n);

    /* Waveform pipeline: precompute the 2C02 signal table and FIR taps.
     * Subcarrier at 8 samples/pixel sits at fs/12 ≈ 0.0833 cycles/sample.
     * Y cutoff 0.060 rejects subcarrier with an 11-tap Hamming FIR.
     * C cutoff 0.020 is a tight bandwidth-limit matching real 2C02
     * chroma response (~1.3 MHz I, ~0.5 MHz Q).
     *
     * Default region = NTSC. comp_set_region() can flip the whole
     * pipeline to PAL at runtime (rebuilds tables + FIRs). */
    n->signal.region            = COMP_REGION_NTSC;
    n->signal.samples_per_pixel = COMP_NTSC_SAMPLES_PER_PIXEL;
    n->signal.signal_w          = COMP_NTSC_SIGNAL_W;
    comp_precompute_signal_table_ntsc(&n->signal);
    comp_precompute_demod(&n->signal);
    /* Aggressive defaults to kill dot crawl. At 8 samples/pixel the
     * subcarrier sits at 1/12 cyc/sample = 0.0833 normalized. A Hamming
     * FIR's transition bandwidth is ~3.3/N, so to put 0.083 deep in the
     * stopband we need N large and cutoff low:
     *   N=25, cutoff=0.025: stopband starts at ~0.091, subcarrier at
     *   0.083 sits 88% of the way into the transition → ~45 dB reject.
     * This is 2x the cost of the old 11-tap default but well inside
     * the 16.7ms frame budget, and visually much cleaner. */
    /* Live-tuned: y_cutoff=0.043 gives a moderate Y lowpass that
     * preserves sharper luma edges (and some visible dot crawl) for
     * a more authentic classic-NTSC look. c_taps=47 with c_cutoff
     * 0.015 makes the chroma FIR significantly wider, producing
     * the softer color bleeding the 80s/90s TVs had. */
    n->signal.y_cutoff = 0.043f;
    n->signal.c_cutoff = 0.015f;
    n->signal.fir_y_n  = 37;
    n->signal.fir_c_n  = 47;
    /* Go through the clamping path so buffer sizes and parity are
     * enforced correctly regardless of what defaults are set above. */
    comp_redesign_firs(&n->signal);
    /* Phase defaults — dialed in against reference ROMs with the SDL
     * tuning hotkeys. demod_rotate = +4 /12 aligns the decoder reference
     * carrier to the 2C02's colorburst convention, which matches the
     * hues we get out of the static LUT path. */
    /* Tuned phase: line_adv=6 (180° flip between scanlines) with a
     * 4-field cycle advancing 6 slots per frame gives stable, gentle
     * dot crawl that averages in the viewer's eye. Comb filter off
     * by default — adaptive comb still has mild luma-edge artifacts
     * on detailed content and is better left for the user to enable
     * deliberately. */
    n->signal.phase_base       = 0;
    n->signal.phase_line_adv   = 6;
    n->signal.phase_field_adv  = 6;
    n->signal.phase_num_fields = 4;
    n->signal.demod_rotate     = 4;
    n->signal.chroma_gain      = 1.3f;
    n->signal.color_killer     = 0.05f;   /* mutes chroma on whites */
    n->signal.comb_filter      = 0;       /* off by default */
    n->signal.svideo           = 0;       /* composite by default */

    /* Populate the output decode matrix. Default is NTSC YIQ; switches
     * to PAL YUV on comp_set_region(n, COMP_REGION_PAL). */
    comp_apply_color_matrix(n, COMP_REGION_NTSC);

    /* ---- PAL-CRT state ----
     * Point the vendored PAL-CRT pipeline at a 240-row stage buffer
     * (one RGB row per NES scanline). The PAL branch of comp_process
     * then iterates those 240 rows through comp_emit_output_row_rgb,
     * which replicates each row `rows_per_scanline` times with the CRT
     * beam profile — the same one the NTSC path uses. Without this
     * second stage, PAL output would be flat (no visible scanline gaps,
     * no vignette, no HDR highlight knee) and look nothing like NTSC.
     *
     * PAL-CRT's palette→signal→RGB transform is entirely self-contained
     * (this is NOT an intermediate palette LUT stage). The stage buffer
     * represents "the RGB row a PAL TV would display for this NES
     * scanline"; our beam profile pass models the CRT beam tracing that
     * row out onto phosphor across `rows_per_scanline` display lines. */
    n->palcrt_rgb = (uint8_t *)calloc((size_t)(n->out_w * 240 * 3), 1);
    pal_init(&n->palcrt, n->out_w, 240, PAL_PIX_FORMAT_RGB, n->palcrt_rgb);
    /* PAL-CRT defaults (pal_reset): saturation=10, contrast=180,
     * brightness=0, scanlines=0. Leave those alone — they're tuned
     * against real 2C07 captures, and the scanlines setting is crude
     * (just blanks the bottom N rows of each line group). Our beam
     * profile does the real scanline look, so keep PAL-CRT at 0.
     *
     * Enable chroma_correction: this is the Hanover-bar-killing 1H
     * delay-line U/V averaging, the single most recognizable PAL
     * feature and the reason PAL is famous for being robust to phase
     * errors. */
    n->palcrt.chroma_correction = 1;
    memset(&n->palcrt_settings, 0, sizeof(n->palcrt_settings));
}

static inline void comp_destroy(Composite *n) {
    free(n->output);        n->output      = NULL;
    free(n->output_prev);   n->output_prev = NULL;
    free(n->output_tmp);    n->output_tmp  = NULL;
    free(n->palcrt_rgb);    n->palcrt_rgb  = NULL;
}

/* Resize the composite output buffer without touching tuning state.
 * Used by the SDL frontend when the window size changes so the
 * composite texture matches the display's integer scanline cadence —
 * preventing the moiré banding that happens when SDL nearest-neighbor
 * upscales a low-rps texture to a high-resolution display. */
static inline void comp_resize(Composite *n, int out_w, int rows_per_scanline) {
    if (rows_per_scanline < 1) rows_per_scanline = 1;
    if (rows_per_scanline > 12) rows_per_scanline = 12;
    if (out_w < 1) out_w = 1;
    if (out_w > COMP_MAX_W) out_w = COMP_MAX_W;
    n->rows_per_scanline = rows_per_scanline;
    n->out_w = out_w;
    int out_h = 240 * n->rows_per_scanline;
    free(n->output);       n->output      = (uint8_t *)calloc((size_t)(n->out_w * out_h * 3), 1);
    free(n->output_prev);  n->output_prev = (uint8_t *)calloc((size_t)(n->out_w * out_h * 3), 1);
    free(n->output_tmp);   n->output_tmp  = (uint8_t *)calloc((size_t)(n->out_w * out_h * 3), 1);

    /* Re-point PAL-CRT at the new stage buffer. Source height is fixed
     * at 240 rows (one per NES scanline); only the width scales with
     * our output width. pal_resize preserves internal state (ccf,
     * vsync, hsync, rn) so the burst accumulator and sync lock stay
     * warm across resizes. Flip field_initialized to 0 to force the
     * first pal_modulate after a resize to re-run setup_field(). */
    free(n->palcrt_rgb);
    n->palcrt_rgb = (uint8_t *)calloc((size_t)(n->out_w * 240 * 3), 1);
    pal_resize(&n->palcrt, n->out_w, 240, PAL_PIX_FORMAT_RGB, n->palcrt_rgb);
    n->palcrt_settings.field_initialized = 0;
}

/* ============================================================================
 * Internal helpers
 * ============================================================================ */

/* Matrix-driven Y/C1/C2 → RGB decoder.
 *
 * The 3×3 matrix and bias vector in Composite are populated at init
 * (and at runtime region changes) via comp_apply_color_matrix. NTSC uses
 * YIQ coefficients, PAL uses YUV — the same function handles both via
 * the matrix swap.
 *
 * The c1/c2 channel labels are region-dependent:
 *   NTSC: c1 = I (cos demod), c2 = Q (sin demod)
 *   PAL:  c1 = V (cos demod), c2 = U (sin demod)
 *
 * In the waveform pipeline the demod output Ip goes into the c1 slot
 * and Qp into c2 for both regions — the matrix reinterprets them. */
static void comp_decode_to_rgb(const Composite *n,
                                float y, float c1, float c2,
                                uint8_t *r, uint8_t *g, uint8_t *b) {
    const float (*m)[3] = (const float (*)[3])n->color_matrix;
    const float *bs = n->color_bias;
    float rf = m[0][0] * y + m[0][1] * c1 + m[0][2] * c2 + bs[0];
    float gf = m[1][0] * y + m[1][1] * c1 + m[1][2] * c2 + bs[1];
    float bf = m[2][0] * y + m[2][1] * c1 + m[2][2] * c2 + bs[2];
    if (rf < 0.0f) rf = 0.0f; if (rf > 255.0f) rf = 255.0f;
    if (gf < 0.0f) gf = 0.0f; if (gf > 255.0f) gf = 255.0f;
    if (bf < 0.0f) bf = 0.0f; if (bf > 255.0f) bf = 255.0f;
    *r = (uint8_t)rf;
    *g = (uint8_t)gf;
    *b = (uint8_t)bf;
}

/* Horizontal luma afterglow — simulates phosphor decay trailing the beam.
 * The beam moves left→right across the scanline, and phosphors take time
 * to fade, so bright pixels leave a rightward decaying trail. This is a
 * physical effect separate from NTSC bandwidth limiting. */
static void comp_afterglow_row(float *row, int w, float amount,
                                float pixels_per_nes) {
    if (amount < 0.01f) return;
    /* Decay per pixel: tuned so trail fades over ~1-2 NES pixels at default. */
    float decay = expf(-1.5f / (pixels_per_nes * (0.5f + amount * 1.5f)));
    float mix = amount * 0.35f;
    float trail = row[0];
    for (int x = 0; x < w; x++) {
        trail *= decay;
        if (row[x] > trail) trail = row[x];
        /* Lift the current pixel by the trail, but only if trail is brighter
         * (we never darken content, only add afterglow). */
        if (trail > row[x])
            row[x] = row[x] + (trail - row[x]) * mix;
    }
}

/* Chroma overshoot (composite chroma amplifier ringing). Parallel to
 * comp_ringing_row but for the I/Q channels. Real composite decoders
 * couldn't perfectly isolate the 3.58 MHz subcarrier, so saturation
 * transitions produced slight chroma ringing (a colored halo on the
 * high-saturation side of a sharp color edge). Formula matches
 * comp_ringing_row: row' = row + k*(row - lowpass(row)). Clamps are
 * looser since chroma is centered on zero and can swing negative. */
static void comp_chroma_ringing_row(float *row, int w, float amount) {
    if (amount < 0.01f) return;
    static float sm[COMP_MAX_W];
    sm[0] = (row[0] * 2.0f + row[1]) * (1.0f / 3.0f);
    for (int x = 1; x < w - 1; x++)
        sm[x] = (row[x-1] + row[x] + row[x+1]) * (1.0f / 3.0f);
    sm[w-1] = (row[w-2] + row[w-1] * 2.0f) * (1.0f / 3.0f);
    float k = amount * 0.9f;
    for (int x = 0; x < w; x++) {
        float boost = row[x] + (row[x] - sm[x]) * k;
        /* Chroma clamp range: I and Q stay roughly in [-0.6, 0.6] after
         * demod + gain + FIR. Allow some overshoot headroom. */
        if (boost < -1.0f) boost = -1.0f;
        if (boost >  1.0f) boost =  1.0f;
        row[x] = boost;
    }
}

/* Luma overshoot (video amplifier ringing).
 * Real CRT video amps have a slight high-frequency boost that creates
 * overshoot on sharp bright→dark transitions (the characteristic bright
 * halo just before a dark edge). Implemented as: row' = row + k*(row - lp)
 * where lp is a quick smoothing of row. */
static void comp_ringing_row(float *row, int w, float amount) {
    if (amount < 0.01f) return;
    static float sm[COMP_MAX_W];
    /* 3-tap smoothing, clamp at edges */
    sm[0] = (row[0] * 2.0f + row[1]) * (1.0f / 3.0f);
    for (int x = 1; x < w - 1; x++)
        sm[x] = (row[x-1] + row[x] + row[x+1]) * (1.0f / 3.0f);
    sm[w-1] = (row[w-2] + row[w-1] * 2.0f) * (1.0f / 3.0f);
    float k = amount * 0.9f;
    for (int x = 0; x < w; x++) {
        float boost = row[x] + (row[x] - sm[x]) * k;
        if (boost < 0.0f) boost = 0.0f;
        if (boost > 1.15f) boost = 1.15f;  /* allow slight overshoot above 1 */
        row[x] = boost;
    }
}

/* Horizontal phosphor bloom on luma channel.
 * Thresholds bright pixels, blurs wide, adds glow back to Y. */
static void comp_bloom_row(float *row, int w, float amount, float pixels_per_nes) {
    if (amount < 0.01f) return;
    static float bright[COMP_MAX_W], blurred[COMP_MAX_W];

    /* Extract bright areas above threshold. Early-out if no pixels
     * exceed the threshold (common for dark rows like backgrounds). */
    float threshold = 0.60f;
    int any_bright = 0;
    for (int x = 0; x < w; x++) {
        float v = row[x] - threshold;
        if (v > 0.0f) { bright[x] = v; any_bright = 1; }
        else bright[x] = 0.0f;
    }
    if (!any_bright) return;

    /* Tight gaussian (two-pass box). Smaller radius than before for
     * a tighter, more intense glow around bright features. */
    int radius = (int)(pixels_per_nes * 3.5f * amount);
    if (radius < 2) radius = 2;
    if (radius > 30) radius = 30;

    /* Box blur pass 1: bright -> blurred */
    float inv_diam = 1.0f / (float)(2 * radius + 1);
    float running = 0.0f;
    for (int x = 0; x < radius && x < w; x++) running += bright[x];
    for (int x = 0; x < w; x++) {
        int ra = x + radius; if (ra >= w) ra = w - 1;
        int la = x - radius - 1;
        running += bright[ra];
        if (la >= 0) running -= bright[la];
        blurred[x] = running * inv_diam;
    }

    /* Box blur pass 2: blurred -> bright (second pass smooths the box shape) */
    running = 0.0f;
    for (int x = 0; x < radius && x < w; x++) running += blurred[x];
    for (int x = 0; x < w; x++) {
        int ra = x + radius; if (ra >= w) ra = w - 1;
        int la = x - radius - 1;
        running += blurred[ra];
        if (la >= 0) running -= blurred[la];
        bright[x] = running * inv_diam;
    }

    /* Add glow back to luma with soft knee to avoid hard clipping.
     * Instead of row += glow (which can push past 1.0), use a soft
     * saturation: y' = y + glow * (1 - y) so highlights get lifted
     * but never exceed 1.0. */
    float intensity = amount * 1.0f;
    for (int x = 0; x < w; x++) {
        float g = bright[x] * intensity;
        float y = row[x];
        float headroom = 1.0f - y;
        if (headroom < 0.0f) headroom = 0.0f;
        row[x] = y + g * headroom;
    }
}

/* Apply a symmetric FIR in-place on `row[0..w-1]`. Edge handling is
 * done by copying the row into a PADDED buffer with mirrored edges
 * so the hot inner loop has no conditionals.
 *
 * Hot path: process 4 output pixels at a time with explicit NEON
 * (4-wide float) or AVX2 (8-wide float) intrinsics so we get
 * guaranteed FMA codegen instead of relying on the auto-vectorizer,
 * which in practice only manages `fmul.4s` scattered through the
 * inner loop (not the clean `fmla.4s` chain we want). With 4
 * independent accumulators we break the FMA latency chain and
 * saturate the 4 (M1) / 2 (x86-FMA) FMA units.
 *
 * Non-SIMD fallback keeps the 4-way-ILP scalar version for platforms
 * without NEON or AVX2 — good enough to run correctly, but the SIMD
 * paths are 3-5× faster on the actual hot frame pipeline.
 *
 * Buffer sizing: COMP_SIGNAL_MAX (2560, = PAL scanline at 10 spp) +
 * 2*64 headroom for the widest tap count we allow (63). Aligned to
 * 32 bytes so SIMD loads use aligned moves wherever possible. */
#define COMP_FIR_PAD 64
static void comp_fir_symmetric(float * __restrict row, int w,
                                const float * __restrict taps, int n) {
    static _Alignas(32) float out[COMP_SIGNAL_MAX];
    static _Alignas(32) float pad[COMP_SIGNAL_MAX + 2 * COMP_FIR_PAD];
    int half = n / 2;
    if (half > COMP_FIR_PAD) half = COMP_FIR_PAD;   /* safety */

    /* Build padded row with mirrored edges around x=0 and x=w-1.
     * For x<0 use row[-x] (reflect); for x>=w use row[2w-2-x]. */
    float *p = pad + COMP_FIR_PAD;
    memcpy(p, row, (size_t)w * sizeof(float));
    for (int k = 1; k <= half; k++) {
        int left  = (k < w) ? k : w - 1;
        int right = (w - 1 - k >= 0) ? (w - 1 - k) : 0;
        p[-k]    = row[left];       /* mirror around left edge */
        p[w - 1 + k] = row[right];  /* mirror around right edge */
    }

    const float * __restrict tps  = taps;
    float       * __restrict out_p = out;
    const float * __restrict p_in  = p;
    int x = 0;

#if defined(__ARM_NEON)
    /* NEON: process 4 output pixels per iteration with 4 independent
     * 4-wide accumulators. The j-stride layout (each of the 4 accs
     * advances through taps in lockstep by +4) lets the CPU dispatch
     * 4 FMAs per cycle with no loop-carried dep chain. Tail (k & 3
     * leftover taps) collapses back onto one accumulator. */
    for (; x + 4 <= w; x += 4) {
        const float *pw = p_in + x - half;
        float32x4_t a0 = vdupq_n_f32(0.0f);
        float32x4_t a1 = vdupq_n_f32(0.0f);
        float32x4_t a2 = vdupq_n_f32(0.0f);
        float32x4_t a3 = vdupq_n_f32(0.0f);
        int k = 0;
        for (; k + 4 <= n; k += 4) {
            a0 = vfmaq_n_f32(a0, vld1q_f32(pw + k + 0), tps[k + 0]);
            a1 = vfmaq_n_f32(a1, vld1q_f32(pw + k + 1), tps[k + 1]);
            a2 = vfmaq_n_f32(a2, vld1q_f32(pw + k + 2), tps[k + 2]);
            a3 = vfmaq_n_f32(a3, vld1q_f32(pw + k + 3), tps[k + 3]);
        }
        float32x4_t acc = vaddq_f32(vaddq_f32(a0, a1), vaddq_f32(a2, a3));
        for (; k < n; k++) {
            acc = vfmaq_n_f32(acc, vld1q_f32(pw + k), tps[k]);
        }
        vst1q_f32(&out_p[x], acc);
    }
#elif defined(__AVX2__) && defined(__FMA__)
    /* AVX2: process 8 output pixels per iteration. Same 4-way ILP on
     * the taps dimension. On Intel/AMD with 2 FMA units this is
     * roughly peak FLOPs for single-precision FIR. */
    for (; x + 8 <= w; x += 8) {
        const float *pw = p_in + x - half;
        __m256 a0 = _mm256_setzero_ps();
        __m256 a1 = _mm256_setzero_ps();
        __m256 a2 = _mm256_setzero_ps();
        __m256 a3 = _mm256_setzero_ps();
        int k = 0;
        for (; k + 4 <= n; k += 4) {
            a0 = _mm256_fmadd_ps(_mm256_loadu_ps(pw + k + 0),
                                 _mm256_broadcast_ss(&tps[k + 0]), a0);
            a1 = _mm256_fmadd_ps(_mm256_loadu_ps(pw + k + 1),
                                 _mm256_broadcast_ss(&tps[k + 1]), a1);
            a2 = _mm256_fmadd_ps(_mm256_loadu_ps(pw + k + 2),
                                 _mm256_broadcast_ss(&tps[k + 2]), a2);
            a3 = _mm256_fmadd_ps(_mm256_loadu_ps(pw + k + 3),
                                 _mm256_broadcast_ss(&tps[k + 3]), a3);
        }
        __m256 acc = _mm256_add_ps(_mm256_add_ps(a0, a1),
                                   _mm256_add_ps(a2, a3));
        for (; k < n; k++) {
            acc = _mm256_fmadd_ps(_mm256_loadu_ps(pw + k),
                                  _mm256_broadcast_ss(&tps[k]), acc);
        }
        _mm256_storeu_ps(&out_p[x], acc);
    }
#endif

    /* Scalar tail (also the pure fallback path on non-NEON/non-AVX2
     * targets). 4-way ILP on taps = break loop-carried dep chain on
     * the accumulator. */
    for (; x < w; x++) {
        const float * __restrict pw = p_in + x - half;
        float s0 = 0.0f, s1 = 0.0f, s2 = 0.0f, s3 = 0.0f;
        int k = 0;
        for (; k + 4 <= n; k += 4) {
            s0 += tps[k + 0] * pw[k + 0];
            s1 += tps[k + 1] * pw[k + 1];
            s2 += tps[k + 2] * pw[k + 2];
            s3 += tps[k + 3] * pw[k + 3];
        }
        float s = (s0 + s1) + (s2 + s3);
        for (; k < n; k++) s += tps[k] * pw[k];
        out_p[x] = s;
    }
    memcpy(row, out, (size_t)w * sizeof(float));
}

/* Downsample `in[0..in_w-1]` → `out[0..out_w-1]` via a block average
 * weighted by fractional coverage. Each input sample is touched once,
 * so this is O(in_w) total. Use *after* the Y FIR has already lowpass
 * filtered the input so downsampling doesn't alias. */
static void comp_downsample(const float *in, int in_w, float *out, int out_w) {
    if (out_w <= 0 || in_w <= 0) return;
    float ratio = (float)in_w / (float)out_w;
    float inv_ratio = 1.0f / ratio;
    for (int x = 0; x < out_w; x++) {
        float start = (float)x * ratio;
        float end   = start + ratio;
        int   i0 = (int)start;
        int   i1 = (int)end;
        if (i0 >= in_w) i0 = in_w - 1;
        if (i1 >= in_w) i1 = in_w - 1;
        /* Fractional weights at the two ends */
        float w0 = 1.0f - (start - (float)i0);
        float w1 = end - (float)i1;
        if (i0 == i1) {
            out[x] = in[i0];   /* sub-sample interval lives entirely in one input */
            continue;
        }
        float sum = in[i0] * w0;
        for (int k = i0 + 1; k < i1; k++) sum += in[k];
        sum += in[i1] * w1;
        out[x] = sum * inv_ratio;
    }
}

/* Pass-2 beam-profile write: fill `rps` output rows, blending the
 * narrow/wide scanline curves per-pixel by the Y² cache. Hot loop —
 * SIMD-optimized with NEON (16 pixels / iter, interleaved RGB store)
 * and AVX2 (16 pixels / iter via u16 widening). Scalar fallback
 * handles the tail + non-SIMD targets.
 *
 * Inputs: bright_r/g/b/ym caches populated by the calling emit_row
 * pass 1, plus curve_narrow/curve_wide tables for the current `rps`,
 * and the destination base pointer out0.
 *
 * This sits in a static helper so both comp_emit_output_row (YIQ
 * input) and comp_emit_output_row_rgb (RGB input) share the hot
 * write — they do different pass 1 work but pass 2 is identical. */
static void comp_emit_pass2_write(uint8_t * __restrict out0,
                                   const uint8_t * __restrict br,
                                   const uint8_t * __restrict bg,
                                   const uint8_t * __restrict bb,
                                   const uint8_t * __restrict ym,
                                   int W, int rps,
                                   const int *curve_narrow,
                                   const int *curve_wide) {
    int row_stride = W * 3;
    for (int k = 0; k < rps; k++) {
        int cn = curve_narrow[k];
        int cw = curve_wide[k];
        int cdiff = cw - cn;
        uint8_t *dst = out0 + k * row_stride;
        int ox = 0;
#if defined(__ARM_NEON)
        /* NEON hot path: 16 pixels / iter.
         * c_mod = cn + ((cdiff * ym) >> 8), clamped to u8 with
         * saturating narrow. Then per-channel widening multiply
         * (u8 * u8 → u16), shift right 8, re-pack, and interleaved
         * store via vst3q_u8 (writes 48 bytes contiguously at 16 px). */
        int16x8_t cdiff_s16 = vdupq_n_s16((int16_t)cdiff);
        int16x8_t cn_s16    = vdupq_n_s16((int16_t)cn);
        for (; ox + 16 <= W; ox += 16) {
            uint8x16_t v_ym = vld1q_u8(&ym[ox]);
            /* Widen ym → s16 (zero-extend is safe, ym is uint8). */
            int16x8_t ym_lo = vreinterpretq_s16_u16(vmovl_u8(vget_low_u8(v_ym)));
            int16x8_t ym_hi = vreinterpretq_s16_u16(vmovl_u8(vget_high_u8(v_ym)));
            /* cdiff * ym, arithmetic shift right 8, add cn. */
            int16x8_t cm_lo = vaddq_s16(cn_s16,
                                vshrq_n_s16(vmulq_s16(ym_lo, cdiff_s16), 8));
            int16x8_t cm_hi = vaddq_s16(cn_s16,
                                vshrq_n_s16(vmulq_s16(ym_hi, cdiff_s16), 8));
            /* Saturating narrow to u8; c_mod ∈ [0, 256] → clamp at 255
             * (we lose 1/256 of precision at the upper edge, invisible). */
            uint8x16_t c_mod = vcombine_u8(vqmovun_s16(cm_lo),
                                            vqmovun_s16(cm_hi));
            uint8x16_t v_br = vld1q_u8(&br[ox]);
            uint8x16_t v_bg = vld1q_u8(&bg[ox]);
            uint8x16_t v_bb = vld1q_u8(&bb[ox]);
            /* Per-channel (ch * c_mod) >> 8. */
            uint16x8_t pr_lo = vmull_u8(vget_low_u8(v_br),  vget_low_u8(c_mod));
            uint16x8_t pr_hi = vmull_u8(vget_high_u8(v_br), vget_high_u8(c_mod));
            uint16x8_t pg_lo = vmull_u8(vget_low_u8(v_bg),  vget_low_u8(c_mod));
            uint16x8_t pg_hi = vmull_u8(vget_high_u8(v_bg), vget_high_u8(c_mod));
            uint16x8_t pb_lo = vmull_u8(vget_low_u8(v_bb),  vget_low_u8(c_mod));
            uint16x8_t pb_hi = vmull_u8(vget_high_u8(v_bb), vget_high_u8(c_mod));
            uint8x16x3_t rgb;
            rgb.val[0] = vcombine_u8(vshrn_n_u16(pr_lo, 8), vshrn_n_u16(pr_hi, 8));
            rgb.val[1] = vcombine_u8(vshrn_n_u16(pg_lo, 8), vshrn_n_u16(pg_hi, 8));
            rgb.val[2] = vcombine_u8(vshrn_n_u16(pb_lo, 8), vshrn_n_u16(pb_hi, 8));
            vst3q_u8(&dst[ox * 3], rgb);
        }
#endif
        for (; ox < W; ox++) {
            int y_mod = ym[ox];
            int c_mod = cn + ((cdiff * y_mod) >> 8);
            dst[ox*3]   = (uint8_t)((br[ox] * c_mod) >> 8);
            dst[ox*3+1] = (uint8_t)((bg[ox] * c_mod) >> 8);
            dst[ox*3+2] = (uint8_t)((bb[ox] * c_mod) >> 8);
        }
    }
}

/* Emit one source scanline into the output framebuffer. Applies vignette,
 * converts YIQ→RGB, and writes `rows_per_scanline` output rows with the
 * scanline brightness curve (phosphor glow).
 *
 * Shared between the legacy RGB path and the waveform path so both pipelines
 * produce identical output formatting for equivalent filtered Y/I/Q input.
 */
static void comp_emit_output_row(Composite *n, int sy,
                                  const float *filtY, const float *filtI,
                                  const float *filtQ) {
    const int W = n->out_w;
    float vignette_strength = n->vignette * 0.45f;
    float inv_half_w = 2.0f / (float)W;

    /* Vignette: precompute per-row multiplier table (once per sy).
     * Stored as fixed-point [0,256]. 256 means full brightness. */
    static int vig_row[COMP_MAX_W];
    {
        float vy = (float)sy / 239.0f * 2.0f - 1.0f;
        float vy2 = vy * vy;
        for (int ox = 0; ox < W; ox++) {
            float vx = (float)ox * inv_half_w - 1.0f;
            float d2 = vx * vx + vy2;
            float excess = d2 - 1.0f;
            float vig = 1.0f - (excess > 0.0f ? excess : 0.0f) * vignette_strength;
            if (vig < 0.0f) vig = 0.0f;
            vig_row[ox] = (int)(vig * 256.0f);
        }
    }

    /* Each source scanline produces `rows_per_scanline` output rows with
     * a brightness curve (phosphor glow). Rendering at display resolution
     * avoids scaling moire. */
    int rps = n->rows_per_scanline;
    int out_row_base = sy * rps * W * 3;

    /* CRT electron beam profile — two Gaussian scanline curves that
     * we blend per-pixel based on brightness:
     *
     *   curve_narrow: tight beam (sigma≈0.22), visible scanline gaps
     *                 at both top and bottom of each NES scanline.
     *                 Used for dark pixels.
     *   curve_wide:   wide beam (sigma≈0.55), almost no gap between
     *                 scanlines. Used for bright pixels (the CRT
     *                 beam physically bloomed wider on highlights).
     *
     * Each curve PEAKS in the MIDDLE of the scanline and fades
     * symmetrically at top and bottom — so adjacent scanlines have
     * a smooth dark-to-dark transition at the boundary (no hard
     * "256 → next-row-256" cliff).
     *
     * Per-pixel blend factor comes from bright_ym (Y² cache) so the
     * beam expands smoothly with luma, which is how a real CRT's
     * electron gun behaves. */
    int curve_narrow[12], curve_wide[12];
    {
        const float sigma_narrow = 0.22f;
        const float sigma_wide   = 0.55f;
        const float inv_rps = 1.0f / (float)rps;
        const float inv_2sn2 = 1.0f / (2.0f * sigma_narrow * sigma_narrow);
        const float inv_2sw2 = 1.0f / (2.0f * sigma_wide * sigma_wide);
        for (int k = 0; k < rps; k++) {
            float pos = ((float)k + 0.5f) * inv_rps;   /* 0..1 inside scanline */
            float d   = pos - 0.5f;
            float n_  = expf(-(d * d) * inv_2sn2);
            float w_  = expf(-(d * d) * inv_2sw2);
            curve_narrow[k] = (int)(n_ * 256.0f + 0.5f);
            curve_wide[k]   = (int)(w_ * 256.0f + 0.5f);
        }
    }

    /* Per-scanline precompute for the color controls (hue/sat/contrast/
     * brightness). Neutral defaults short-circuit the expensive paths. */
    float hue_c = 1.0f, hue_s = 0.0f;
    if (n->hue_deg != 0.0f) {
        float a = n->hue_deg * (float)M_PI / 180.0f;
        hue_c = cosf(a);
        hue_s = sinf(a);
    }
    float sat = n->saturation;
    float contrast = n->contrast;
    float brightness = n->brightness;

    /* Pass 1: compute per-pixel "peak brightness" RGB + Y² cache.
     * No writes yet — stage the color data for the beam profile. */
    static uint8_t bright_r[COMP_MAX_W], bright_g[COMP_MAX_W];
    static uint8_t bright_b[COMP_MAX_W], bright_ym[COMP_MAX_W];
    uint8_t *out0 = n->output + out_row_base;
    for (int ox = 0; ox < W; ox++) {
        float Y = filtY[ox], I = filtI[ox], Q = filtQ[ox];

        /* Apply user color controls in YIQ space before RGB conversion
         * (keeps chroma/luma operations orthogonal and correct). */
        Y = (Y - 0.5f) * contrast + 0.5f + brightness;
        if (sat != 1.0f) { I *= sat; Q *= sat; }
        if (n->hue_deg != 0.0f) {
            float nI = I * hue_c - Q * hue_s;
            float nQ = I * hue_s + Q * hue_c;
            I = nI; Q = nQ;
        }

        /* Vignette as a pure luminance attenuation — keeps chroma
         * untouched so darkened corners don't desaturate. */
        int vig = vig_row[ox];
        if (vig < 256) Y = Y * (float)vig * (1.0f / 256.0f);

        /* HDR-ish soft-knee highlight compression. Below 0.75 the
         * response is linear; above 0.75 we asymptote toward 1.0 via
         * a Reinhard-style curve so bright content preserves detail
         * instead of hard-clipping at 255. This turns highlights that
         * would otherwise blow out white into a softer glow and gives
         * the image a pseudo-HDR "hot" feel on an SDR display.
         *
         *   knee = 0.75
         *   if Y <= knee: Y stays linear
         *   else:         Y = knee + (1-knee) * (1 - 1/(1+k*(Y-knee)))
         * With k≈2.5 the curve lifts up smoothly and asymptotes near
         * 1.0 as Y grows toward 2.0, so even "above-white" values map
         * into a useful display range. */
        if (Y < 0.0f) Y = 0.0f;
        else if (Y > 0.75f) {
            float x = Y - 0.75f;
            Y = 0.75f + 0.25f * (1.0f - 1.0f / (1.0f + 2.5f * x));
        }
        if (Y > 1.0f) Y = 1.0f;

        uint8_t r, g, b;
        comp_decode_to_rgb(n, Y, I, Q, &r, &g, &b);

        bright_r[ox] = r;
        bright_g[ox] = g;
        bright_b[ox] = b;
        bright_ym[ox] = (uint8_t)(Y * Y * 255.0f);
    }

    /* Phosphor mask / chromatic convergence / gamma are NOT applied here.
     * They moved to the comp_post_process pass so they run AFTER barrel
     * distortion — otherwise the high-frequency mask/convergence patterns
     * interact with the radial warp and produce circular Moire. In output
     * space (post-barrel) these patterns are uniform across the screen. */

    /* Pass 2: beam-profile write, delegated to the shared SIMD helper. */
    comp_emit_pass2_write(out0, bright_r, bright_g, bright_b, bright_ym,
                          W, rps, curve_narrow, curve_wide);
}

/* Emit one source scanline into the output framebuffer, starting from
 * a pre-decoded RGB row (what comes out of PAL-CRT's stage buffer).
 *
 * This is the PAL path's analogue of comp_emit_output_row. Both apply
 * the same CRT beam profile, vignette, HDR highlight knee, and aperture
 * grille, so NTSC and PAL output have matching CRT character. What's
 * different: this function skips the YIQ→RGB matrix decode (PAL-CRT
 * already handed us RGB) and skips the hue/sat/contrast/brightness
 * controls (PAL-CRT has its own saturation/contrast, and hue-wheel
 * rotation on a YIQ decoder doesn't map cleanly onto post-decoded RGB).
 *
 * Y is derived from RGB via luminance weights so vignette + HDR knee
 * act on perceptual brightness, and we reapply the resulting scale to
 * all three channels so hue is preserved through the attenuation. */
static void comp_emit_output_row_rgb(Composite *n, int sy,
                                      const uint8_t *row_rgb) {
    const int W = n->out_w;
    float vignette_strength = n->vignette * 0.45f;
    float inv_half_w = 2.0f / (float)W;

    /* Vignette multiplier table, same structure as the YIQ path. */
    static int vig_row[COMP_MAX_W];
    {
        float vy = (float)sy / 239.0f * 2.0f - 1.0f;
        float vy2 = vy * vy;
        for (int ox = 0; ox < W; ox++) {
            float vx = (float)ox * inv_half_w - 1.0f;
            float d2 = vx * vx + vy2;
            float excess = d2 - 1.0f;
            float vig = 1.0f - (excess > 0.0f ? excess : 0.0f) * vignette_strength;
            if (vig < 0.0f) vig = 0.0f;
            vig_row[ox] = (int)(vig * 256.0f);
        }
    }

    int rps = n->rows_per_scanline;
    int out_row_base = sy * rps * W * 3;

    /* Beam profile: identical to comp_emit_output_row so scanline
     * character matches across NTSC and PAL. */
    int curve_narrow[12], curve_wide[12];
    {
        const float sigma_narrow = 0.22f;
        const float sigma_wide   = 0.55f;
        const float inv_rps = 1.0f / (float)rps;
        const float inv_2sn2 = 1.0f / (2.0f * sigma_narrow * sigma_narrow);
        const float inv_2sw2 = 1.0f / (2.0f * sigma_wide * sigma_wide);
        for (int k = 0; k < rps; k++) {
            float pos = ((float)k + 0.5f) * inv_rps;
            float d   = pos - 0.5f;
            float n_  = expf(-(d * d) * inv_2sn2);
            float w_  = expf(-(d * d) * inv_2sw2);
            curve_narrow[k] = (int)(n_ * 256.0f + 0.5f);
            curve_wide[k]   = (int)(w_ * 256.0f + 0.5f);
        }
    }

    /* Pass 1: stage per-pixel "peak brightness" RGB + Y² cache.
     * Y is derived from the input RGB via luminance weights so the
     * vignette attenuation and HDR knee act on perceptual brightness;
     * the resulting scale is applied uniformly to R/G/B so hue is
     * preserved through the attenuation. */
    static uint8_t bright_r[COMP_MAX_W], bright_g[COMP_MAX_W];
    static uint8_t bright_b[COMP_MAX_W], bright_ym[COMP_MAX_W];
    uint8_t *out0 = n->output + out_row_base;
    for (int ox = 0; ox < W; ox++) {
        float rf = (float)row_rgb[ox * 3 + 0] * (1.0f / 255.0f);
        float gf = (float)row_rgb[ox * 3 + 1] * (1.0f / 255.0f);
        float bf = (float)row_rgb[ox * 3 + 2] * (1.0f / 255.0f);
        /* BT.601 luminance — matches our warm-tint decoder's Y-axis
         * weighting well enough for vignette + HDR-knee purposes. */
        float Y = 0.299f * rf + 0.587f * gf + 0.114f * bf;
        if (Y < 1e-5f) Y = 1e-5f;  /* avoid div-by-zero below */
        float Y_orig = Y;

        /* Vignette as a pure luminance attenuation. */
        int vig = vig_row[ox];
        if (vig < 256) Y = Y * (float)vig * (1.0f / 256.0f);

        /* HDR highlight knee — matches comp_emit_output_row exactly. */
        if (Y < 0.0f) Y = 0.0f;
        else if (Y > 0.75f) {
            float x = Y - 0.75f;
            Y = 0.75f + 0.25f * (1.0f - 1.0f / (1.0f + 2.5f * x));
        }
        if (Y > 1.0f) Y = 1.0f;

        /* Reapply the Y change to RGB uniformly (preserves hue). */
        float scale = Y / Y_orig;
        float rr = rf * scale * 255.0f;
        float gg = gf * scale * 255.0f;
        float bb = bf * scale * 255.0f;
        if (rr < 0.0f) rr = 0.0f; if (rr > 255.0f) rr = 255.0f;
        if (gg < 0.0f) gg = 0.0f; if (gg > 255.0f) gg = 255.0f;
        if (bb < 0.0f) bb = 0.0f; if (bb > 255.0f) bb = 255.0f;
        bright_r[ox]  = (uint8_t)rr;
        bright_g[ox]  = (uint8_t)gg;
        bright_b[ox]  = (uint8_t)bb;
        bright_ym[ox] = (uint8_t)(Y * Y * 255.0f);
    }

    /* Phosphor mask / chromatic convergence / gamma run in the
     * comp_post_process pass, AFTER barrel distortion. See
     * comp_emit_output_row for why. */

    /* Pass 2: beam-profile write, delegated to the shared SIMD helper. */
    comp_emit_pass2_write(out0, bright_r, bright_g, bright_b, bright_ym,
                          W, rps, curve_narrow, curve_wide);
}

/* ============================================================================
 * Main Pipeline
 * ============================================================================ */

/* Waveform path: generates composite signal directly from NES palette
 * indices + emphasis bits using Bisqwit's 2C02 model, then runs a proper
 * FIR-based Y/I/Q decode. This is the sole composite renderer —
 * comp_process is just a thin wrapper that forwards here. */
static void comp_process_waveform(Composite *n, const uint16_t *idx_fb) {
    const int W = n->out_w;
    if (!n->output || W == 0) return;

    /* Scratch buffers — single-threaded, file-scope static.
     * Sized to COMP_SIGNAL_MAX so they hold either NTSC (2048 samples,
     * 8 spp) or PAL (2560 samples, 10 spp) scanlines without realloc.
     * Aligned to 32 bytes so AVX2/NEON SIMD loads/stores can use
     * aligned instructions and the auto-vectorizer doesn't need to
     * generate scalar head/tail peels. */
    static _Alignas(32) float signal[COMP_SIGNAL_MAX];
    static _Alignas(32) float signal_prev[COMP_SIGNAL_MAX];
    static _Alignas(32) float signal_y[COMP_SIGNAL_MAX];
    static _Alignas(32) float signal_c[COMP_SIGNAL_MAX];
    static _Alignas(32) float Y_n[COMP_SIGNAL_MAX];
    static _Alignas(32) float I_n[COMP_SIGNAL_MAX];
    static _Alignas(32) float Q_n[COMP_SIGNAL_MAX];
    static _Alignas(32) float filtY[COMP_MAX_W];
    static _Alignas(32) float filtI[COMP_MAX_W];
    static _Alignas(32) float filtQ[COMP_MAX_W];

    /* Chroma phase parameters are all runtime-tunable (see CompositeSignal).
     * Defaults are first-order approximations; use the SDL frontend
     * hotkeys to dial them in against reference ROMs. The (+ 120) offset
     * before the modulo keeps negative advances well-defined. */
    int num_fields = n->signal.phase_num_fields;
    if (num_fields < 1) num_fields = 1;
    int field_ph = (int)(n->frame_count % (unsigned)num_fields)
                   * n->signal.phase_field_adv;
    int line_adv = n->signal.phase_line_adv;
    int base_ph  = n->signal.phase_base;
    int demod_rot = n->signal.demod_rotate;
    float chroma_gain = n->signal.chroma_gain;

    /* Region-dependent signal rate. Hoisted to locals so the inner
     * emission loop doesn't reach into the struct every iteration. */
    const int spp       = n->signal.samples_per_pixel;
    const int signal_w  = n->signal.signal_w;
    const int is_pal    = (n->signal.region == COMP_REGION_PAL);

    for (int sy = 0; sy < 240; sy++) {
        int phase0 = ((base_ph + field_ph + sy * line_adv) % 12 + 120) % 12;

        /* --- A. Emit raw composite signal for this scanline.
         * Per-pixel chroma phase advances by spp slots (8 NTSC, 10 PAL).
         * The signal table stores 24 slots per entry so an 8- or 10-
         * sample read at any phase 0..11 is in-bounds without modulo.
         *
         * PAL picks signal_table_alt on odd lines to model 2C07's
         * per-line V-phase inversion. At our demod_rotate = +3
         * (= 90°), the encoder V-flip is invisible to cos-demod
         * (cos(θ+90°) = cos((180°-θ)+90°) numerically — i.e., Ip
         * stays the same on both lines) but DOES flip sign on the
         * sin-demod output (Qp). The decoder compensates by flipping
         * Qp on alter lines (see demod section below).
         *
         * NTSC always uses signal_table (no encoder V-flip).
         *
         * Fast-path the NTSC 8 spp case with a compiler-friendly
         * unrolled 8-sample copy. */
        const uint16_t *row = &idx_fb[sy * 256];
        const float (*sig_tab)[COMP_TABLE_STRIDE] =
            (is_pal && (sy & 1))
                ? (const float (*)[COMP_TABLE_STRIDE])n->signal.signal_table_alt
                : (const float (*)[COMP_TABLE_STRIDE])n->signal.signal_table;
        int ph = phase0;
        if (spp == 8) {
            for (int px = 0; px < 256; px++) {
                uint16_t entry = row[px] & 0x1FF;  /* 9 bits: color + emph */
                const float *src = &sig_tab[entry][ph];
                float *dst = &signal[px * 8];
                dst[0] = src[0]; dst[1] = src[1];
                dst[2] = src[2]; dst[3] = src[3];
                dst[4] = src[4]; dst[5] = src[5];
                dst[6] = src[6]; dst[7] = src[7];
                ph += 8;
                if (ph >= COMP_TABLE_PHASES) ph -= COMP_TABLE_PHASES;
            }
        } else {
            /* Generic path: used for PAL (spp=10) and any future rates. */
            for (int px = 0; px < 256; px++) {
                uint16_t entry = row[px] & 0x1FF;
                const float *src = &sig_tab[entry][ph];
                float *dst = &signal[px * spp];
                for (int s = 0; s < spp; s++) dst[s] = src[s];
                ph += spp;
                while (ph >= COMP_TABLE_PHASES) ph -= COMP_TABLE_PHASES;
            }
        }

        /* --- A'. 2D ADAPTIVE comb filter: vertical 1-line delay
         * with edge-aware fallback.
         *
         * Classical 1-line comb exploits the 180° chroma phase flip
         * between scanlines (works when phase_line_adv = 6):
         *    signal_y = (curr + prev)/2   → luma (chroma cancels)
         *    signal_c = (curr - prev)/2   → chroma (luma cancels)
         *
         * Pure additive combing BLURS vertical luma edges — text
         * tops/bottoms get averaged with the empty scanline above/
         * below, producing ghost halves at half brightness.
         *
         * Adaptive fix: detect luma edges by comparing a CHROMA-FREE
         * version of curr/prev (via a 12-tap running box which
         * perfectly nulls the subcarrier at 8 samples/pixel), then
         * BLEND between comb result and raw signal based on edge
         * magnitude. Flat content → full comb; edges → raw signal.
         *
         * On line 0 or when disabled, skip combing and use raw signal. */
        static _Alignas(32) float lp_curr[COMP_SIGNAL_MAX];
        static _Alignas(32) float lp_prev[COMP_SIGNAL_MAX];
        if (n->signal.comb_filter && sy > 0) {
            /* 12-tap running boxcar → luma-only approximation.
             * 12 samples = 1 subcarrier period for either region (NTSC
             * 8 spp and PAL 10 spp both give 12 samples per cycle), so
             * the box's first null sits exactly on the subcarrier.
             * Split into two loops so the branch-free body vectorizes. */
            const int N_lp = 12;
            const float inv_n = 1.0f / (float)N_lp;
            float sum_c = 0.0f, sum_p = 0.0f;
            for (int k = 0; k < N_lp; k++) {
                sum_c += signal[k];
                sum_p += signal_prev[k];
            }
            const int body_end = signal_w - N_lp;
            for (int s = 0; s < body_end; s++) {
                lp_curr[s] = sum_c * inv_n;
                lp_prev[s] = sum_p * inv_n;
                sum_c += signal[s + N_lp]      - signal[s];
                sum_p += signal_prev[s + N_lp] - signal_prev[s];
            }
            /* Tail: reuse the last valid sum for the final N_lp samples. */
            float lp_c_tail = sum_c * inv_n;
            float lp_p_tail = sum_p * inv_n;
            for (int s = body_end; s < signal_w; s++) {
                lp_curr[s] = lp_c_tail;
                lp_prev[s] = lp_p_tail;
            }

            /* Blend per sample by luma-edge magnitude.
             * edge = clamp(|Y_curr - Y_prev| * gain, 0..1)
             * signal = comb * (1 - edge) + raw * edge
             * Branchless: fabsf + fminf so the compiler can SIMD. */
            const float edge_gain = 8.0f;
            const float * __restrict sig_in   = signal;
            const float * __restrict prev_in  = signal_prev;
            const float * __restrict lp_c_in  = lp_curr;
            const float * __restrict lp_p_in  = lp_prev;
            float * __restrict sy_out = signal_y;
            float * __restrict sc_out = signal_c;
            for (int s = 0; s < signal_w; s++) {
                float curr = sig_in[s];
                float prev = prev_in[s];
                float luma_diff = lp_c_in[s] - lp_p_in[s];
                float edge = fabsf(luma_diff) * edge_gain;
                if (edge > 1.0f) edge = 1.0f;

                float y_comb = (curr + prev) * 0.5f;
                float c_comb = (curr - prev) * 0.5f;

                sy_out[s] = y_comb + (curr - y_comb) * edge;
                sc_out[s] = c_comb + (curr - c_comb) * edge;
            }
        } else {
            memcpy(signal_y, signal, (size_t)signal_w * sizeof(float));
            memcpy(signal_c, signal, (size_t)signal_w * sizeof(float));
        }
        memcpy(signal_prev, signal, (size_t)signal_w * sizeof(float));

        /* --- B. Demodulate Y/I/Q at native 2048 rate.
         *
         * Two-step to help the compiler vectorize: first fill a
         * per-sample carrier table with the 12-slot cos/sin cycle
         * (small loop, branchy on a predictable pattern), then run
         * the main FP work as a branchless straight-line loop that
         * clang/gcc can SIMD.
         *
         * When comb filter is active, Y uses signal_y (chroma
         * cancelled) and I/Q use signal_c (luma cancelled). */
        static _Alignas(32) float demod_cos_stream[COMP_SIGNAL_MAX];
        static _Alignas(32) float demod_sin_stream[COMP_SIGNAL_MAX];
        {
            int demod_idx = ((phase0 + demod_rot) % 12 + 120) % 12;
            const float *cos_base = n->signal.demod_cos;
            const float *sin_base = n->signal.demod_sin;
            for (int s = 0; s < signal_w; s++) {
                demod_cos_stream[s] = cos_base[demod_idx];
                demod_sin_stream[s] = sin_base[demod_idx];
                if (++demod_idx == COMP_TABLE_PHASES) demod_idx = 0;
            }
        }
        {
            /* PAL V-flip compensation at the decoder.
             *
             * With encoder V-flip active (signal_table_alt on odd lines),
             * the fundamental shifts from θ to (180° - θ). At our
             * demod_rotate = +3 (= 90°):
             *   cos(θ+90°) vs cos((180°-θ)+90°) = cos(270°-θ) = sin(θ)
             *   Hmm wait that's not equal. Let me re-derive:
             *   cos(45° + 90°) = cos(135°) = -0.707  (even, θ=45°)
             *   cos(135° + 90°) = cos(225°) = -0.707 (odd, θ=135°)
             *   → SAME. Ip (cos-demod) is invariant to the V-flip at r=90°.
             *
             *   sin(45° + 90°) = sin(135°) = +0.707  (even)
             *   sin(135° + 90°) = sin(225°) = -0.707 (odd)
             *   → FLIPPED. Qp (sin-demod) flips sign on alter lines.
             *
             * So the decoder compensation at r=90° goes on Qp, not Ip.
             * After this, both lines produce consistent Ip and Qp, and
             * the downstream 1H V-averaging operates on stable V. */
            float u_sign = 1.0f;
            if (is_pal && (sy & 1))
                u_sign = -1.0f;

            const float * __restrict syin = signal_y;
            const float * __restrict sc   = signal_c;
            const float * __restrict dc   = demod_cos_stream;
            const float * __restrict ds   = demod_sin_stream;
            float * __restrict Yp = Y_n;
            float * __restrict Ip = I_n;
            float * __restrict Qp = Q_n;
            for (int s = 0; s < signal_w; s++) {
                float c = sc[s] * chroma_gain;
                Yp[s] = syin[s];
                Ip[s] = c * dc[s];
                Qp[s] = c * ds[s] * u_sign;
            }
        }

        /* --- C. Y FIR at native signal rate (kills subcarrier before
         * downsample). signal_w is 2048 (NTSC) or 2560 (PAL). */
        comp_fir_symmetric(Y_n, signal_w,
                           n->signal.fir_y, n->signal.fir_y_n);

        /* --- D. Downsample all three to output width. Y is now
         * bandlimited by the FIR; I/Q are broad but will be tightened
         * by their own FIR post-downsample (cheaper there). */
        comp_downsample(Y_n, signal_w, filtY, W);
        comp_downsample(I_n, signal_w, filtI, W);
        comp_downsample(Q_n, signal_w, filtQ, W);

        /* --- E. C FIR at output width. Tight bandwidth-limit for
         * authentic chroma softness. */
        comp_fir_symmetric(filtI, W, n->signal.fir_c, n->signal.fir_c_n);
        comp_fir_symmetric(filtQ, W, n->signal.fir_c, n->signal.fir_c_n);

        /* Chroma ringing: overshoot the I/Q channels at saturation
         * transitions. Parallel to luma ringing (applied below) but
         * on the chroma pair, producing the characteristic colored
         * halo on high-saturation edges that real composite decoders
         * had due to imperfect subcarrier isolation. */
        comp_chroma_ringing_row(filtI, W, n->chroma_ringing);
        comp_chroma_ringing_row(filtQ, W, n->chroma_ringing);

        /* --- E''. PAL 1H delay-line V averaging.
         *
         * The defining feature of PAL decoders: a 1-horizontal-line
         * delay averages V (= filtI in our convention) between the
         * current scanline and the previous one. Combined with the
         * encoder's per-line V-phase inversion (baked into
         * signal_table_alt), this:
         *
         *   - Cancels small hue errors: if the decoder's phase is off
         *     by ε, adjacent lines pick up +Δ and -Δ V-error. Averaging
         *     them cancels the error exactly — that's PAL's famous
         *     "phase-error robustness."
         *
         *   - Gives PAL its characteristic soft vertical chroma: fine
         *     color detail blurs vertically while Y stays sharp. This
         *     is THE most recognizable PAL look.
         *
         * Only V (Ip/filtI) is delayed; U (Qp/filtQ) passes through.
         * The state buffer holds the previous line's RAW (pre-averaged)
         * V so each line averages with its immediate neighbor, not
         * with a cascaded exponential decay. NTSC skips this entirely. */
        if (is_pal) {
            static _Alignas(32) float filtI_prev[COMP_MAX_W];
            if (sy > 0) {
                for (int s = 0; s < W; s++) {
                    float raw = filtI[s];
                    filtI[s] = 0.5f * (raw + filtI_prev[s]);
                    filtI_prev[s] = raw;
                }
            } else {
                memcpy(filtI_prev, filtI, (size_t)W * sizeof(float));
            }
        }

        /* --- E'. Color killer — NTSC TVs had a chroma-squelch
         * circuit that muted chroma when its amplitude fell below
         * a threshold. This forces whites/greys (which have tiny
         * residual cross-color) back to achromatic while leaving
         * real saturated colors untouched. Classic TV trick. */
        if (n->signal.color_killer > 0.0f) {
            float t  = n->signal.color_killer;
            float t2 = t * t;
            for (int s = 0; s < W; s++) {
                float i = filtI[s];
                float q = filtQ[s];
                if (i * i + q * q < t2) {
                    filtI[s] = 0.0f;
                    filtQ[s] = 0.0f;
                }
            }
        }

        /* --- F..H. Unchanged post-processing. */
        float ppn = (float)W / 256.0f;
        comp_ringing_row(filtY, W, n->luma_ringing);
        comp_afterglow_row(filtY, W, n->luma_afterglow, ppn);
        comp_bloom_row(filtY, W, n->bloom, ppn);
        comp_emit_output_row(n, sy, filtY, filtI, filtQ);
    }

    n->frame_count++;
}

/* PAL pipeline: vendored LMP88959/PAL-CRT end-to-end palette→output.
 *
 * Feeds the 256×240 index framebuffer straight into the 2C07 encoder
 * (per-line V-phase inversion + HardWareMan voltage levels) and runs
 * the PAL decoder (burst-extracted phase reference + 1H delay-line U/V
 * averaging with chroma_correction enabled) to fill n->output directly.
 * No intermediate buffer, no second pass — PAL-CRT IS the pipeline.
 *
 * The index framebuffer encoding matches what PAL-CRT's square_sample
 * expects:
 *   bits 0..3 = hue
 *   bits 4..5 = luma
 *   bits 6..8 = emphasis (R/G/B)
 * We captured these in ppu_render_pixel via
 *     index_framebuffer[x] = color_idx | ((mask & 0xE0) << 1)
 * which is the raw PPUMASK[5..7] layout — and because PAL hardware
 * uses mask[5] for GREEN (not RED), we set SWAP_RED_GREEN_EMPHASIS_BITS
 * = 1 in the vendored pal_nes.c so PAL-CRT's internal active[] table
 * swaps R↔G to match our layout. See src/nes/palcrt/README.md. */
static void comp_process_palcrt(Composite *n, const uint16_t *idx_fb) {
    if (!n->output || n->out_w == 0 || !n->palcrt_rgb) return;

    struct PAL_SETTINGS *s = &n->palcrt_settings;
    s->data    = idx_fb;
    s->w       = 256;
    s->h       = 240;
    /* PAL-CRT's internal hue knob; 0 = neutral. Our Composite.hue_deg
     * doesn't apply here — PAL-CRT runs its own chroma decode. */
    s->hue     = 0;
    s->xoffset = 0;
    s->yoffset = 0;
    /* ua6538 = 0 selects the RP2C07 voltage table (vs UA6538 clone).
     * The RP2C07 is the standard PAL NES PPU; UA6538 was a Famicom
     * Titler variant we don't care about. */
    s->ua6538  = 0;
    /* Leave s->field_initialized / s->altline[] as-is across frames —
     * that's PAL-CRT's persistent encoder state, reset only on resize. */

    pal_modulate(&n->palcrt, s);
    pal_demodulate(&n->palcrt, 0);   /* noise = 0 */

    /* PAL-CRT just wrote 240 RGB rows into palcrt_rgb (out_w × 240 × 3).
     * Expand each row into `rows_per_scanline` output rows with the
     * same CRT beam profile + vignette + HDR knee + aperture grille the
     * NTSC path uses, so PAL has matching scanline character. */
    const int stage_stride = n->out_w * 3;
    for (int sy = 0; sy < 240; sy++) {
        const uint8_t *src = n->palcrt_rgb + sy * stage_stride;
        comp_emit_output_row_rgb(n, sy, src);
    }

    n->frame_count++;
}

/* S-Video pipeline: skips the whole composite chain (no subcarrier, no
 * dot crawl, no chroma/luma bleed) and feeds the palette-LUT RGB from
 * the PPU straight through the scanline + CRT post-processing stack.
 *
 * Real S-Video keeps Y and C on separate wires, so no cross-leakage.
 * Since our palette LUT is essentially "the ideal Y + C combined at
 * infinite bandwidth", using it directly is indistinguishable from a
 * perfect S-Video decoder. What the user gets visually: pin-sharp NES
 * pixels with the same CRT character (scanline beam profile, vignette,
 * phosphor mask, HDR knee, chromatic convergence, gamma) as the
 * composite path. The output is noticeably "crisper" than composite,
 * which is exactly the S-Video → composite aesthetic tradeoff.
 *
 * source: 256×240 RGB from ppu.framebuffer (palette LUT output)
 * target: n->output via comp_emit_output_row_rgb */
static void comp_process_svideo(Composite *n, const uint8_t *fb_rgb) {
    if (!n->output || n->out_w == 0 || !fb_rgb) return;
    const int W = n->out_w;
    static _Alignas(32) uint8_t row[COMP_MAX_W * 3];
    for (int sy = 0; sy < 240; sy++) {
        const uint8_t *src = fb_rgb + sy * 256 * 3;
        /* Nearest-neighbor upscale 256 → W. Fast enough that a bilinear
         * upsample isn't worth it here — the scanline beam profile does
         * the vertical smoothing anyway, and horizontal sharpness is
         * specifically what the user asked for by picking S-Video. */
        for (int ox = 0; ox < W; ox++) {
            int sx = (ox * 256) / W;
            row[ox*3 + 0] = src[sx*3 + 0];
            row[ox*3 + 1] = src[sx*3 + 1];
            row[ox*3 + 2] = src[sx*3 + 2];
        }
        comp_emit_output_row_rgb(n, sy, row);
    }
    n->frame_count++;
}

/* ---- Full-frame post-processing ----
 *
 * These three effects operate on the finished output framebuffer (or a
 * copy of it) rather than during per-row emission. They all interact
 * with pixels from neighboring ROWS, which per-row functions can't see.
 *
 * Run order: persistence → 2D bloom → barrel.
 *   - Persistence first so bloom sees the "current frame + ghost of
 *     previous" composite — which is physically correct (the phosphor
 *     glow feeds into the bloom halo).
 *   - Bloom second so barrel warps an already-bloomed image (the halo
 *     follows the glass curvature, not the underlying pixels).
 *   - Barrel last so sampling reads a complete, effect-applied frame.
 */

/* Vertical phosphor persistence: max-blend the current frame with a
 * decayed snapshot of the previous frame. Real CRTs had ~1-frame
 * phosphor decay depending on the phosphor type (P22 was fast; some
 * data-display CRTs were slower). Max-blend (rather than linear mix)
 * matches real phosphor behavior: brighter pixel wins, so high-contrast
 * sprites leave a subtle trail without washing out dark backgrounds.
 *
 * NEON hot path processes 16 bytes at a time; scalar tail + fallback
 * handles the remainder and non-NEON targets. Widens uint8 → uint16
 * for the multiply-by-decay, shifts back to uint8, vmaxq vs current. */
static void comp_apply_persistence(Composite *n) {
    if (n->persistence < 0.01f) return;
    if (!n->output || !n->output_prev) return;
    const int total = n->out_w * (240 * n->rows_per_scanline) * 3;
    /* decay ∈ [0, 0.95] — strength 1.0 = near-hold, 0.5 ≈ 1 frame decay. */
    int decay_num = (int)(n->persistence * 0.95f * 256.0f);
    if (decay_num < 0) decay_num = 0;
    if (decay_num > 243) decay_num = 243;   /* cap to avoid infinite hold */
    uint8_t * __restrict cur  = n->output;
    uint8_t * __restrict prev = n->output_prev;
    int i = 0;
#if defined(__ARM_NEON)
    {
        uint16x8_t dec = vdupq_n_u16((uint16_t)decay_num);
        for (; i + 16 <= total; i += 16) {
            uint8x16_t c = vld1q_u8(&cur[i]);
            uint8x16_t p = vld1q_u8(&prev[i]);
            /* Widen prev to u16, multiply by decay, shift right 8. */
            uint16x8_t pl = vmulq_u16(vmovl_u8(vget_low_u8(p)),  dec);
            uint16x8_t ph = vmulq_u16(vmovl_u8(vget_high_u8(p)), dec);
            uint8x16_t p_scaled = vcombine_u8(vshrn_n_u16(pl, 8),
                                               vshrn_n_u16(ph, 8));
            uint8x16_t m = vmaxq_u8(c, p_scaled);
            vst1q_u8(&cur[i],  m);
            vst1q_u8(&prev[i], m);
        }
    }
#elif defined(__AVX2__)
    {
        __m256i dec = _mm256_set1_epi16((short)decay_num);
        for (; i + 32 <= total; i += 32) {
            __m256i c = _mm256_loadu_si256((const __m256i *)&cur[i]);
            __m256i p = _mm256_loadu_si256((const __m256i *)&prev[i]);
            /* Unpack to u16, multiply, shift, re-pack. */
            __m256i zero = _mm256_setzero_si256();
            __m256i pl = _mm256_unpacklo_epi8(p, zero);
            __m256i ph = _mm256_unpackhi_epi8(p, zero);
            pl = _mm256_srli_epi16(_mm256_mullo_epi16(pl, dec), 8);
            ph = _mm256_srli_epi16(_mm256_mullo_epi16(ph, dec), 8);
            __m256i p_scaled = _mm256_packus_epi16(pl, ph);
            __m256i m = _mm256_max_epu8(c, p_scaled);
            _mm256_storeu_si256((__m256i *)&cur[i],  m);
            _mm256_storeu_si256((__m256i *)&prev[i], m);
        }
    }
#endif
    /* Scalar tail / fallback. */
    for (; i < total; i++) {
        int c = cur[i];
        int p = (prev[i] * decay_num) >> 8;
        int m = c > p ? c : p;
        cur[i]  = (uint8_t)m;
        prev[i] = (uint8_t)m;
    }
}

/* 2D phosphor bloom: full-frame radial halo on bright regions. Operates
 * at quarter resolution (1/4 width × 1/4 height) via three-pass box blur
 * which is a good approximation of a gaussian for this purpose and keeps
 * the cost at a few hundred KB of scratch reads per frame. The bloomed
 * halo is upsampled with bilinear interpolation and added back to the
 * original with a soft-knee mix so it lifts highlights without clipping. */
static void comp_apply_bloom2d(Composite *n) {
    if (n->bloom_2d < 0.01f) return;
    if (!n->output) return;
    const int W  = n->out_w;
    const int H  = 240 * n->rows_per_scanline;
    const int sw = W / 4;
    const int sh = H / 4;
    if (sw < 8 || sh < 8) return;
    /* Scratch: two quarter-res float buffers (R+G+B interleaved) for
     * the separable blur passes. 293 × 240 × 3 × 4 ≈ 850 KB — comfortably
     * inside L2 on any modern core. */
    static _Alignas(32) float tmpA[(COMP_MAX_W/4) * (240*12/4) * 3];
    static _Alignas(32) float tmpB[(COMP_MAX_W/4) * (240*12/4) * 3];
    /* Extract bright pixels (above threshold) into tmpA at quarter res
     * via a 4×4 box average downsample. */
    const float threshold = 0.55f * 255.0f;
    for (int y = 0; y < sh; y++) {
        for (int x = 0; x < sw; x++) {
            int rr = 0, gg = 0, bb = 0;
            for (int dy = 0; dy < 4; dy++) {
                const uint8_t *src = n->output + (y*4 + dy) * W * 3 + x*4*3;
                for (int dx = 0; dx < 4; dx++) {
                    rr += src[dx*3 + 0];
                    gg += src[dx*3 + 1];
                    bb += src[dx*3 + 2];
                }
            }
            float fr = (float)(rr >> 4) - threshold;
            float fg = (float)(gg >> 4) - threshold;
            float fb = (float)(bb >> 4) - threshold;
            if (fr < 0.0f) fr = 0.0f;
            if (fg < 0.0f) fg = 0.0f;
            if (fb < 0.0f) fb = 0.0f;
            int idx = (y * sw + x) * 3;
            tmpA[idx + 0] = fr;
            tmpA[idx + 1] = fg;
            tmpA[idx + 2] = fb;
        }
    }
    /* Horizontal box blur: tmpA → tmpB. Radius scales with bloom_2d
     * amount but is clamped so the kernel fits in scratch. */
    int radius = (int)(n->bloom_2d * 12.0f + 2.0f);
    if (radius < 2)  radius = 2;
    if (radius > 30) radius = 30;
    float inv_diam = 1.0f / (float)(2 * radius + 1);
    for (int y = 0; y < sh; y++) {
        float sr = 0.0f, sg = 0.0f, sb = 0.0f;
        /* Prime: fill the window with the left edge replicated. */
        for (int i = -radius; i <= radius; i++) {
            int xi = i < 0 ? 0 : (i >= sw ? sw - 1 : i);
            int idx = (y * sw + xi) * 3;
            sr += tmpA[idx + 0];
            sg += tmpA[idx + 1];
            sb += tmpA[idx + 2];
        }
        for (int x = 0; x < sw; x++) {
            int out = (y * sw + x) * 3;
            tmpB[out + 0] = sr * inv_diam;
            tmpB[out + 1] = sg * inv_diam;
            tmpB[out + 2] = sb * inv_diam;
            int x_add = x + radius + 1; if (x_add >= sw) x_add = sw - 1;
            int x_sub = x - radius;     if (x_sub < 0)    x_sub = 0;
            int ia = (y * sw + x_add) * 3;
            int is = (y * sw + x_sub) * 3;
            sr += tmpA[ia + 0] - tmpA[is + 0];
            sg += tmpA[ia + 1] - tmpA[is + 1];
            sb += tmpA[ia + 2] - tmpA[is + 2];
        }
    }
    /* Vertical box blur: tmpB → tmpA. Same running-sum pattern. */
    for (int x = 0; x < sw; x++) {
        float sr = 0.0f, sg = 0.0f, sb = 0.0f;
        for (int i = -radius; i <= radius; i++) {
            int yi = i < 0 ? 0 : (i >= sh ? sh - 1 : i);
            int idx = (yi * sw + x) * 3;
            sr += tmpB[idx + 0];
            sg += tmpB[idx + 1];
            sb += tmpB[idx + 2];
        }
        for (int y = 0; y < sh; y++) {
            int out = (y * sw + x) * 3;
            tmpA[out + 0] = sr * inv_diam;
            tmpA[out + 1] = sg * inv_diam;
            tmpA[out + 2] = sb * inv_diam;
            int y_add = y + radius + 1; if (y_add >= sh) y_add = sh - 1;
            int y_sub = y - radius;     if (y_sub < 0)    y_sub = 0;
            int ia = (y_add * sw + x) * 3;
            int is = (y_sub * sw + x) * 3;
            sr += tmpB[ia + 0] - tmpB[is + 0];
            sg += tmpB[ia + 1] - tmpB[is + 1];
            sb += tmpB[ia + 2] - tmpB[is + 2];
        }
    }
    /* Second horizontal+vertical pass = smoother cubic-ish profile.
     * Cheap: same loops, tmpA → tmpB → tmpA. */
    for (int y = 0; y < sh; y++) {
        float sr = 0.0f, sg = 0.0f, sb = 0.0f;
        for (int i = -radius; i <= radius; i++) {
            int xi = i < 0 ? 0 : (i >= sw ? sw - 1 : i);
            int idx = (y * sw + xi) * 3;
            sr += tmpA[idx + 0];
            sg += tmpA[idx + 1];
            sb += tmpA[idx + 2];
        }
        for (int x = 0; x < sw; x++) {
            int out = (y * sw + x) * 3;
            tmpB[out + 0] = sr * inv_diam;
            tmpB[out + 1] = sg * inv_diam;
            tmpB[out + 2] = sb * inv_diam;
            int x_add = x + radius + 1; if (x_add >= sw) x_add = sw - 1;
            int x_sub = x - radius;     if (x_sub < 0)    x_sub = 0;
            int ia = (y * sw + x_add) * 3;
            int is = (y * sw + x_sub) * 3;
            sr += tmpA[ia + 0] - tmpA[is + 0];
            sg += tmpA[ia + 1] - tmpA[is + 1];
            sb += tmpA[ia + 2] - tmpA[is + 2];
        }
    }
    for (int x = 0; x < sw; x++) {
        float sr = 0.0f, sg = 0.0f, sb = 0.0f;
        for (int i = -radius; i <= radius; i++) {
            int yi = i < 0 ? 0 : (i >= sh ? sh - 1 : i);
            int idx = (yi * sw + x) * 3;
            sr += tmpB[idx + 0];
            sg += tmpB[idx + 1];
            sb += tmpB[idx + 2];
        }
        for (int y = 0; y < sh; y++) {
            int out = (y * sw + x) * 3;
            tmpA[out + 0] = sr * inv_diam;
            tmpA[out + 1] = sg * inv_diam;
            tmpA[out + 2] = sb * inv_diam;
            int y_add = y + radius + 1; if (y_add >= sh) y_add = sh - 1;
            int y_sub = y - radius;     if (y_sub < 0)    y_sub = 0;
            int ia = (y_add * sw + x) * 3;
            int is = (y_sub * sw + x) * 3;
            sr += tmpB[ia + 0] - tmpB[is + 0];
            sg += tmpB[ia + 1] - tmpB[is + 1];
            sb += tmpB[ia + 2] - tmpB[is + 2];
        }
    }
    /* Upsample tmpA (quarter res) back to full and add with soft knee
     * to n->output. Bilinear interpolation — 4 taps per output pixel. */
    const float intensity = n->bloom_2d * 1.5f;
    for (int y = 0; y < H; y++) {
        float fy = (float)y * 0.25f - 0.5f;
        int   y0 = (int)floorf(fy);
        float wy = fy - (float)y0;
        if (y0 < 0)       { y0 = 0; wy = 0.0f; }
        if (y0 >= sh - 1) { y0 = sh - 2; wy = 1.0f; }
        uint8_t *dst = n->output + y * W * 3;
        for (int x = 0; x < W; x++) {
            float fx = (float)x * 0.25f - 0.5f;
            int   x0 = (int)floorf(fx);
            float wx = fx - (float)x0;
            if (x0 < 0)       { x0 = 0; wx = 0.0f; }
            if (x0 >= sw - 1) { x0 = sw - 2; wx = 1.0f; }
            int i00 = (y0 * sw + x0) * 3;
            int i10 = (y0 * sw + x0 + 1) * 3;
            int i01 = ((y0 + 1) * sw + x0) * 3;
            int i11 = ((y0 + 1) * sw + x0 + 1) * 3;
            for (int c = 0; c < 3; c++) {
                float v0 = tmpA[i00 + c] * (1.0f - wx) + tmpA[i10 + c] * wx;
                float v1 = tmpA[i01 + c] * (1.0f - wx) + tmpA[i11 + c] * wx;
                float glow = (v0 * (1.0f - wy) + v1 * wy) * intensity;
                int base = dst[x*3 + c];
                float headroom = 255.0f - (float)base;
                if (headroom < 0.0f) headroom = 0.0f;
                int out = base + (int)(glow * headroom * (1.0f / 255.0f));
                if (out < 0) out = 0; if (out > 255) out = 255;
                dst[x*3 + c] = (uint8_t)out;
            }
        }
    }
}

/* Barrel distortion: CRT glass curvature with bilinear sampling.
 *
 * Reads from output_tmp (a snapshot of n->output taken before the
 * warp) and writes warped pixels back into n->output. Inverse sampling:
 * for each output (u, v) (normalized to [-1, 1]), read the source at
 * (u * s, v * s) where s = 1 + k*r², r² = u²+v².
 *   - Center (r²=0): s=1, sample in place.
 *   - Corners (r²=2): s=1+2k > 1, sample from OUTSIDE → black bezel.
 *
 * Bilinear sampling (vs nearest-neighbor) is important here because
 * the source image contains high-frequency content (scanline gaps,
 * the emit pass-2 beam profile) and a radial warp at non-integer
 * sampling rates aliases those high frequencies into visible
 * patterns. Bilinear smooths over the sub-pixel difference. The
 * phosphor mask + chromatic convergence + gamma are also deferred
 * to run AFTER barrel (see comp_post_process) so those high-
 * frequency patterns don't interact with the warp at all. */
static void comp_apply_barrel(Composite *n) {
    if (n->barrel < 0.01f) return;
    if (!n->output || !n->output_tmp) return;
    const int W = n->out_w;
    const int H = 240 * n->rows_per_scanline;
    memcpy(n->output_tmp, n->output, (size_t)(W * H * 3));
    const float k = n->barrel * 0.25f;
    const float inv_hw = 2.0f / (float)W;
    const float inv_hh = 2.0f / (float)H;
    const float half_W = (float)W * 0.5f;
    const float half_H = (float)H * 0.5f;
    const uint8_t * __restrict src_base = n->output_tmp;
    const int stride = W * 3;
    for (int y = 0; y < H; y++) {
        float v = (float)y * inv_hh - 1.0f;
        float v2 = v * v;
        uint8_t *dst = n->output + y * stride;
        for (int x = 0; x < W; x++) {
            float u = (float)x * inv_hw - 1.0f;
            float s = 1.0f + k * (u * u + v2);
            float sx = (u * s + 1.0f) * half_W;
            float sy = (v * s + 1.0f) * half_H;
            int ix = (int)sx;
            int iy = (int)sy;
            if (ix < 0 || ix >= W - 1 || iy < 0 || iy >= H - 1) {
                dst[x*3]   = 0;
                dst[x*3+1] = 0;
                dst[x*3+2] = 0;
                continue;
            }
            /* Fixed-point bilinear: 8-bit fractional weights, all
             * integer multiply-shift. Avoids 12 float→int conversions
             * per pixel that the float path had. Total weights sum to
             * 65536 so the final >>16 recovers the correct scale. */
            int fx8 = (int)((sx - (float)ix) * 256.0f);
            int fy8 = (int)((sy - (float)iy) * 256.0f);
            int ifx = 256 - fx8;
            int ify = 256 - fy8;
            int w00 = ifx * ify;   /* (256-fx)(256-fy) */
            int w10 = fx8 * ify;
            int w01 = ifx * fy8;
            int w11 = fx8 * fy8;
            const uint8_t *p00 = src_base + iy * stride + ix * 3;
            const uint8_t *p10 = p00 + 3;
            const uint8_t *p01 = p00 + stride;
            const uint8_t *p11 = p01 + 3;
            dst[x*3]   = (uint8_t)((p00[0]*w00 + p10[0]*w10 + p01[0]*w01 + p11[0]*w11) >> 16);
            dst[x*3+1] = (uint8_t)((p00[1]*w00 + p10[1]*w10 + p01[1]*w01 + p11[1]*w11) >> 16);
            dst[x*3+2] = (uint8_t)((p00[2]*w00 + p10[2]*w10 + p01[2]*w01 + p11[2]*w11) >> 16);
        }
    }
}

/* H-sync wobble: per-scanline horizontal jitter from composite sync
 * instability. Uses a low-frequency noise source (xorshift seeded per
 * frame) smoothed across adjacent rows for the characteristic slowly-
 * wandering top-edge wobble of analog TV. Applied by writing shifted
 * rows from output_tmp into output. The shift is in output pixels
 * (not NES pixels) so the magnitude scales with resolution.
 *
 * Cheap: one xorshift per row + one memcpy per row. ~0.5 ms at 4×. */
static void comp_apply_hsync_wobble(Composite *n) {
    if (n->hsync_wobble < 0.01f) return;
    if (!n->output || !n->output_tmp) return;
    const int W = n->out_w;
    const int H = 240 * n->rows_per_scanline;
    const int stride = W * 3;
    memcpy(n->output_tmp, n->output, (size_t)(W * H * 3));
    /* xorshift32 seeded from frame count for temporal variation.
     * Smoothed by a 1st-order IIR (exponential average across rows)
     * so adjacent scanlines don't jump wildly — real H-sync drift is
     * a smooth wander, not per-line white noise. */
    uint32_t rng = 214013u * (uint32_t)(n->frame_count + 1) + 2531011u;
    float max_shift = n->hsync_wobble * 3.0f;  /* max px amplitude */
    float smooth = 0.0f;
    for (int y = 0; y < H; y++) {
        rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5;
        float noise = ((float)(rng & 0xFFFF) / 32768.0f - 1.0f) * max_shift;
        smooth = smooth * 0.92f + noise * 0.08f;  /* IIR low-pass */
        int shift = (int)(smooth + (smooth > 0 ? 0.5f : -0.5f));
        if (shift == 0) {
            memcpy(n->output + y * stride, n->output_tmp + y * stride, (size_t)stride);
            continue;
        }
        uint8_t *dst = n->output + y * stride;
        const uint8_t *src = n->output_tmp + y * stride;
        if (shift > 0) {
            /* Shift right: black on left edge. */
            if (shift * 3 < stride) {
                memset(dst, 0, (size_t)(shift * 3));
                memcpy(dst + shift * 3, src, (size_t)(stride - shift * 3));
            } else {
                memset(dst, 0, (size_t)stride);
            }
        } else {
            /* Shift left: black on right edge. */
            int s = -shift;
            if (s * 3 < stride) {
                memcpy(dst, src + s * 3, (size_t)(stride - s * 3));
                memset(dst + stride - s * 3, 0, (size_t)(s * 3));
            } else {
                memset(dst, 0, (size_t)stride);
            }
        }
    }
}

/* Signal ghosting (multipath reflection): an attenuated, horizontally
 * delayed copy of each row added on top of itself. Classic antenna RF
 * artifact where the signal bounces off buildings/terrain and arrives
 * at the TV with a slight delay. The "ghost" is dimmer and shifted
 * rightward. ghosting ∈ [0, 1] = ghost intensity, ghost_offset =
 * horizontal delay in output pixels (typically 5-12). */
static void comp_apply_ghosting(Composite *n) {
    if (n->ghosting < 0.01f) return;
    if (!n->output || !n->output_tmp) return;
    const int W = n->out_w;
    const int H = 240 * n->rows_per_scanline;
    const int stride = W * 3;
    int offset = n->ghost_offset;
    if (offset < 1) offset = 1;
    if (offset >= W) return;
    int ghost_num = (int)(n->ghosting * 64.0f);  /* 0..64 of 256 */
    if (ghost_num < 1) return;
    if (ghost_num > 64) ghost_num = 64;
    memcpy(n->output_tmp, n->output, (size_t)(W * H * 3));
    for (int y = 0; y < H; y++) {
        uint8_t *dst = n->output + y * stride;
        const uint8_t *src = n->output_tmp + y * stride;
        for (int x = offset; x < W; x++) {
            for (int c = 0; c < 3; c++) {
                int base = dst[x*3 + c];
                int ghost = src[(x - offset)*3 + c];
                int v = base + ((ghost * ghost_num) >> 8);
                if (v > 255) v = 255;
                dst[x*3 + c] = (uint8_t)v;
            }
        }
    }
}

/* RF reception noise (snow): per-pixel luma-only grain injected by a
 * fast xorshift32 RNG. Luma-only (R = G = B offset) matches real RF
 * noise which is mostly baseband Y interference. Grain amplitude
 * scales with the `snow` knob. Cheap: ~0.5 ms at 4× horizontal. */
static void comp_apply_snow(Composite *n) {
    if (n->snow < 0.01f) return;
    if (!n->output) return;
    const int W = n->out_w;
    const int H = 240 * n->rows_per_scanline;
    uint32_t rng = 1103515245u * (uint32_t)(n->frame_count + 1) + 12345u;
    int amp = (int)(n->snow * 40.0f);   /* max ± noise magnitude */
    if (amp < 1) return;
    for (int y = 0; y < H; y++) {
        uint8_t *row = n->output + y * W * 3;
        for (int x = 0; x < W; x++) {
            rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5;
            int noise = (int)(rng & 0xFF) - 128;  /* -128..+127 */
            noise = (noise * amp) >> 7;            /* scale by amplitude */
            for (int c = 0; c < 3; c++) {
                int v = row[x*3 + c] + noise;
                if (v < 0) v = 0; if (v > 255) v = 255;
                row[x*3 + c] = (uint8_t)v;
            }
        }
    }
}

/* 60 Hz AC hum bar: slowly rolling horizontal brightness modulation.
 * Real TVs with bad grounding picked up 60 Hz mains hum which
 * produced a dim band (brighter or darker than surrounding rows)
 * slowly rolling up or down the screen. The roll rate is the
 * difference between the CRT's vertical refresh (60.0988 Hz) and
 * mains (60 Hz), which is ~0.1 Hz — one full roll per ~10 seconds.
 * We use frame_count to advance the phase slowly. */
static void comp_apply_hum_bar(Composite *n) {
    if (n->hum_bar < 0.01f) return;
    if (!n->output) return;
    const int W = n->out_w;
    const int H = 240 * n->rows_per_scanline;
    /* Roll phase: advance ~0.6° per frame → full cycle ~600 frames
     * ≈ 10 seconds at 60 fps. */
    float phase = (float)(n->frame_count % 600) / 600.0f * 2.0f * (float)M_PI;
    float amp = n->hum_bar * 25.0f;  /* max brightness offset */
    for (int y = 0; y < H; y++) {
        float row_phase = phase + (float)y / (float)H * 2.0f * (float)M_PI;
        float mod = sinf(row_phase) * amp;
        int mod_i = (int)mod;
        if (mod_i == 0) continue;
        uint8_t *row = n->output + y * W * 3;
        for (int x = 0; x < W * 3; x++) {
            int v = row[x] + mod_i;
            if (v < 0) v = 0; if (v > 255) v = 255;
            row[x] = (uint8_t)v;
        }
    }
}

/* Combined display-effects pass: phosphor mask + chromatic convergence
 * + gamma LUT + black floor, all in ONE sweep of n->output.
 *
 * Before the merge this was 4 separate full-frame passes (mask, conv,
 * gamma, floor), each reading + writing ~3.4 MB. Now it's 1 pass with
 * 1 optional memcpy (for convergence source). That's 4× less memory
 * traffic and much better cache utilization — the biggest single perf
 * win for presets that enable barrel + post-effects.
 *
 * Convergence needs shifted reads (R from x-shift, B from x+shift),
 * which would feed back if we read+write the same buffer. So we
 * snapshot to output_tmp first, then read from tmp and write to output.
 * When convergence is off, we read and write output in-place (no copy).
 *
 * The phosphor mask uses 2-output-pixel cells (6 per triad) to avoid
 * beat-frequency Moire under SDL upscale. See the old standalone
 * comp_apply_phosphor_mask comment for the rationale. */
static void comp_apply_display_effects(Composite *n) {
    if (!n->output) return;
    const int W = n->out_w;
    const int H = 240 * n->rows_per_scanline;
    const int stride = W * 3;

    const int has_mask  = (n->aperture_grille >= 0.01f);
    const int has_conv  = (n->chromatic_conv >= 0.01f);
    const int has_gamma = (n->gamma != 1.0f);
    const int has_floor = (n->black_floor > 0);
    if (!has_mask && !has_conv && !has_gamma && !has_floor) return;

    /* Convergence shift (integer output pixels). */
    int cc_shift = 0;
    if (has_conv) {
        cc_shift = (int)(n->chromatic_conv * 3.0f + 0.5f);
        if (cc_shift < 1) cc_shift = 0;
        if (cc_shift > 3) cc_shift = 3;
    }

    /* Snapshot for convergence reads (shifted R/B need an untouched
     * source). Skip the memcpy when convergence is off — saves ~1 ms. */
    const uint8_t * __restrict src;
    if (cc_shift > 0 && n->output_tmp) {
        memcpy(n->output_tmp, n->output, (size_t)(W * H * 3));
        src = n->output_tmp;
    } else {
        src = n->output;
    }

    /* Phosphor mask table (loop-invariant). */
    uint16_t mask[3][3];
    if (has_mask) {
        float str = n->aperture_grille;
        if (str > 1.0f) str = 1.0f;
        int unlit = (int)((1.0f - str) * 256.0f);
        if (unlit < 0) unlit = 0;
        if (unlit > 256) unlit = 256;
        for (int ph = 0; ph < 3; ph++) {
            mask[ph][0] = (ph == 0) ? 256 : (uint16_t)unlit;
            mask[ph][1] = (ph == 1) ? 256 : (uint16_t)unlit;
            mask[ph][2] = (ph == 2) ? 256 : (uint16_t)unlit;
        }
    }

    const uint8_t *glut = n->gamma_lut;
    const int floor_val = (has_floor && n->black_floor <= 60) ? n->black_floor : 0;
    const int cell_w = 2;   /* 2 output px per phosphor cell */

    for (int y = 0; y < H; y++) {
        const uint8_t *srow = src + y * stride;
        uint8_t       *drow = n->output + y * stride;
        for (int x = 0; x < W; x++) {
            int r, g, b;
            /* Read with convergence shift (or straight if off). */
            if (cc_shift > 0) {
                int xr = x - cc_shift; if (xr < 0)  xr = 0;
                int xb = x + cc_shift; if (xb >= W) xb = W - 1;
                r = srow[xr*3 + 0];
                g = srow[x*3  + 1];
                b = srow[xb*3 + 2];
            } else {
                r = drow[x*3 + 0];
                g = drow[x*3 + 1];
                b = drow[x*3 + 2];
            }
            /* Phosphor mask. */
            if (has_mask) {
                int ph = (x / cell_w) % 3;
                r = (r * mask[ph][0]) >> 8;
                g = (g * mask[ph][1]) >> 8;
                b = (b * mask[ph][2]) >> 8;
            }
            /* Gamma LUT. */
            if (has_gamma) {
                r = glut[r];
                g = glut[g];
                b = glut[b];
            }
            /* Black floor. */
            if (floor_val > 0) {
                if (r < floor_val) r = floor_val;
                if (g < floor_val) g = floor_val;
                if (b < floor_val) b = floor_val;
            }
            drow[x*3 + 0] = (uint8_t)r;
            drow[x*3 + 1] = (uint8_t)g;
            drow[x*3 + 2] = (uint8_t)b;
        }
    }
}

/* Run order:
 *   persistence → bloom2d → barrel → hsync_wobble → ghosting → snow →
 *   hum_bar → phosphor_mask → convergence → gamma → black_floor
 *
 *   - Persistence first: temporal blend so bloom/barrel see the right composite.
 *   - Bloom2d second: radial halo feeds into barrel.
 *   - Barrel third: geometric warp of the lit-and-bloomed image.
 *   - H-sync wobble AFTER barrel: the per-row shift operates in output
 *     space on the warped image, which is physically correct (the CRT
 *     deflects the beam row by row after the glass curvature).
 *   - Ghosting AFTER barrel: multipath delay is in signal space, and
 *     we're in output space — but applying the ghost on the final
 *     image is visually indistinguishable from signal-space ghost.
 *   - Snow after barrel: same reasoning.
 *   - Hum bar after barrel: brightness modulation in display space.
 *   - Phosphor mask AFTER barrel: axis-aligned stripe in output space
 *     avoids the circular Moire from radial warp of a grid.
 *   - Convergence AFTER barrel: R/B shifts follow glass curvature.
 *   - Gamma second-to-last: tonemap on the final composited image.
 *   - Black floor LAST: raises the absolute minimum after all other
 *     processing. If it ran before gamma, gamma would crush it. */
static inline void comp_post_process(Composite *n) {
    comp_apply_persistence(n);
    if (!(n->skip_display_effects & COMP_SKIP_BLOOM2D))
        comp_apply_bloom2d(n);
    if (!(n->skip_display_effects & COMP_SKIP_BARREL))
        comp_apply_barrel(n);
    comp_apply_hsync_wobble(n);
    comp_apply_ghosting(n);
    comp_apply_snow(n);
    comp_apply_hum_bar(n);
    if (!(n->skip_display_effects & COMP_SKIP_DISPLAY_FX))
        comp_apply_display_effects(n);
}

/* Thin wrapper. Dispatches based on region + svideo flag:
 *   NTSC composite → comp_process_waveform (Bisqwit 2C02 square-wave)
 *   NTSC S-Video   → comp_process_svideo   (palette LUT, no composite)
 *   PAL (any mode) → comp_process_palcrt   (vendored PAL-CRT 2C07 chain)
 *
 * After the main path fills n->output, runs the full-frame post pass
 * (vertical persistence → 2D bloom → barrel distortion).
 *
 * PAL + svideo currently falls through to PAL-CRT's composite chain —
 * the PAL-CRT pipeline doesn't expose a "clean S-Video" exit point.
 * NTSC S-Video is the meaningful toggle.
 *
 * Callers pass the 256×240 index framebuffer (bits 0..5 = palette
 * index, bits 6..8 = PPUMASK emphasis) AND the palette-LUT RGB frame
 * (used by the S-Video path only). */
static void comp_process(Composite *n,
                         const uint8_t *framebuffer_rgb,
                         const uint16_t *framebuffer_idx) {
    if (!framebuffer_idx) return;
    if (n->signal.region == COMP_REGION_PAL) {
        comp_process_palcrt(n, framebuffer_idx);
    } else if (n->signal.svideo && framebuffer_rgb) {
        comp_process_svideo(n, framebuffer_rgb);
    } else {
        comp_process_waveform(n, framebuffer_idx);
    }
    comp_post_process(n);
}

#endif /* NES_COMPOSITE_H */
