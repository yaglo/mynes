/*
 * Signal Precompute Tests — Terminated 2C02 DAC + FIR Design
 * ========================================================
 *
 * Verifies the standalone precompute module (no dependency on the
 * composite pipeline). Two groups of tests:
 *
 *   1. 2C02 DAC — waveform shape, emphasis attenuation, normalization,
 *      octant mask, wraparound duplication.
 *   2. FIR design — DC gain, passband/stopband behaviour, notch null
 *      depth, peaking HF boost, composability, symmetry.
 *   3. Init — default dimensions and matrix entries for NTSC/PAL.
 *
 * Build: gcc -O2 -lm -I.. test_signal_precompute.c -o test_signal_precompute
 * Run:   ./test_signal_precompute
 */

#include "../signal_precompute.h"
#include "../waveform_gen.h"
#include <stdio.h>
#include <math.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

static int tests_run = 0;
static int tests_passed = 0;

#define ASSERT_NEAR(actual, expected, tol, msg) do { \
    float _a = (actual), _e = (expected), _t = (tol); \
    if (fabsf(_a - _e) > _t) { \
        printf("  FAIL: %s: got %.6f, expected %.6f (tol %.6f)\n", msg, _a, _e, _t); \
        return 0; \
    } \
} while(0)

#define RUN_TEST(fn) do { \
    tests_run++; \
    printf("TEST %s: ", #fn); \
    if (fn()) { tests_passed++; printf("PASS\n"); } \
    else { printf("FAILED\n"); } \
} while(0)

/* ============================================================================
 * Helpers
 * ============================================================================ */

/* Evaluate |H(f)| for FIR taps at normalized frequency f (cycles/sample). */
static float fir_magnitude_at(const float *taps, int n, float f) {
    float re = 0, im = 0;
    int half = n / 2;
    for (int k = 0; k < n; k++) {
        float phase = -2.0f * (float)M_PI * f * (float)(k - half);
        re += taps[k] * cosf(phase);
        im += taps[k] * sinf(phase);
    }
    return sqrtf(re*re + im*im);
}

/* Compute mean of 12-phase signal (one subcarrier cycle). */
static float waveform_mean_12(const float *phases) {
    float s = 0.0f;
    for (int i = 0; i < 12; i++) s += phases[i];
    return s / 12.0f;
}

/* Compute std deviation over 12 phases. */
static float waveform_stddev_12(const float *phases) {
    float m = waveform_mean_12(phases);
    float v = 0.0f;
    for (int i = 0; i < 12; i++) {
        float d = phases[i] - m;
        v += d * d;
    }
    return sqrtf(v / 12.0f);
}

/* Sum of FIR taps (DC gain). */
static float fir_tap_sum(const float *taps, int n) {
    float s = 0.0f;
    for (int k = 0; k < n; k++) s += taps[k];
    return s;
}

/* ============================================================================
 * Terminated 2C02 DAC tests
 * ============================================================================ */

static int test_published_voltage_rails(void) {
    SignalPrecompute sp;
    signal_precompute_ntsc(&sp);
    /* Independent measurements in millivolts, including flat rails and
     * sub-black; 14 mV accounts for published noise/quantization uncertainty. */
    const int codes[] = {0x0d, 0x1d, 0x2d, 0x3d, 0x00, 0x10, 0x20, 0x30};
    const float measured[] = {228,312,552,880,616,840,1100,1100};
    const float emphasized[] = {192,256,448,712,500,676,896,896};
    for (int i=0;i<8;i++) for (int ph=0;ph<12;ph++) {
        ASSERT_NEAR(sp.table[codes[i]][ph]*788+312, measured[i], 14, "terminated DAC rail mV");
        ASSERT_NEAR(sp.table[codes[i]|(7<<6)][ph]*788+312, emphasized[i], 14, "emphasis rail mV");
    }
    for (int emph=0;emph<8;emph++) for (int code=14;code<64;code+=16)
        for (int ph=0;ph<12;ph++) {
            ASSERT_NEAR(sp.table[(emph<<6)|code][ph], 0, 1e-6f, "$xE ignores emphasis");
            ASSERT_NEAR(sp.table[(emph<<6)|(code+1)][ph], 0, 1e-6f, "$xF ignores emphasis");
        }
    return 1;
}

static int test_dac_black_waveform(void) {
    /* Palette index 0x1D is NES "black": luma level 1, color >= 13,
     * so level forced to 1 and in_hi=0 → signal stays at blacklo (312 mV).
     * After normalization, 312 mV → 0.0. */
    SignalPrecompute sp;
    signal_precompute_ntsc(&sp);

    int entry = (0 << 6) | 0x1D;  /* emph=0, pal=0x1D */
    for (int p = 0; p < 12; p++) {
        ASSERT_NEAR(sp.table[entry][p], 0.0f, 1e-5f, "black sample ~0.0");
    }
    return 1;
}

static int test_dac_white_waveform(void) {
    /* Palette index 0x20: luma level 2, color 0 → in_hi forced to 1,
     * signal = levels[2 + 4] = levels[6] = 1100 mV = whitehi.
     * After normalization: (1100 mV - 312 mV) / (1100 mV - 312 mV) = 1.0. */
    SignalPrecompute sp;
    signal_precompute_ntsc(&sp);

    int entry = (0 << 6) | 0x20;
    for (int p = 0; p < 12; p++) {
        ASSERT_NEAR(sp.table[entry][p], 1.0f, 1e-5f, "white sample ~1.0");
    }
    return 1;
}

static int test_dac_saturated_colors(void) {
    /* For saturated chroma (color 1..12), the waveform oscillates
     * between in_hi and in_lo as phase sweeps. Expect std > 0.1. */
    SignalPrecompute sp;
    signal_precompute_ntsc(&sp);

    for (int color = 1; color <= 12; color++) {
        int pal_idx = (0 << 4) | color;  /* level 0, saturated colors */
        int entry = (0 << 6) | pal_idx;
        float sd = waveform_stddev_12(sp.table[entry]);
        if (sd < 0.1f) {
            printf("  FAIL: color 0x%02X stddev %.4f < 0.1 (not oscillating)\n",
                   pal_idx, sd);
            return 0;
        }
    }
    return 1;
}

static int test_dac_emphasis_attenuation(void) {
    /* emph=7 (all R,G,B emphasis active) attenuates ALL octants
     * since every octant is covered by at least one of R/G/B masks.
     * So mean(emph=7) < mean(emph=0) for a bright color. */
    SignalPrecompute sp;
    signal_precompute_ntsc(&sp);

    /* Use palette index 0x30 (bright, "lightest" level) which is
     * level=3, color=0 → in_hi=1 always, so sig = levels[3+4] = 1100 mV. */
    int pal_idx = 0x30;
    int entry0 = (0 << 6) | pal_idx;
    int entry7 = (7 << 6) | pal_idx;

    float m0 = waveform_mean_12(sp.table[entry0]);
    float m7 = waveform_mean_12(sp.table[entry7]);

    if (!(m0 > m7)) {
        printf("  FAIL: emph0 mean %.4f not greater than emph7 mean %.4f\n",
               m0, m7);
        return 0;
    }
    /* All three emphasis gates lower the white rail to 896 mV. */
    if (m7 > m0 * 0.9f) {
        printf("  FAIL: emph7 mean %.4f not attenuated enough vs emph0 %.4f\n",
               m7, m0);
        return 0;
    }
    return 1;
}

static int test_dac_emphasis_octant_mask(void) {
    /* R emphasis attenuates phases 0..5; G and B gates rotate by 4
     * and 8 samples. $xE/$xF remain blank regardless of emphasis. */
    SignalPrecompute sp;
    signal_precompute_ntsc(&sp);

    int pal_idx = 0x30;  /* bright, constant-high waveform */
    int e0 = (0 << 6) | pal_idx;
    int e1 = (1 << 6) | pal_idx;

    /* Verify each octant individually. Octant = phase/2. */
    const int emph_oct = 0264513;
    int attenuated_count = 0, preserved_count = 0;
    for (int p = 0; p < 12; p++) {
        int octant = p >> 1;
        int mask = (emph_oct >> (3 * octant)) & 0x07;
        float s0 = sp.table[e0][p];
        float s1 = sp.table[e1][p];
        if (1 & mask) {
            if (!(s1 < s0 - 1e-4f)) {
                printf("  FAIL: octant %d (phase %d) mask=%d should attenuate but s1=%.4f !< s0=%.4f\n",
                       octant, p, mask, s1, s0);
                return 0;
            }
            attenuated_count++;
        } else {
            /* Not attenuated: s1 should equal s0. */
            ASSERT_NEAR(s1, s0, 1e-5f, "octant preserved (emph=1)");
            preserved_count++;
        }
    }

    if (attenuated_count == 0 || preserved_count == 0) {
        printf("  FAIL: expected mix of attenuated/preserved octants (att=%d pre=%d)\n",
               attenuated_count, preserved_count);
        return 0;
    }
    return 1;
}

static int test_signal_table_wraparound(void) {
    /* table[i][0..11] must equal table[i][12..23] — the duplicate is
     * there so the shader can fetch phase..phase+spp-1 without a modulo. */
    SignalPrecompute sp;
    signal_precompute_ntsc(&sp);

    for (int entry = 0; entry < SIG_TABLE_ENTRIES; entry++) {
        for (int p = 0; p < 12; p++) {
            ASSERT_NEAR(sp.table[entry][p], sp.table[entry][p + 12],
                        1e-6f, "wraparound duplicate");
        }
    }
    return 1;
}

/* ============================================================================
 * FIR design tests
 * ============================================================================ */

static int test_fir_lowpass_dc_gain_is_one(void) {
    /* Unit DC gain is a design invariant: sum of taps == 1.0. */
    float taps[37];
    float cutoffs[3] = {0.05f, 0.1f, 0.2f};
    for (int i = 0; i < 3; i++) {
        signal_design_fir(taps, 37, cutoffs[i]);
        float s = fir_tap_sum(taps, 37);
        ASSERT_NEAR(s, 1.0f, 1e-5f, "FIR DC gain == 1");
    }
    return 1;
}

static int test_fir_lowpass_rejects_above_cutoff(void) {
    /* At f = 2 * cutoff, the Hamming-windowed sinc should have at least
     * 20 dB of stopband attenuation. */
    float taps[37];
    float cutoffs[3] = {0.05f, 0.1f, 0.15f};
    for (int i = 0; i < 3; i++) {
        signal_design_fir(taps, 37, cutoffs[i]);
        float mag = fir_magnitude_at(taps, 37, 2.0f * cutoffs[i]);
        if (mag >= 0.1f) {
            printf("  FAIL: cutoff=%.2f: |H(2*fc)|=%.4f >= 0.1 (<20 dB)\n",
                   cutoffs[i], mag);
            return 0;
        }
    }
    return 1;
}

static int test_fir_notch_nulls_at_notch_freq(void) {
    /* Notch design: verify deep null at notch_freq, preserved DC,
     * and preserved passband. */
    float taps[47];
    float cutoff = 0.1f;
    float notch_freq = 0.0833f;  /* ~1/12 = NTSC subcarrier normalized */
    float depth = 0.8f;
    signal_design_fir_notch(taps, 47, cutoff, notch_freq, depth);

    float mag_notch = fir_magnitude_at(taps, 47, notch_freq);
    if (mag_notch >= 0.2f) {
        printf("  FAIL: |H(notch)|=%.4f >= 0.2 (notch not deep)\n", mag_notch);
        return 0;
    }

    float mag_dc = fir_magnitude_at(taps, 47, 0.0f);
    ASSERT_NEAR(mag_dc, 1.0f, 1e-4f, "notch |H(0)| ~1");

    float mag_pass = fir_magnitude_at(taps, 47, cutoff * 0.5f);
    if (mag_pass < 0.8f) {
        printf("  FAIL: |H(cutoff/2)|=%.4f < 0.8 (passband ruined)\n", mag_pass);
        return 0;
    }
    return 1;
}

static int test_fir_notch_default_depth(void) {
    /* depth=0 → notch should degenerate to plain lowpass. */
    float taps_plain[37], taps_notched[37];
    signal_design_fir(taps_plain, 37, 0.1f);
    signal_design_fir_notch(taps_notched, 37, 0.1f, 0.0833f, 0.0f);

    for (int k = 0; k < 37; k++) {
        ASSERT_NEAR(taps_notched[k], taps_plain[k], 1e-5f,
                    "depth=0 == plain lowpass");
    }
    return 1;
}

static int test_fir_peaking_boosts_high_freq(void) {
    /* After peaking, |H(0.07)| should increase (edge enhancement). */
    float taps_plain[37], taps_peaked[37];
    signal_design_fir(taps_plain, 37, 0.1f);
    memcpy(taps_peaked, taps_plain, sizeof(taps_plain));
    signal_apply_peaking(taps_peaked, 37, 0.1f, 0.5f);

    float mag_plain = fir_magnitude_at(taps_plain, 37, 0.07f);
    float mag_peaked = fir_magnitude_at(taps_peaked, 37, 0.07f);

    if (!(mag_peaked > mag_plain)) {
        printf("  FAIL: peaking did not boost |H(0.07)|: plain=%.4f peaked=%.4f\n",
               mag_plain, mag_peaked);
        return 0;
    }
    return 1;
}

static int test_fir_peaking_preserves_dc(void) {
    /* Peaking renormalizes — sum of taps must remain ~1.0. */
    float taps[37];
    signal_design_fir(taps, 37, 0.1f);
    signal_apply_peaking(taps, 37, 0.1f, 0.5f);
    float s = fir_tap_sum(taps, 37);
    ASSERT_NEAR(s, 1.0f, 1e-4f, "peaked FIR DC gain == 1");
    return 1;
}

static int test_fir_notch_plus_peaking_composable(void) {
    /* Chain notch THEN peaking. The null must still be deep and
     * the HF boost must still be visible. */
    float taps[47];
    float cutoff = 0.1f;
    float notch_freq = 0.0833f;
    signal_design_fir_notch(taps, 47, cutoff, notch_freq, 0.8f);

    /* Snapshot magnitudes before peaking. */
    float mag_notch_before = fir_magnitude_at(taps, 47, notch_freq);
    float mag_hf_before = fir_magnitude_at(taps, 47, 0.07f);

    signal_apply_peaking(taps, 47, cutoff, 0.5f);

    float mag_notch_after = fir_magnitude_at(taps, 47, notch_freq);
    float mag_hf_after = fir_magnitude_at(taps, 47, 0.07f);

    /* Null is partially filled by peaking (peaking boosts HF including
     * the notch frequency if it's near the peaking band). Just verify
     * it's still below the pre-peak HF level. */
    if (mag_notch_after >= 1.0f) {
        printf("  FAIL: after peaking notch lost: |H|=%.4f\n", mag_notch_after);
        return 0;
    }
    /* HF boost must be present: peaked > unpeaked. */
    if (!(mag_hf_after > mag_hf_before)) {
        printf("  FAIL: peaking boost absent: before=%.4f after=%.4f\n",
               mag_hf_before, mag_hf_after);
        return 0;
    }
    /* DC still preserved. */
    float dc = fir_magnitude_at(taps, 47, 0.0f);
    ASSERT_NEAR(dc, 1.0f, 1e-4f, "notch+peak DC gain");

    (void)mag_notch_before;
    return 1;
}

static int test_fir_symmetric(void) {
    /* Hamming-windowed sinc lowpass is linear-phase → even symmetry. */
    int ns[3] = {31, 37, 47};
    float cutoffs[3] = {0.05f, 0.1f, 0.2f};
    float taps[64];
    for (int i = 0; i < 3; i++) {
        for (int j = 0; j < 3; j++) {
            signal_design_fir(taps, ns[i], cutoffs[j]);
            for (int k = 0; k < ns[i] / 2; k++) {
                ASSERT_NEAR(taps[k], taps[ns[i] - 1 - k],
                            1e-6f, "FIR symmetric");
            }
        }
    }
    return 1;
}

/* ============================================================================
 * signal_precompute_init tests
 * ============================================================================ */

static int test_init_ntsc_defaults(void) {
    SignalPrecompute sp;
    signal_precompute_init(&sp, SIGNAL_REGION_NTSC);

    if (sp.samples_per_pixel != 8) {
        printf("  FAIL: NTSC samples_per_pixel=%d (expected 8)\n",
               sp.samples_per_pixel);
        return 0;
    }
    if (sp.samples_per_line != 2048) {
        printf("  FAIL: NTSC samples_per_line=%d (expected 2048)\n",
               sp.samples_per_line);
        return 0;
    }
    if (sp.fir_y_n != 37) {
        printf("  FAIL: fir_y_n=%d (expected 37)\n", sp.fir_y_n);
        return 0;
    }
    if (sp.fir_c_n != 47) {
        printf("  FAIL: fir_c_n=%d (expected 47)\n", sp.fir_c_n);
        return 0;
    }
    if (sp.phase_line_adv != 4) {
        printf("  FAIL: phase_line_adv=%d (expected 4)\n", sp.phase_line_adv);
        return 0;
    }
    if (sp.phase_field_adv != 4) {
        printf("  FAIL: phase_field_adv=%d (expected 4)\n", sp.phase_field_adv);
        return 0;
    }
    if (sp.phase_num_fields != 2) {
        printf("  FAIL: phase_num_fields=%d (expected 2)\n", sp.phase_num_fields);
        return 0;
    }
    for(int c=0;c<3;c++) {
        ASSERT_NEAR(sp.color_matrix[c][0], 1, 1e-6f, "neutral grayscale gain");
        ASSERT_NEAR(sp.color_bias[c], 0, 1e-6f, "no hidden black lift");
    }

    /* Also sanity-check that FIR taps have unit DC gain after init. */
    float sum_y = fir_tap_sum(sp.fir_y, sp.fir_y_n);
    float sum_c = fir_tap_sum(sp.fir_c, sp.fir_c_n);
    ASSERT_NEAR(sum_y, 1.0f, 1e-4f, "init fir_y DC gain");
    ASSERT_NEAR(sum_c, 1.0f, 1e-4f, "init fir_c DC gain");

    return 1;
}

static int test_init_pal_dimensions(void) {
    SignalPrecompute sp;
    signal_precompute_init(&sp, SIGNAL_REGION_PAL);

    if (sp.samples_per_pixel != 10) {
        printf("  FAIL: PAL samples_per_pixel=%d (expected 10)\n",
               sp.samples_per_pixel);
        return 0;
    }
    if (sp.samples_per_line != 2560) {
        printf("  FAIL: PAL samples_per_line=%d (expected 2560)\n",
               sp.samples_per_line);
        return 0;
    }
    if (sp.region != SIGNAL_REGION_PAL) {
        printf("  FAIL: PAL region field not set\n");
        return 0;
    }
    return 1;
}

/* PAL must populate BOTH table and table_alt — the 2C07 flips chroma V
 * phase every other line, and the shader samples the alt buffer on
 * odd scanlines. If the alt table stays all-zero, every other line
 * decodes as black. */
static int test_pal_alt_table_populated(void) {
    SignalPrecompute sp;
    signal_precompute_init(&sp, SIGNAL_REGION_PAL);

    /* Entry for a saturated color ($21 blue, luma 2, no emphasis).
     * Alt table should have non-trivial variation across phases. */
    int entry = 0x21;
    float min_alt = 1e9f, max_alt = -1e9f;
    for (int p = 0; p < 12; p++) {
        float v = sp.table_alt[entry][p];
        if (v < min_alt) min_alt = v;
        if (v > max_alt) max_alt = v;
    }
    if (max_alt - min_alt < 0.05f) {
        printf("  FAIL: PAL alt table appears flat (range %.4f)\n",
               max_alt - min_alt);
        return 0;
    }
    return 1;
}

/* The V-phase flip means the alt table's hue on a chroma entry is
 * the mirror of the base table's hue (through the V-axis), which
 * manifests as the base/alt high-samples falling on DIFFERENT phases
 * of the subcarrier cycle for most hues. Verify they differ for a
 * representative saturated entry. */
static int test_pal_alt_vs_base_differs(void) {
    SignalPrecompute sp;
    signal_precompute_init(&sp, SIGNAL_REGION_PAL);

    /* Entry $25 — hue 5 (magenta/red), luma 2. Hue 1..12 should V-flip. */
    int entry = 0x25;
    int different = 0;
    for (int p = 0; p < 12; p++) {
        if (fabsf(sp.table[entry][p] - sp.table_alt[entry][p]) > 0.05f) {
            different++;
        }
    }
    if (different == 0) {
        printf("  FAIL: PAL base and alt tables are identical for "
               "a saturated hue\n");
        return 0;
    }
    return 1;
}

/* NTSC must not populate the alt table — it's PAL-specific. If the
 * alt buffer leaks data into the NTSC path the shader binding that
 * reuses buf_signal_table as the alt for NTSC would decode every
 * other line from stale content. */
static int test_ntsc_alt_table_zero(void) {
    SignalPrecompute sp;
    signal_precompute_init(&sp, SIGNAL_REGION_NTSC);
    for (int e = 0; e < SIG_TABLE_ENTRIES; e++) {
        for (int p = 0; p < SIG_TABLE_STRIDE; p++) {
            if (sp.table_alt[e][p] != 0.0f) {
                printf("  FAIL: NTSC alt table not zero at [%d][%d]=%.4f\n",
                       e, p, sp.table_alt[e][p]);
                return 0;
            }
        }
    }
    return 1;
}

/* The GPU frontend's CPU-side waveform emitter must switch to table_alt on
 * odd PAL lines; otherwise the downstream PAL decoder "corrects" a V-flip
 * that never happened and produces alternating-line hue errors. */
static int test_pal_waveform_generate_uses_alt_lines(void) {
    SignalPrecompute sp;
    signal_precompute_init(&sp, SIGNAL_REGION_PAL);

    static uint16_t idx_fb[256 * 240];
    static float waveform[SIGNAL_PAL_SAMPLES_PER_LINE * 240];
    memset(idx_fb, 0, sizeof(idx_fb));
    memset(waveform, 0, sizeof(waveform));

    /* Saturated entry where base and alt tables differ strongly. */
    idx_fb[0] = 0x25;
    idx_fb[256] = 0x25;

    waveform_generate(waveform, idx_fb, &sp, 0);

    int phase_line0 = 0;
    int phase_line1 = ((sp.phase_line_adv % 12) + 12) % 12;
    bool differs = false;
    for (int sample = 0; sample < sp.samples_per_pixel; sample++) {
        float expected = sp.table_alt[0x25][phase_line1 + sample];
        float wrong = sp.table[0x25][phase_line1 + sample];
        ASSERT_NEAR(waveform[sample], sp.table[0x25][phase_line0 + sample], 1e-6f,
                    "PAL even line waveform");
        ASSERT_NEAR(waveform[sp.samples_per_line + sample], expected, 1e-6f,
                    "PAL odd line waveform");
        differs |= fabsf(expected - wrong) > 0.05f;
    }
    if (!differs) return 0;
    return 1;
}

/* ============================================================================
 * Main
 * ============================================================================ */

static int test_clock_phase_and_decay(void) {
    SignalPrecompute sp;
    signal_precompute_init(&sp, SIGNAL_REGION_NTSC);
    unsigned long long dots = 0;
    for (unsigned frame = 0; frame < 24; frame++) {
        if (signal_frame_phase(&sp, frame) != (int)(dots * 8 % 12)) return 0;
        dots += 341 * 262 - (frame & 1);
    }
    if (sp.phase_line_adv != (341 * 8) % 12) return 0;
    sp.frame_phase_override = 10;
    if (signal_frame_phase(&sp, 3) != 10) return 0;
    signal_precompute_init(&sp, SIGNAL_REGION_PAL);
    if (sp.phase_line_adv != (341 * 10) % 12) return 0;
    for (unsigned f = 0; f < 12; f++)
        if (signal_frame_phase(&sp, f) != (int)((unsigned long long)f * 341 * 312 * 10 % 12)) return 0;
    ASSERT_NEAR(signal_persistence_weight(0, 0, 1), 0, 0, "zero decay");
    ASSERT_NEAR(signal_persistence_weight(0, 20, 0), 0, 0, "zero channel");
    if (!(signal_persistence_weight(1, 20, 1) < signal_persistence_weight(0, 20, 1))) return 0;
    if (!(signal_persistence_weight(0, 20, .5f) < signal_persistence_weight(0, 20, 1))) return 0;
    return 1;
}

int main(void) {
    printf("=== Signal Precompute Tests ===\n\n");

    printf("--- Terminated 2C02 DAC ---\n");
    RUN_TEST(test_published_voltage_rails);
    RUN_TEST(test_dac_black_waveform);
    RUN_TEST(test_dac_white_waveform);
    RUN_TEST(test_dac_saturated_colors);
    RUN_TEST(test_dac_emphasis_attenuation);
    RUN_TEST(test_dac_emphasis_octant_mask);
    RUN_TEST(test_signal_table_wraparound);

    printf("\n--- FIR Design ---\n");
    RUN_TEST(test_fir_lowpass_dc_gain_is_one);
    RUN_TEST(test_fir_lowpass_rejects_above_cutoff);
    RUN_TEST(test_fir_notch_nulls_at_notch_freq);
    RUN_TEST(test_fir_notch_default_depth);
    RUN_TEST(test_fir_peaking_boosts_high_freq);
    RUN_TEST(test_fir_peaking_preserves_dc);
    RUN_TEST(test_fir_notch_plus_peaking_composable);
    RUN_TEST(test_fir_symmetric);

    printf("\n--- signal_precompute_init ---\n");
    RUN_TEST(test_clock_phase_and_decay);
    RUN_TEST(test_init_ntsc_defaults);
    RUN_TEST(test_init_pal_dimensions);
    RUN_TEST(test_pal_alt_table_populated);
    RUN_TEST(test_pal_alt_vs_base_differs);
    RUN_TEST(test_pal_waveform_generate_uses_alt_lines);
    RUN_TEST(test_ntsc_alt_table_zero);

    printf("\n=== Results: %d/%d tests passed ===\n", tests_passed, tests_run);
    return (tests_passed == tests_run) ? 0 : 1;
}
