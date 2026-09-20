/*
 * Signal Precompute — Standalone Bisqwit 2C02/2C07 Signal Table + FIR Taps
 * ==========================================================================
 *
 * Precomputes the data the GPU signal chain needs, WITHOUT depending on
 * composite.h or the Composite struct. Pure math, no pipeline state.
 *
 * Extracted from composite.h's comp_precompute_signal_table_ntsc and
 * comp_design_fir so the GPU frontend has zero dependency on the CPU
 * composite pipeline.
 */

#ifndef SIGNAL_PRECOMPUTE_H
#define SIGNAL_PRECOMPUTE_H

#include <math.h>
#include <string.h>
#include "signal_format.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* Signal table dimensions (must match dac_2c02.comp.glsl expectations). */
#define SIG_TABLE_ENTRIES  512   /* 64 colors × 8 emphasis states */
#define SIG_TABLE_STRIDE   24   /* 12 phases × 2 (duplicated for wraparound) */

/* Complete precomputed signal data for the GPU chain. */
typedef struct {
    /* PPU voltage waveform table. Indexed by:
     *   table[(emph << 6) | palette_idx][phase0..phase0+spp-1]
     * Values normalized to [0,1] (black=0, white=1).
     *
     * - NTSC (2C02): only `table` is used; `table_alt` stays zero.
     * - PAL  (2C07): `table` is used on even scanlines, `table_alt`
     *   on odd ones, reproducing the 2C07's per-line V-phase
     *   inversion that makes PAL color phase self-correcting. */
    float table[SIG_TABLE_ENTRIES][SIG_TABLE_STRIDE];
    float table_alt[SIG_TABLE_ENTRIES][SIG_TABLE_STRIDE];

    /* FIR filter taps (Hamming-windowed sinc, unit DC gain). */
    float fir_y[64];    /* luma lowpass */
    float fir_c[64];    /* I-channel chroma lowpass (wider; NTSC-spec: 1.3 MHz) */
    float fir_q[64];    /* Q-channel chroma lowpass (narrower; NTSC-spec: 0.5 MHz) */
    int   fir_y_n;      /* luma tap count (odd) */
    int   fir_c_n;      /* I tap count (odd) */
    int   fir_q_n;      /* Q tap count (odd) */

    /* Phase parameters for dot crawl. */
    int   phase_base;
    int   frame_phase_override; /* -1: synthetic frame timing; otherwise clock-derived */
    int   phase_line_adv;
    int   phase_field_adv;
    int   phase_num_fields;
    int   demod_rotate;
    float chroma_gain;          /* post-demod I/Q multiplier (default 1.3) */
    /* color_killer moved to TVDisplayParams — shader reads it directly. */
    float brightness;           /* Y offset (default 0.0) */
    float contrast;             /* Y multiplier (default 1.0) */
    int   samples_per_pixel;
    int   samples_per_line;
    int   region;              /* SIGNAL_REGION_NTSC / SIGNAL_REGION_PAL */

    /* YIQ → RGB decode matrix (3×3 row-major) + bias. */
    float color_matrix[3][3];
    float color_bias[3];
} SignalPrecompute;

/* Precompute the PAL 2C07 signal table from measured voltage levels.
 * Two tables are generated: `table` for even scanlines and `table_alt`
 * for odd scanlines. The 2C07 inverts the chroma V-phase line-to-line,
 * which the decoder averages out so small phase errors don't manifest
 * as hue drift. Without the alt table, odd lines would decode with the
 * wrong hue — visible as horizontal color bands. */
static inline void signal_precompute_pal(SignalPrecompute *sp) {
    /* 2C07 voltage levels, measured on real silicon. Indexed as
     *   [(high << 3) | (emph << 2) | luma]
     * and expressed in mV above the black reference; white = 1467 mV,
     * black = 0 mV. */
    static const float mV[16] = {
        /* low, no emph:    L0       L1       L2       L3   */
                           -175.0f,    0.0f,  475.0f, 1067.0f,
        /* low, emph:       L0       L1       L2       L3   */
                           -266.0f, -133.0f,  225.0f,  700.0f,
        /* high, no emph:   L0       L1       L2       L3   */
                            609.0f, 1000.0f, 1467.0f, 1467.0f,
        /* high, emph:      L0       L1       L2       L3   */
                            334.0f,  642.0f, 1000.0f, 1000.0f,
    };
    const float norm = 1.0f / 1467.0f;

    /* Per-phase emphasis mask: which emphasis bits darken which octant
     * of the color wheel. This is a property of the color wheel itself
     * and identical to NTSC, just re-expressed to match PPUMASK's bit
     * order for 2C07 (bit5=G, bit6=R, bit7=B) — the emph_swap below
     * normalises that to the same R/G/B ordering signal_precompute_ntsc
     * uses, so both paths agree on "emph bit 0 = red attenuator".
     * 6 octants per subcarrier cycle × 12 phases/cycle → octant = p/2. */
    static const int active[6] = { 0x3, 0x1, 0x5, 0x4, 0x6, 0x2 };

    for (int alter = 0; alter < 2; alter++) {
        float (*dst)[SIG_TABLE_STRIDE] = alter ? sp->table_alt : sp->table;

        for (int pal_idx = 0; pal_idx < 64; pal_idx++) {
            int color = pal_idx & 0x0F;
            int luma  = (pal_idx >> 4) & 0x03;

            /* $0E / $0F: flat zero — 2C07 forces these to the
             * blanking reference. $0D is sub-black, handled via the
             * luma=0 low entry below (same as $1D would be). */
            int forced_zero = (color == 0x0E || color == 0x0F);

            /* Alt table V-flips hues 1..12 (skip $0 flat-white and $D
             * flat-black, which have no chroma to invert). The remap
             * formula 0x0D - ((color + 2) & 0x0F) mirrors the hue
             * index through the point diametrically opposite the
             * V-axis on the color wheel — the 2C07's encoder behavior. */
            int hue_for_square = color;
            if (alter && color >= 1 && color <= 12) {
                hue_for_square = 0x0D - ((color + 2) & 0x0F);
            }

            for (int emph_ntsc = 0; emph_ntsc < 8; emph_ntsc++) {
                int entry = (emph_ntsc << 6) | pal_idx;

                /* PAL PPUMASK swaps the R/G bit meanings vs NTSC; our
                 * emph field carries the raw PPUMASK bit layout, so
                 * convert to the R/G/B-ordered mask `active[]` wants. */
                int emph = (emph_ntsc & 0x4) |
                           ((emph_ntsc & 0x2) >> 1) |
                           ((emph_ntsc & 0x1) << 1);

                for (int p = 0; p < 12; p++) {
                    float sig;
                    if (forced_zero) {
                        sig = 0.0f;
                    } else {
                        int high = ((hue_for_square + p) % 12 + 12) % 12;
                        high = (high < 6) ? 1 : 0;
                        /* Flat colors override the square wave. */
                        if (color == 0x00)      high = 1;
                        else if (color == 0x0D) high = 0;

                        int octant = (p >> 1) % 6;
                        int e = (emph & active[octant]) ? 1 : 0;

                        int ire_idx = (high << 3) | (e << 2) | luma;
                        sig = mV[ire_idx] * norm;
                    }
                    dst[entry][p] = sig;
                    dst[entry][p + 12] = sig;  /* wraparound duplicate */
                }
            }
        }
    }
}

/* Precompute the NTSC 2C02 signal table from Bisqwit's voltage model. */
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
                sp->table[entry][p + 12] = norm_sig;  /* wraparound duplicate */
            }
        }
    }
}

/* Design a Hamming-windowed sinc lowpass FIR. */
/* Design a Hamming-windowed sinc FIR lowpass filter.
 * ringing: 0.0 = pure Hamming (smooth), 1.0 = rectangular (max Gibbs ringing).
 * Intermediate values blend between the two, producing controlled overshoot
 * on sharp transitions — mimicking cheap NTSC decoder ICs. */
static inline void signal_design_fir_ex(float *taps, int n, float cutoff,
                                         float ringing) {
    int half = n / 2;
    float sum = 0.0f;
    for (int k = 0; k < n; k++) {
        int m = k - half;
        float sinc = (m == 0)
            ? 2.0f * cutoff
            : sinf(2.0f * (float)M_PI * cutoff * (float)m)
              / ((float)M_PI * (float)m);
        float hamming = 0.54f - 0.46f * cosf(2.0f * (float)M_PI * (float)k / (float)(n - 1));
        float w = hamming * (1.0f - ringing) + 1.0f * ringing;
        taps[k] = sinc * w;
        sum += taps[k];
    }
    float inv = 1.0f / sum;
    for (int k = 0; k < n; k++) taps[k] *= inv;
}

static inline void signal_design_fir(float *taps, int n, float cutoff) {
    signal_design_fir_ex(taps, n, cutoff, 0.0f);
}

/* Apply luma peaking (TV "sharpness" control) to an existing FIR.
 * Boosts high frequencies near the cutoff by adding a scaled
 * derivative (highpass) component. Creates edge enhancement.
 * amount: 0.0 = no peaking, 0.5 = moderate, 1.0 = strong edge enhancement.
 * The boost frequency is centered at cutoff * 0.7 (below the cutoff). */
static inline void signal_apply_peaking(float *taps, int n, float cutoff,
                                         float amount) {
    if (amount < 0.001f) return;
    /* Design a highpass at the peaking frequency. */
    float peak_freq = cutoff * 0.7f;
    int half = n / 2;
    float hp[64];
    float hpsum = 0.0f;
    for (int k = 0; k < n && k < 64; k++) {
        int m = k - half;
        float sinc = (m == 0)
            ? 2.0f * peak_freq
            : sinf(2.0f * (float)M_PI * peak_freq * (float)m)
              / ((float)M_PI * (float)m);
        float w = 0.54f - 0.46f * cosf(2.0f * (float)M_PI * (float)k / (float)(n - 1));
        hp[k] = sinc * w;
        hpsum += hp[k];
    }
    /* Convert lowpass to highpass: delta - lowpass. */
    for (int k = 0; k < n; k++) {
        float delta = (k == half) ? 1.0f : 0.0f;
        hp[k] = delta - hp[k] / (hpsum + 1e-6f);
    }
    /* Add scaled highpass to the existing taps (boost edges). */
    for (int k = 0; k < n; k++) taps[k] += amount * hp[k];
    /* Renormalize DC gain. */
    float sum = 0.0f;
    for (int k = 0; k < n; k++) sum += taps[k];
    float inv = 1.0f / (sum + 1e-6f);
    for (int k = 0; k < n; k++) taps[k] *= inv;
}

/* Design a lowpass FIR with a notch (null) at a specific frequency.
 * Used for S-Video luma: preserves full luma bandwidth while killing
 * the 3.58 MHz subcarrier horizontally — no vertical line doubling.
 * notch_freq: normalized frequency to reject (e.g., 1/12 for NTSC subcarrier).
 * notch_depth: rejection strength (0.5-1.0, higher = deeper null). */
static inline void signal_design_fir_notch(float *taps, int n, float cutoff,
                                            float notch_freq, float notch_depth) {
    /* Start with standard lowpass. */
    signal_design_fir(taps, n, cutoff);
    /* Subtract a windowed cosine at the notch frequency.
     * A windowed cosine at f0 has DTFT magnitude ≈ 0.5 * sum(window) at f0.
     * To subtract `notch_depth` gain at f0, scale the cosine by
     * notch_depth / (0.5 * sum(window)). */
    int half = n / 2;
    float notch[64];
    float w_sum = 0.0f;
    for (int k = 0; k < n && k < 64; k++) {
        int m = k - half;
        float hamming = 0.54f - 0.46f * cosf(2.0f * (float)M_PI * (float)k / (float)(n - 1));
        notch[k] = cosf(2.0f * (float)M_PI * notch_freq * (float)m) * hamming;
        w_sum += hamming;
    }
    /* Scale so the subtraction reduces gain at notch_freq by exactly notch_depth. */
    float nscale = 2.0f * notch_depth / (w_sum + 1e-6f);
    for (int k = 0; k < n; k++) taps[k] -= notch[k] * nscale;
    /* Renormalize DC gain to 1.0. */
    float sum = 0.0f;
    for (int k = 0; k < n; k++) sum += taps[k];
    float inv = 1.0f / (sum + 1e-6f);
    for (int k = 0; k < n; k++) taps[k] *= inv;
}

/* Initialize with NTSC defaults (Consumer TV). */
static inline void signal_precompute_init(SignalPrecompute *sp, int region) {
    memset(sp, 0, sizeof(*sp));

    sp->region = region;
    if (region == SIGNAL_REGION_PAL) {
        sp->samples_per_pixel = SIGNAL_PAL_SAMPLES_PER_PIXEL;
        sp->samples_per_line = SIGNAL_PAL_SAMPLES_PER_LINE;
        signal_precompute_pal(sp);
    } else {
        sp->samples_per_pixel = SIGNAL_NTSC_SAMPLES_PER_PIXEL;
        sp->samples_per_line = SIGNAL_NTSC_SAMPLES_PER_LINE;
        signal_precompute_ntsc(sp);
    }

    /* FIR defaults (Consumer TV tuning).
     * Q tap count matches C by default — many consumer sets used
     * equi-band I/Q. Presets that model a real NTSC decoder IC should
     * override chroma_q_bandwidth ≈ 0.5 MHz (Q cutoff ≈ 0.012). */
    sp->fir_y_n = 37;
    sp->fir_c_n = 47;
    sp->fir_q_n = 47;
    signal_design_fir(sp->fir_y, sp->fir_y_n, 0.043f);
    signal_design_fir(sp->fir_c, sp->fir_c_n, 0.015f);
    signal_design_fir(sp->fir_q, sp->fir_q_n, 0.015f);

    /* 341 dots include horizontal blanking: NTSC advances 4/12 of a
     * carrier cycle per line, PAL 2/12. NTSC synthetic frames alternate
     * 89342 and 89341 dots; live emulation supplies its actual PPU clock. */
    sp->phase_base = 0;
    sp->frame_phase_override = -1;
    sp->phase_line_adv = signal_region_line_phase(region);
    sp->phase_field_adv = region == SIGNAL_REGION_PAL ? 0 : 4;
    sp->phase_num_fields = region == SIGNAL_REGION_PAL ? 1 : 2;
    sp->demod_rotate = (region == SIGNAL_REGION_PAL) ? 3 : 4;
    sp->chroma_gain = 1.00f;
    /* sp->color_killer removed — now in TVDisplayParams */
    sp->brightness = 0.0f;
    sp->contrast = 1.0f;

    /* Standard NTSC YIQ → RGB decode matrix with warm tint. */
    const float warm_r = 1.03f, warm_g = 1.01f, warm_b = 0.97f;
    const float sat = 1.15f;
    sp->color_matrix[0][0] = warm_r;
    sp->color_matrix[0][1] = 1.1222f * sat;
    sp->color_matrix[0][2] = 0.7391f * sat;
    sp->color_matrix[1][0] = warm_g;
    sp->color_matrix[1][1] = -0.3192f * sat;
    sp->color_matrix[1][2] = -0.7384f * sat;
    sp->color_matrix[2][0] = warm_b;
    sp->color_matrix[2][1] = -1.2374f * sat;
    sp->color_matrix[2][2] = 1.9058f * sat;
    sp->color_bias[0] = 0.015f;
    sp->color_bias[1] = 0.015f;
    sp->color_bias[2] = 0.018f;
}

static inline int signal_frame_phase(const SignalPrecompute *sp, unsigned frame) {
    if (sp->frame_phase_override >= 0) return sp->frame_phase_override % 12;
    unsigned period = sp->phase_num_fields > 0 ? (unsigned)sp->phase_num_fields : 1u;
    unsigned field = frame % period;
    int phase = (int)field * sp->phase_field_adv;
    if (sp->region == SIGNAL_REGION_NTSC) phase -= (int)(field / 2u) * 8;
    return (phase % 12 + 12) % 12;
}

#endif /* SIGNAL_PRECOMPUTE_H */
