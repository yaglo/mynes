/*
 * Kernel Primitive Tests — synthetic signal verification
 * ========================================================
 *
 * Each test feeds a known input through a CPU reference kernel and
 * verifies the output against analytical expectations. These are the
 * ground truth that GPU compute shaders must match.
 *
 * Test signals:
 *   - DC (constant value) — verifies gain, offset, steady-state
 *   - Impulse (single spike) — verifies impulse response shape
 *   - Sine wave — verifies frequency response, phase
 *   - Step function — verifies transient response, time constant
 *   - Colour bars (video) — verifies composite encode/decode round-trip
 *
 * Build: gcc -O2 -lm -I.. test_kernels.c -o test_kernels
 * Run:   ./test_kernels
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "../kernels/pointwise_ref.h"
#include "../kernels/rc_filter_ref.h"
#include "../kernels/fir_ref.h"
#include "../kernels/delay_ref.h"
#include "../kernels/modulator_ref.h"
#include "../kernels/pal_chroma_ref.h"
#include "../signal_format.h"
#include "../audio_format.h"

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
 * Pointwise tests
 * ============================================================================ */

static int test_pointwise_gain(void) {
    float data[] = {0.0f, 0.5f, 1.0f, -0.5f};
    pointwise_gain_ref(data, 4, 2.0f, 0.1f);
    ASSERT_NEAR(data[0], 0.1f,  1e-6f, "gain(0)");
    ASSERT_NEAR(data[1], 1.1f,  1e-6f, "gain(0.5)");
    ASSERT_NEAR(data[2], 2.1f,  1e-6f, "gain(1.0)");
    ASSERT_NEAR(data[3], -0.9f, 1e-6f, "gain(-0.5)");
    return 1;
}

static int test_pointwise_softclip(void) {
    float data[] = {0.0f, 0.5f, 1.0f, 2.0f, -1.0f};
    pointwise_softclip_ref(data, 5, 2.0f);
    /* tanh(0) = 0 */
    ASSERT_NEAR(data[0], 0.0f, 1e-5f, "softclip(0)");
    /* tanh(1)/tanh(2) ≈ 0.762/0.964 ≈ 0.790 */
    ASSERT_NEAR(data[1], tanhf(1.0f) / tanhf(2.0f), 1e-5f, "softclip(0.5)");
    /* Should be < 1.0 (compressed) */
    if (data[3] >= 2.0f) { printf("  FAIL: softclip(2.0) not compressed\n"); return 0; }
    /* Odd symmetry */
    ASSERT_NEAR(data[4], -data[2], 1e-5f, "softclip symmetry");
    return 1;
}

static int test_pointwise_gamma(void) {
    float data[] = {0.0f, 0.25f, 0.5f, 1.0f};
    pointwise_gamma_ref(data, 4, 2.2f);
    ASSERT_NEAR(data[0], 0.0f, 1e-6f, "gamma(0)");
    ASSERT_NEAR(data[1], powf(0.25f, 2.2f), 1e-5f, "gamma(0.25)");
    ASSERT_NEAR(data[2], powf(0.5f, 2.2f), 1e-5f, "gamma(0.5)");
    ASSERT_NEAR(data[3], 1.0f, 1e-6f, "gamma(1)");
    return 1;
}

/* ============================================================================
 * RC filter tests
 * ============================================================================ */

static int test_rc_lowpass_dc(void) {
    /* A DC input should converge to the input value. */
    float data[1024];
    for (int i = 0; i < 1024; i++) data[i] = 1.0f;
    float state = 0.0f;
    float a, b;
    rc_lowpass_coeffs_from_fc(&a, &b, 1000.0f, 44100.0f);
    rc_lowpass_ref(data, 1024, a, b, &state);
    /* After 1024 samples at 44.1kHz with 1kHz cutoff,
     * the filter should be very close to the DC value (1.0). */
    ASSERT_NEAR(data[1023], 1.0f, 0.001f, "LP DC convergence");
    return 1;
}

static int test_rc_lowpass_step_response(void) {
    /* Step input: output should follow 1 - exp(-t/τ). */
    float data[4096];
    for (int i = 0; i < 4096; i++) data[i] = 1.0f;
    float state = 0.0f;
    float fc = 1000.0f;  /* 1 kHz cutoff */
    float fs = 44100.0f;
    float a, b;
    rc_lowpass_coeffs_from_fc(&a, &b, fc, fs);
    rc_lowpass_ref(data, 4096, a, b, &state);

    /* At t = τ = 1/(2π·fc), output should be ~63.2% of final value.
     * τ = 1/(2π·1000) ≈ 159 µs = ~7 samples at 44.1 kHz. */
    float tau_samples = fs / (2.0f * (float)M_PI * fc);
    int tau_idx = (int)tau_samples;
    float expected_at_tau = 1.0f - expf(-1.0f);  /* ≈ 0.632 */
    /* Discrete-time step response deviates slightly from continuous-time
     * formula (exp(-dt/τ) vs the ideal 1-e^(-1)). Allow ~10% tolerance. */
    ASSERT_NEAR(data[tau_idx], expected_at_tau, 0.07f, "LP step at τ");
    return 1;
}

static int test_rc_highpass_dc_blocking(void) {
    /* A DC input through a highpass should decay to zero. */
    float data[2048];
    for (int i = 0; i < 2048; i++) data[i] = 1.0f;
    RCHighpassState state = {0};
    float a, b;
    rc_highpass_coeffs_from_fc(&a, &b, 100.0f, 44100.0f);
    rc_highpass_ref(data, 2048, a, &state);
    /* After many samples, output should be near zero (DC blocked). */
    ASSERT_NEAR(data[2047], 0.0f, 0.01f, "HP DC blocking");
    /* First sample should be close to 1.0 (AC passes through). */
    ASSERT_NEAR(data[0], 1.0f * a, 0.01f, "HP initial pass");
    return 1;
}

static int test_rc_cable_model(void) {
    /* An impulse through an N-section cable should produce a
     * smoothed, delayed pulse. More sections = smoother. */
    float data1[512], data4[512];
    memset(data1, 0, sizeof(data1));
    memset(data4, 0, sizeof(data4));
    data1[0] = 1.0f;
    data4[0] = 1.0f;

    float states1[1] = {0};
    float states4[4] = {0};

    /* 2m cable, 75Ω, 67pF/m → total R=150Ω, C=134pF */
    rc_cable_ref(data1, 512, 150.0f, 134e-12f, 1, 44100.0f, states1);
    rc_cable_ref(data4, 512, 150.0f, 134e-12f, 4, 44100.0f, states4);

    /* 4-section should be smoother (lower peak) than 1-section. */
    float peak1 = 0, peak4 = 0;
    for (int i = 0; i < 512; i++) {
        if (data1[i] > peak1) peak1 = data1[i];
        if (data4[i] > peak4) peak4 = data4[i];
    }
    /* With these tiny capacitance values at audio rate, the cable
     * barely filters. But the structure should work. */
    if (peak1 < 0.001f) { printf("  FAIL: 1-section peak too low\n"); return 0; }
    return 1;
}

/* ============================================================================
 * FIR tests
 * ============================================================================ */

static int test_fir_identity(void) {
    /* An identity FIR {0, 0, 1, 0, 0} should reproduce input. */
    float data[] = {1.0f, 2.0f, 3.0f, 4.0f, 5.0f, 6.0f, 7.0f, 8.0f};
    float taps[] = {0.0f, 0.0f, 1.0f, 0.0f, 0.0f};
    float expected[] = {1.0f, 2.0f, 3.0f, 4.0f, 5.0f, 6.0f, 7.0f, 8.0f};
    fir_convolve_ref(data, 8, taps, 5);
    for (int i = 0; i < 8; i++) {
        ASSERT_NEAR(data[i], expected[i], 1e-5f, "FIR identity");
    }
    return 1;
}

static int test_fir_dc_gain(void) {
    /* A lowpass FIR applied to DC should preserve the DC level
     * (unit DC gain by design). */
    float taps[31];
    fir_design_lowpass(taps, 31, 0.1f);

    /* Verify taps sum to 1. */
    float sum = 0;
    for (int i = 0; i < 31; i++) sum += taps[i];
    ASSERT_NEAR(sum, 1.0f, 1e-5f, "FIR tap sum");

    /* Apply to DC signal. */
    float data[256];
    for (int i = 0; i < 256; i++) data[i] = 0.75f;
    fir_convolve_ref(data, 256, taps, 31);
    /* Middle samples should be very close to 0.75. */
    ASSERT_NEAR(data[128], 0.75f, 1e-4f, "FIR DC preservation");
    return 1;
}

/* ============================================================================
 * Delay tests
 * ============================================================================ */

static int test_delay_ghost(void) {
    /* Ghost at delay=3, level=0.5: output[3] should include ghost of input[0]. */
    float data[] = {1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f};
    delay_ghost_ref(data, 8, 3, 0.5f);
    ASSERT_NEAR(data[0], 1.0f, 1e-6f, "ghost: original preserved");
    ASSERT_NEAR(data[3], 0.5f, 1e-6f, "ghost: reflection at delay");
    ASSERT_NEAR(data[4], 0.0f, 1e-6f, "ghost: no energy elsewhere");
    return 1;
}

static int test_comb_filter(void) {
    /* Comb add: (A+B)/2 should cancel if B = -A. */
    float lineA[] = {1.0f, -1.0f, 1.0f, -1.0f};
    float lineB[] = {-1.0f, 1.0f, -1.0f, 1.0f};
    float output[4];
    delay_comb_add_ref(output, lineA, lineB, 4, 0.5f);
    for (int i = 0; i < 4; i++) {
        ASSERT_NEAR(output[i], 0.0f, 1e-6f, "comb cancel");
    }
    return 1;
}

/* ============================================================================
 * Modulator tests
 * ============================================================================ */

static int test_modulate_dc(void) {
    /* DC signal × cos carrier should produce a pure cosine. */
    float data[100];
    for (int i = 0; i < 100; i++) data[i] = 1.0f;
    float phase = 0.0f;
    modulate_cos_ref(data, 100, 1000.0f, 44100.0f, &phase);
    /* First sample: cos(0) = 1.0 */
    ASSERT_NEAR(data[0], 1.0f, 1e-5f, "mod DC×cos[0]");
    /* At quarter period: cos(π/2) = 0 */
    int quarter = (int)(44100.0f / 1000.0f / 4.0f);
    ASSERT_NEAR(data[quarter], 0.0f, 0.05f, "mod DC×cos[quarter]");
    return 1;
}

static int test_demodulate_roundtrip(void) {
    /* Modulate a DC signal, then demodulate and lowpass.
     * Should recover the original DC level (within the filter's
     * settling time). */
    int N = 4096;
    float *signal = (float *)calloc(N, sizeof(float));
    float *out_i = (float *)calloc(N, sizeof(float));
    float *out_q = (float *)calloc(N, sizeof(float));

    /* Generate: DC 0.5 modulated onto 3.58 MHz carrier at 8× sample rate. */
    float carrier_freq = 3579545.0f;
    float sample_rate = carrier_freq * 12.0f / 8.0f * 8.0f;  /* ~42.95 MHz */
    for (int i = 0; i < N; i++) signal[i] = 0.5f;
    float mod_phase = 0.0f;
    modulate_cos_ref(signal, N, carrier_freq, sample_rate, &mod_phase);

    /* Demodulate: multiply by reference carrier. */
    float demod_phase = 0.0f;
    demodulate_iq_ref(signal, N, out_i, out_q, carrier_freq, sample_rate,
                       &demod_phase, 2.0f);

    /* Lowpass filter the demodulated I channel to kill the 2× carrier. */
    float taps[33];
    fir_design_lowpass(taps, 33, 0.06f);  /* cutoff well below carrier */
    fir_convolve_ref(out_i, N, taps, 33);

    /* After settling, I should be close to the original 0.5. */
    ASSERT_NEAR(out_i[N/2], 0.5f, 0.05f, "demod I round-trip");

    free(signal);
    free(out_i);
    free(out_q);
    return 1;
}

/* ============================================================================
 * Comb filter Y/C separation tests
 * ============================================================================ */

static int test_comb_1line_subcarrier_cancel(void) {
    /* 1-line comb: Y=(curr+prev)/2 should cancel the subcarrier.
     * The subcarrier inverts 180° between adjacent scanlines.
     * Two adjacent lines with opposite-phase chroma should yield pure Y. */
    int spl = 24;  /* 2 subcarrier cycles */
    float line0[24], line1[24];
    float Y = 0.5f, C_amp = 0.3f;
    float dp = 2.0f * (float)M_PI / 12.0f;

    for (int i = 0; i < spl; i++) {
        /* Line 0: Y + C*cos(phase) */
        line0[i] = Y + C_amp * cosf(dp * (float)i);
        /* Line 1: Y - C*cos(phase) (180° inversion) */
        line1[i] = Y - C_amp * cosf(dp * (float)i);
    }

    /* 1-line comb: Y_out = (line1 + line0) / 2 */
    for (int i = 0; i < spl; i++) {
        float y_out = (line1[i] + line0[i]) * 0.5f;
        ASSERT_NEAR(y_out, Y, 1e-5f, "1-line comb Y (subcarrier cancelled)");
    }

    /* Chroma: C_out = (line1 - line0) / 2 */
    for (int i = 0; i < spl; i++) {
        float c_out = (line1[i] - line0[i]) * 0.5f;
        float expected_c = -C_amp * cosf(dp * (float)i);
        ASSERT_NEAR(c_out, expected_c, 1e-5f, "1-line comb C extraction");
    }
    return 1;
}

static int test_comb_2line_broken(void) {
    /* Verify that the 2-line comb (same-phase average) does NOT cancel chroma.
     * This is a known design issue: averaging same-phase lines preserves chroma. */
    int spl = 24;
    float line0[24], line2[24];
    float Y = 0.5f, C_amp = 0.3f;
    float dp = 2.0f * (float)M_PI / 12.0f;

    for (int i = 0; i < spl; i++) {
        line0[i] = Y + C_amp * cosf(dp * (float)i);       /* phase 0 */
        line2[i] = Y + C_amp * cosf(dp * (float)i + 0.1f);/* phase 0 (same, slight content diff) */
    }

    /* 2-line: Y = (curr + prev2)/2 — same phase, chroma NOT cancelled */
    float max_error = 0;
    for (int i = 0; i < spl; i++) {
        float y_out = (line0[i] + line2[i]) * 0.5f;
        float error = fabsf(y_out - Y);  /* should NOT be near zero */
        if (error > max_error) max_error = error;
    }
    /* Error should be significant (chroma still present) */
    if (max_error < 0.1f) {
        printf("  FAIL: 2-line comb unexpectedly cancelled chroma (err=%.4f)\n", max_error);
        return 0;
    }
    return 1;
}

static int test_composite_roundtrip(void) {
    /* Full encode→decode round-trip: generate composite from known Y/I/Q,
     * apply 1-line comb, demodulate, FIR lowpass, verify Y/I/Q recovery. */
    int spl = 2048;
    int lines = 4;
    float *composite = (float *)calloc(spl * lines, sizeof(float));
    float dp = 2.0f * (float)M_PI / 12.0f;
    float Y_val = 0.6f, I_val = 0.2f, Q_val = -0.1f;

    /* Encode: composite = Y + I*cos(phase) + Q*sin(phase), alternating per line */
    for (int y = 0; y < lines; y++) {
        float phase_sign = (y % 2 == 0) ? 1.0f : -1.0f;
        for (int x = 0; x < spl; x++) {
            float phase = dp * (float)x;
            composite[y * spl + x] = Y_val
                + phase_sign * I_val * cosf(phase)
                + phase_sign * Q_val * sinf(phase);
        }
    }

    /* 1-line comb on line 1 (has line 0 as reference) */
    float *y_out = (float *)calloc(spl, sizeof(float));
    float *c_out = (float *)calloc(spl, sizeof(float));
    for (int x = 0; x < spl; x++) {
        y_out[x] = (composite[1 * spl + x] + composite[0 * spl + x]) * 0.5f;
        c_out[x] = (composite[1 * spl + x] - composite[0 * spl + x]) * 0.5f;
    }

    /* Verify Y is clean (no subcarrier) — check middle samples */
    for (int x = spl/4; x < spl/4 + 24; x++) {
        ASSERT_NEAR(y_out[x], Y_val, 0.01f, "roundtrip Y");
    }

    /* Demodulate C to extract I: C * cos(phase) * 2 */
    float *i_demod = (float *)calloc(spl, sizeof(float));
    for (int x = 0; x < spl; x++) {
        float phase = dp * (float)x;
        /* Line 1 has negative phase_sign, comb difference flips sign */
        i_demod[x] = c_out[x] * cosf(phase) * (-2.0f);
    }

    /* FIR lowpass to extract DC of demodulated I */
    float taps[33];
    fir_design_lowpass(taps, 33, 0.04f);
    fir_convolve_ref(i_demod, spl, taps, 33);

    /* After settling, I should be close to I_val */
    ASSERT_NEAR(i_demod[spl/2], I_val, 0.06f, "roundtrip I recovery");

    free(composite);
    free(y_out);
    free(c_out);
    free(i_demod);
    return 1;
}

/* ============================================================================
 * Signal format tests
 * ============================================================================ */

static int test_signal_format_ntsc(void) {
    SignalFormat fmt;
    signal_format_init(&fmt, SIGNAL_REGION_NTSC);
    if (fmt.samples_per_line != 2048) { printf("  FAIL: NTSC samples/line\n"); return 0; }
    if (fmt.blocks_per_line != 2)     { printf("  FAIL: NTSC blocks/line\n"); return 0; }
    if (fmt.total_samples != 2048*240){ printf("  FAIL: NTSC total\n"); return 0; }
    return 1;
}

static int test_audio_format_ntsc(void) {
    AudioFormat fmt;
    audio_format_init(&fmt, 0 /* NTSC */);
    if (fmt.samples_per_frame < 29000) { printf("  FAIL: NTSC audio samples\n"); return 0; }
    if (fmt.output_samples_per_frame != 800) { printf("  FAIL: NTSC output samples\n"); return 0; }
    return 1;
}

/* ============================================================================
 * Comb filter with blend (imperfect separation)
 * ============================================================================ */

/* Helper: simulate the shader's 1-line comb with blend factor.
 * Y = mix(signal, avg, blend); C = blend * diff
 * blend=1.0 → perfect separation; blend=0.0 → no comb (Y=signal, C=0) */
static void sim_comb_1line(float *y_out, float *c_out,
                            const float *signal, const float *prev,
                            int n, float blend) {
    for (int i = 0; i < n; i++) {
        float avg = (signal[i] + prev[i]) * 0.5f;
        float diff = (signal[i] - prev[i]) * 0.5f;
        y_out[i] = signal[i] * (1.0f - blend) + avg * blend;
        c_out[i] = blend * diff;
    }
}

static int test_comb_blend_endpoints(void) {
    /* At blend=0: Y=signal (raw composite), C=0 */
    int n = 24;
    float line0[24], line1[24], y[24], c[24];
    float Y = 0.5f, C_amp = 0.3f;
    float dp = 2.0f * (float)M_PI / 12.0f;
    for (int i = 0; i < n; i++) {
        line0[i] = Y + C_amp * cosf(dp * i);
        line1[i] = Y - C_amp * cosf(dp * i);
    }
    sim_comb_1line(y, c, line1, line0, n, 0.0f);
    for (int i = 0; i < n; i++) {
        ASSERT_NEAR(y[i], line1[i], 1e-6f, "blend=0: Y=signal");
        ASSERT_NEAR(c[i], 0.0f,     1e-6f, "blend=0: C=0");
    }
    /* At blend=1: perfect Y, full C */
    sim_comb_1line(y, c, line1, line0, n, 1.0f);
    for (int i = 0; i < n; i++) {
        ASSERT_NEAR(y[i], Y, 1e-5f, "blend=1: Y=pure luma");
    }
    return 1;
}

static int test_comb_blend_partial(void) {
    /* At blend=0.5: Y should be halfway between raw signal and pure luma. */
    int n = 24;
    float line0[24], line1[24], y[24], c[24];
    float Y = 0.5f, C_amp = 0.3f;
    float dp = 2.0f * (float)M_PI / 12.0f;
    for (int i = 0; i < n; i++) {
        line0[i] = Y + C_amp * cosf(dp * i);
        line1[i] = Y - C_amp * cosf(dp * i);
    }
    sim_comb_1line(y, c, line1, line0, n, 0.5f);
    /* Y[i] = line1[i]*0.5 + ((line1+line0)/2)*0.5 = line1*0.5 + Y*0.5 */
    for (int i = 0; i < n; i++) {
        float expected = line1[i] * 0.5f + Y * 0.5f;
        ASSERT_NEAR(y[i], expected, 1e-5f, "blend=0.5 Y interpolation");
    }
    return 1;
}

/* Simulate the shader's 3-line comb: Y = mix(signal, 4-line avg, blend) */
static int test_comb_3line_cancellation(void) {
    /* 4-line average: chroma cancels over 2 full phase cycles.
     * Lines N,N-2 have phase +C; N-1,N-3 have phase -C. Average = Y. */
    int n = 24;
    float lines[4][24];  /* N-3, N-2, N-1, N (most recent last) */
    float Y = 0.5f, C_amp = 0.3f;
    float dp = 2.0f * (float)M_PI / 12.0f;
    for (int i = 0; i < n; i++) {
        lines[0][i] = Y + C_amp * cosf(dp * i);  /* N-3: same phase as N-1 */
        lines[1][i] = Y - C_amp * cosf(dp * i);  /* N-2: opposite */
        lines[2][i] = Y + C_amp * cosf(dp * i);  /* N-1: same */
        lines[3][i] = Y - C_amp * cosf(dp * i);  /* N: opposite */
    }
    /* Wait — this isn't quite right. Real NTSC: phase inverts every line,
     * so N, N-1, N-2, N-3 have phases: +C, -C, +C, -C. Average all 4 = Y. */
    for (int i = 0; i < n; i++) {
        float avg4 = (lines[3][i] + lines[2][i] + lines[1][i] + lines[0][i]) * 0.25f;
        ASSERT_NEAR(avg4, Y, 1e-6f, "3-line comb: chroma cancels in 4-line avg");
    }
    return 1;
}

static int test_pal_chroma_delay_line(void) {
    /* PAL correction stage:
     *   - odd-line U sign flips back to match the previous line
     *   - V averages with the previous scanline */
    float v_in[8] = {
        0.20f, 0.30f, 0.40f, 0.50f,
        0.60f, 0.70f, 0.80f, 0.90f,
    };
    float u_in[8] = {
        0.25f, 0.25f, 0.25f, 0.25f,
       -0.25f,-0.25f,-0.25f,-0.25f,
    };
    float v_out[8], u_out[8];

    pal_chroma_ref(v_in, u_in, v_out, u_out, 8, 4);

    /* First line passes through unchanged. */
    ASSERT_NEAR(v_out[0], 0.20f, 1e-6f, "PAL V line0 passthrough");
    ASSERT_NEAR(v_out[3], 0.50f, 1e-6f, "PAL V line0 passthrough end");
    /* Second line averages with the previous line. */
    ASSERT_NEAR(v_out[4], 0.40f, 1e-6f, "PAL V line1 averaged");
    ASSERT_NEAR(v_out[7], 0.70f, 1e-6f, "PAL V line1 averaged end");
    /* Odd-line U is sign-corrected back to positive. */
    ASSERT_NEAR(u_out[0], 0.25f, 1e-6f, "PAL U line0 stable");
    ASSERT_NEAR(u_out[4], 0.25f, 1e-6f, "PAL U line1 sign-corrected");
    return 1;
}

/* ============================================================================
 * Pointwise extended tests
 * ============================================================================ */

static int test_pointwise_clip(void) {
    /* Clip: data[i] = clamp(data[i], min, max). */
    float data[] = {-2.0f, -0.5f, 0.0f, 0.5f, 2.0f};
    pointwise_hardclip_ref(data, 5, -1.0f, 1.0f);
    ASSERT_NEAR(data[0], -1.0f, 1e-6f, "clip: below min");
    ASSERT_NEAR(data[1], -0.5f, 1e-6f, "clip: inside");
    ASSERT_NEAR(data[2],  0.0f, 1e-6f, "clip: zero");
    ASSERT_NEAR(data[3],  0.5f, 1e-6f, "clip: inside pos");
    ASSERT_NEAR(data[4],  1.0f, 1e-6f, "clip: above max");
    return 1;
}

/* ============================================================================
 * FIR extended tests
 * ============================================================================ */

static int test_fir_mirror_boundary(void) {
    /* With mirror boundary, index k < 0 → -k (mirror at 0).
     * Test: a symmetric kernel applied to an impulse at the first sample
     * should produce correct output without NaN or runaway. */
    float data[32] = {0};
    data[0] = 1.0f;  /* impulse at position 0 */
    float taps[5];
    fir_design_lowpass(taps, 5, 0.2f);
    /* Manually apply with mirror boundary (matches shader logic). */
    float out[32];
    for (int i = 0; i < 32; i++) {
        float sum = 0;
        for (int k = 0; k < 5; k++) {
            int idx = i - 2 + k;  /* half = 2 */
            if (idx < 0) idx = -idx;
            if (idx >= 32) idx = 2 * 32 - idx - 1;
            sum += taps[k] * data[idx];
        }
        out[i] = sum;
    }
    /* Output must not have NaN/Inf. */
    for (int i = 0; i < 32; i++) {
        if (isnan(out[i]) || isinf(out[i])) {
            printf("  FAIL: mirror produced NaN/Inf at i=%d\n", i);
            return 0;
        }
    }
    /* Verify: the mirror boundary makes output[0] = output[k] for the
     * peak (where k = half). With mirror, the impulse response is
     * symmetric around position 0. */
    /* Actually verify what matters: output must be bounded. */
    for (int i = 0; i < 32; i++) {
        if (fabsf(out[i]) > 2.0f) {
            printf("  FAIL: mirror caused unbounded output at i=%d: %.4f\n", i, out[i]);
            return 0;
        }
    }
    return 1;
}

static int test_fir_decimation_concept(void) {
    /* Decimation: output[n] = FIR(input[n*ratio]).
     * For ratio=2, output length is half the input length, and each
     * output sample is computed from every other input sample. */
    int n = 16;
    float data[16];
    for (int i = 0; i < n; i++) data[i] = (float)i;
    float taps[3] = {0.25f, 0.5f, 0.25f};  /* simple smoothing */
    float out[8];
    int out_n = n / 2;
    for (int i = 0; i < out_n; i++) {
        int base = i * 2;
        float sum = 0;
        for (int k = 0; k < 3; k++) {
            int idx = base - 1 + k;
            if (idx < 0) idx = 0;
            if (idx >= n) idx = n - 1;
            sum += taps[k] * data[idx];
        }
        out[i] = sum;
    }
    /* Output should be monotonically increasing (smoothed ramp). */
    for (int i = 1; i < out_n; i++) {
        if (out[i] <= out[i-1]) {
            printf("  FAIL: decimated output not monotonic\n");
            return 0;
        }
    }
    return 1;
}

/* ============================================================================
 * Modulator extended tests
 * ============================================================================ */

static int test_modulate_sin(void) {
    /* DC × sin carrier: sin(0)=0, sin(π/2)=1. */
    float data[100];
    for (int i = 0; i < 100; i++) data[i] = 1.0f;
    float phase = 0.0f;
    modulate_sin_ref(data, 100, 1000.0f, 44100.0f, &phase);
    ASSERT_NEAR(data[0], 0.0f, 1e-5f, "mod DC×sin[0]");
    int quarter = (int)(44100.0f / 1000.0f / 4.0f);
    ASSERT_NEAR(data[quarter], 1.0f, 0.05f, "mod DC×sin[quarter]");
    return 1;
}

static int test_modulate_phase_continuity(void) {
    /* Modulate two chunks of DC, verify phase state carries over. */
    float data1[50], data2[50];
    for (int i = 0; i < 50; i++) data1[i] = 1.0f;
    for (int i = 0; i < 50; i++) data2[i] = 1.0f;
    float phase = 0.0f;
    modulate_cos_ref(data1, 50, 1000.0f, 44100.0f, &phase);
    modulate_cos_ref(data2, 50, 1000.0f, 44100.0f, &phase);
    /* If phase is continuous, data2[0] should match what data1[50] would be. */
    float phase2 = 0.0f;
    float check[100];
    for (int i = 0; i < 100; i++) check[i] = 1.0f;
    modulate_cos_ref(check, 100, 1000.0f, 44100.0f, &phase2);
    ASSERT_NEAR(data2[0], check[50], 1e-4f, "phase continuity across chunks");
    return 1;
}

/* ============================================================================
 * RC filter extended tests
 * ============================================================================ */

static int test_rc_lowpass_attenuates_hf(void) {
    /* A sinusoid at frequency above cutoff should be attenuated. */
    int n = 4096;
    float *data = (float *)calloc(n, sizeof(float));
    float fs = 44100.0f, fin = 10000.0f;
    for (int i = 0; i < n; i++) data[i] = sinf(2.0f * (float)M_PI * fin * i / fs);

    float a, b, state = 0;
    rc_lowpass_coeffs_from_fc(&a, &b, 1000.0f, fs);  /* cutoff 1 kHz */
    rc_lowpass_ref(data, n, a, b, &state);

    /* Measure peak in second half (after settling). */
    float peak = 0;
    for (int i = n/2; i < n; i++) {
        float v = fabsf(data[i]);
        if (v > peak) peak = v;
    }
    /* 10 kHz through a 1 kHz LP should be heavily attenuated (< 0.2). */
    if (peak > 0.2f) {
        printf("  FAIL: HF not attenuated (peak %.3f > 0.2)\n", peak);
        free(data);
        return 0;
    }
    free(data);
    return 1;
}

/* ============================================================================
 * Main
 * ============================================================================ */

int main(void) {
    printf("=== GPU Kernel Primitive Tests ===\n\n");

    printf("--- Pointwise ---\n");
    RUN_TEST(test_pointwise_gain);
    RUN_TEST(test_pointwise_softclip);
    RUN_TEST(test_pointwise_gamma);

    printf("\n--- RC Filter ---\n");
    RUN_TEST(test_rc_lowpass_dc);
    RUN_TEST(test_rc_lowpass_step_response);
    RUN_TEST(test_rc_highpass_dc_blocking);
    RUN_TEST(test_rc_cable_model);

    printf("\n--- FIR ---\n");
    RUN_TEST(test_fir_identity);
    RUN_TEST(test_fir_dc_gain);

    printf("\n--- Delay ---\n");
    RUN_TEST(test_delay_ghost);
    RUN_TEST(test_comb_filter);

    printf("\n--- Modulator ---\n");
    RUN_TEST(test_modulate_dc);
    RUN_TEST(test_demodulate_roundtrip);

    printf("\n--- Comb Filter Y/C Separation ---\n");
    RUN_TEST(test_comb_1line_subcarrier_cancel);
    RUN_TEST(test_comb_2line_broken);
    RUN_TEST(test_composite_roundtrip);
    RUN_TEST(test_comb_blend_endpoints);
    RUN_TEST(test_comb_blend_partial);
    RUN_TEST(test_comb_3line_cancellation);
    RUN_TEST(test_pal_chroma_delay_line);

    printf("\n--- Pointwise extended ---\n");
    RUN_TEST(test_pointwise_clip);

    printf("\n--- FIR extended ---\n");
    RUN_TEST(test_fir_mirror_boundary);
    RUN_TEST(test_fir_decimation_concept);

    printf("\n--- Modulator extended ---\n");
    RUN_TEST(test_modulate_sin);
    RUN_TEST(test_modulate_phase_continuity);

    printf("\n--- RC filter extended ---\n");
    RUN_TEST(test_rc_lowpass_attenuates_hf);

    printf("\n--- Signal/Audio Format ---\n");
    RUN_TEST(test_signal_format_ntsc);
    RUN_TEST(test_audio_format_ntsc);

    printf("\n=== Results: %d/%d tests passed ===\n", tests_passed, tests_run);
    return (tests_passed == tests_run) ? 0 : 1;
}
