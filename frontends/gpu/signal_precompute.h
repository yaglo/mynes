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
     * Blank-relative voltage units (sub-black may be negative, white=1).
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
    float chroma_gain;          /* post-demod I/Q multiplier (default 1) */
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

/* Terminated 2C02 measurements, NESdev NTSC_video (lidnariq).
 * Keep picture, emphasis, sync and burst in the same voltage reference.
 * Blanking is 312 mV; NES white is 1100 mV (not broadcast 100 IRE).
 * Emphasis changes the measured DAC rails, not a uniform gain multiplier. */
static inline void signal_precompute_ntsc(SignalPrecompute *sp) {
    static const float levels[16] = {
        228, 312, 552, 880, 616, 840, 1100, 1100,
        192, 256, 448, 712, 500, 676, 896, 896
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
                int octant = (p % 12) >> 1;
                int mask = (emph_oct >> (3 * octant)) & 0x07;
                int attenuated = color < 14 && (emph & mask) ? 8 : 0;
                float sig = levels[level + (in_hi ? 4 : 0) + attenuated];
                float norm_sig = (sig - blacklo) * norm;
                sp->table[entry][p] = norm_sig;
                sp->table[entry][p + 12] = norm_sig;  /* wraparound duplicate */
            }
        }
    }
}

/* Chroma-band extraction before line combing. Gaussian frequency response,
 * unity at the carrier and zero DC: vertical luma detail must not enter the
 * line averager. half_bandwidth is the approximate -3 dB half-width in Hz. */
static inline void signal_design_chroma_bandpass(float *taps, int n, float sample_rate,
                                                 float half_bandwidth) {
    float window[64], sum=0, dc=0, gain=0;
    float sigma=sqrtf(logf(2.0f))*sample_rate/(2.0f*(float)M_PI*half_bandwidth);
    for(int k=0;k<n;k++) {
        float x=(float)(k-n/2);
        window[k]=expf(-0.5f*x*x/(sigma*sigma));
        taps[k]=window[k]*cosf(2.0f*(float)M_PI*x/12.0f);
        dc+=taps[k]; sum+=window[k];
    }
    for(int k=0;k<n;k++) {
        taps[k]-=dc*window[k]/sum;
        gain+=taps[k]*cosf(2.0f*(float)M_PI*(k-n/2)/12.0f);
    }
    for(int k=0;k<n;k++) taps[k]/=gain;
}

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

/* Place -3 dB at the requested frequency instead of the windowed-sinc
 * cutoff (approximately -6 dB). This fits one published bandwidth point;
 * the rest of the response remains a linear-phase FIR approximation.
 * Intended for the RGB stage's 2..10 MHz range at NES sampling rates. */
static inline void signal_design_fir_3db(float *taps, int n, float frequency) {
    float lo = 0.0001f, hi = 0.499f;
    for (int iteration = 0; iteration < 24; iteration++) {
        float cutoff = (lo + hi) * 0.5f;
        signal_design_fir(taps, n, cutoff);
        double response = 0;
        for (int k = 0; k < n; k++)
            response += taps[k] * cos(2 * M_PI * frequency * (k - n/2));
        if (response < 0.7079457843841379) lo = cutoff; // 10^(-3/20)
        else hi = cutoff;
    }
    signal_design_fir(taps, n, (lo + hi) * 0.5f);
}

/* Generic receiver IF for a local AM console modulator. The combined
 * sidebands have unit small-signal video gain; their difference produces
 * quadrature envelope distortion. A nominal 0.75 MHz Nyquist transition
 * is a System-M reference, not a fitted NES RF-module transfer function.
 * taps are interleaved real/imaginary, matching vec2 in rf_if.comp. */
#define SIGNAL_RF_IF_TAPS 97
static inline void signal_design_rf_if(float *taps, float fs, float bandwidth,
                                       float asymmetry, float detune) {
    const int n=SIGNAL_RF_IF_TAPS,bins=1024;
    double sum=0;
    bandwidth=fminf(fs*.45f,fmaxf(1e6f,bandwidth));
    asymmetry=fminf(1,fmaxf(0,asymmetry));
    detune=fminf(1e6f,fmaxf(-1e6f,detune));
    for(int k=0;k<n;k++) {
        double re=0,im=0;
        for(int b=-bins/2;b<bins/2;b++) {
            double f=(double)b*fs/bins,shifted=f+detune;
            double edge=fmin(1,fmax(0,(fabs(shifted)-(bandwidth-.3e6))/.6e6));
            double lowpass=.5+.5*cos(M_PI*edge);
            double slope=sin(M_PI*.5*fmin(1,fmax(-1,shifted/.75e6)));
            double h=lowpass*(1+asymmetry*slope);
            double phase=-2*M_PI*b*(k-n/2)/bins;
            re+=h*cos(phase); im+=h*sin(phase);
        }
        double window=.54-.46*cos(2*M_PI*k/(n-1));
        taps[2*k]=(float)(re*window/bins);
        taps[2*k+1]=(float)(im*window/bins);
        sum+=taps[2*k];
    }
    // Normalize carrier response so tuning changes sidebands, not DC levels.
    for(int k=0;k<2*n;k++) taps[k]/=(float)sum;
}

#define SIGNAL_VHS_TAPS 129
static inline void signal_design_vhs(float *taps, float fs, float luma_bw,
                                     float chroma_bw, float delay_samples) {
    float y[SIGNAL_VHS_TAPS],c[SIGNAL_VHS_TAPS];
    signal_design_fir(y,SIGNAL_VHS_TAPS,fminf(3e6f,fmaxf(.5e6f,luma_bw))/fs);
    signal_design_fir(c,SIGNAL_VHS_TAPS,fminf(.6e6f,fmaxf(.1e6f,chroma_bw))/fs);
    double dc_re=0,dc_im=0;
    for(int k=0;k<SIGNAL_VHS_TAPS;k++) {
        double phase=2*M_PI*3579545.454545/fs*(k-SIGNAL_VHS_TAPS/2-delay_samples);
        taps[4*k]=y[k];
        taps[4*k+1]=2*c[k]*(float)cos(phase);
        taps[4*k+2]=2*c[k]*(float)sin(phase);
        taps[4*k+3]=0;
        dc_re+=taps[4*k+1]; dc_im+=taps[4*k+2];
    }
    // Exact grey neutrality even with finite filter support and envelope delay.
    for(int k=0;k<SIGNAL_VHS_TAPS;k++) {
        taps[4*k+1]-=(float)dc_re*c[k];
        taps[4*k+2]-=(float)dc_im*c[k];
    }
}

/* Add a generic highpass shelf to a FIR. For receiver sharpness, start
 * with an identity FIR and apply the result AFTER luma extraction.
 * Adding this directly to a separation FIR bypasses its stopband/trap.
 * amount: 0.0 = no peaking, 0.5 = moderate, 1.0 = strong edge enhancement.
 * The highpass transition is at cutoff * 0.7. This is not a measured
 * aperture-correction circuit or a bandpass with adjustable centre/Q. */
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

/* Set the peak gain of a symmetric aperture FIR, retaining unity DC.
 * This constrains gain only: it does NOT recover a Sony IC's frequency
 * response, phase, coring or menu law. The dB-linear knob is an assumption.
 * Used at coefficient update time, never in the per-frame signal loop. */
static inline void signal_normalize_aperture_gain(float *taps, int n, float db) {
    int half = n / 2;
    double sum = 0;
    for (int k = 0; k < n; k++) sum += taps[k];
    taps[half] += (float)(1.0 - sum);
    double peak = 1.0;
    for (int bin = 1; bin <= 2048; bin++) {
        double response = 0;
        for (int k = 0; k < n; k++)
            response += taps[k] * cos(M_PI * bin * (k-half) / 2048.0);
        if (response > peak) peak = response;
    }
    double scale = peak > 1.000001 ? (pow(10.0, db / 20.0) - 1.0) / (peak - 1.0) : 0;
    for (int k = 0; k < n; k++) {
        float delta = k == half ? 1.0f : 0.0f;
        taps[k] = delta + (float)((taps[k] - delta) * scale);
    }
}

/* Design a lowpass FIR with a notch (null) at a specific frequency.
 * Used for composite luma: preserves useful luma bandwidth while killing
 * the 3.58 MHz subcarrier horizontally — no vertical line doubling.
 * notch_freq: normalized frequency to reject (e.g., 1/12 for NTSC subcarrier).
 * notch_depth: rejection strength (0.5-1.0, higher = deeper null). */
static inline void signal_design_fir_notch(float *taps, int n, float cutoff,
                                            float notch_freq, float notch_depth) {
    signal_design_fir(taps, n, cutoff);
    if (notch_depth <= 0.0f) return;
    notch_depth = fminf(notch_depth, 1.0f);
    /* A symmetric, zero-DC correction with unit response at f0.
     * Subtract a fraction of the LOWPASS residual at f0, not a fixed
     * unity response: narrow receiver bandwidths may already reject it.
     * Removing the window-weighted mean preserves DC without a final
     * renormalization that would change the requested notch depth. */
    int half = n / 2;
    float window[64], notch[64];
    float sum_window = 0.0f, sum_cos = 0.0f, response = 0.0f;
    for (int k = 0; k < n; k++) {
        float c = cosf(2.0f * (float)M_PI * notch_freq * (k - half));
        window[k] = 0.54f - 0.46f * cosf(2.0f * (float)M_PI * k / (n - 1));
        notch[k] = c;
        sum_window += window[k];
        sum_cos += window[k] * c;
        response += taps[k] * c;
    }
    float gain = 0.0f, mean = sum_cos / sum_window;
    for (int k = 0; k < n; k++) {
        float c = notch[k];
        notch[k] = window[k] * (c - mean);
        gain += notch[k] * c;
    }
    if (fabsf(gain) < 1e-8f) return;
    float scale = notch_depth * response / gain;
    for (int k = 0; k < n; k++) taps[k] -= scale * notch[k];
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

    /* Neutral receiver defaults, identical to preset application. */
    static const float ntsc[3][3] = {{1,.9563f,.6210f},{1,-.2721f,-.6474f},{1,-1.1070f,1.7046f}};
    static const float pal[3][3] = {{1,1.140f,0},{1,-.581f,-.395f},{1,0,2.032f}};
    memcpy(sp->color_matrix, region == SIGNAL_REGION_PAL ? pal : ntsc, sizeof(sp->color_matrix));
    memset(sp->color_bias, 0, sizeof(sp->color_bias));
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
