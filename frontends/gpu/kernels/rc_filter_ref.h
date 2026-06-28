/*
 * RC Filter — CPU Reference Implementation
 * ==========================================
 *
 * First-order IIR filter: y[n] = a · y[n-1] + b · x[n]
 *
 * This is THE fundamental primitive. Every capacitor in both the audio
 * and video signal chains is this operation:
 *
 *   - Coupling capacitor (DC blocking):  RC high-pass
 *   - Amplifier bandwidth limit:         RC low-pass
 *   - Cable shunt capacitance:           RC low-pass
 *   - Speaker crossover:                 RC high/low-pass
 *   - Power supply ripple filter:        RC low-pass
 *
 * The CPU reference is sequential (inherently O(N)). The GPU version
 * uses a parallel prefix scan (Blelloch algorithm) to achieve O(N/P)
 * where P = number of parallel threads. Both must produce identical
 * output within float32 precision.
 *
 * Mapping physical component values to filter coefficients:
 *
 *   RC low-pass (signal passes through R, C shunts to ground):
 *     fc = 1 / (2π · R · C)
 *     a  = exp(-dt / (R·C))  where dt = 1/sample_rate
 *     b  = 1 - a
 *
 *   RC high-pass (signal passes through C, R shunts to ground):
 *     fc = 1 / (2π · R · C)
 *     a  = R·C / (R·C + dt)  (simplified: a ≈ 1 - 2π·fc·dt for small dt)
 *     b  = a
 *     y[n] = a · y[n-1] + a · (x[n] - x[n-1])
 *     Note: high-pass is y[n] = a · (y[n-1] + x[n] - x[n-1]), which
 *     expands to a different recurrence than the standard IIR form.
 *     We handle this by preprocessing: hp_input[n] = x[n] - x[n-1],
 *     then running the same y = a·y + b·hp_input prefix scan.
 */

#ifndef RC_FILTER_REF_H
#define RC_FILTER_REF_H

#include <math.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* Compute RC low-pass coefficients from R (ohms) and C (farads). */
static inline void rc_lowpass_coeffs(float *a, float *b,
                                      float R, float C,
                                      float sample_rate) {
    float tau = R * C;
    float dt = 1.0f / sample_rate;
    *a = expf(-dt / tau);
    *b = 1.0f - *a;
}

/* Compute RC low-pass coefficients from cutoff frequency (Hz). */
static inline void rc_lowpass_coeffs_from_fc(float *a, float *b,
                                              float fc,
                                              float sample_rate) {
    float dt = 1.0f / sample_rate;
    float tau = 1.0f / (2.0f * (float)M_PI * fc);
    *a = expf(-dt / tau);
    *b = 1.0f - *a;
}

/* Compute RC high-pass coefficients from cutoff frequency (Hz).
 * For high-pass, the recurrence is:
 *   y[n] = a · y[n-1] + a · (x[n] - x[n-1])
 * which we split into:
 *   d[n] = x[n] - x[n-1]   (differencer, computed separately)
 *   y[n] = a · y[n-1] + a · d[n]
 * So a_hp = a, b_hp = a (same coefficient for both terms). */
static inline void rc_highpass_coeffs_from_fc(float *a, float *b,
                                               float fc,
                                               float sample_rate) {
    float dt = 1.0f / sample_rate;
    float rc = 1.0f / (2.0f * (float)M_PI * fc);
    *a = rc / (rc + dt);
    *b = *a;  /* for the differenced input */
}

/* ============================================================================
 * CPU reference: sequential RC low-pass filter (in-place).
 *
 * This is the ground truth. The GPU prefix scan MUST match this output
 * within float32 precision (relative error < 1e-5).
 *
 * state: the filter's memory (y[n-1]). Initialize to 0 for the first
 * call on a fresh signal. Preserved across calls for streaming (the
 * coupling cap's charge persists across frames).
 * ============================================================================ */
static inline void rc_lowpass_ref(float *data, int count,
                                   float a, float b,
                                   float *state) {
    float y = *state;
    for (int i = 0; i < count; i++) {
        y = a * y + b * data[i];
        data[i] = y;
    }
    *state = y;
}

/* CPU reference: sequential RC high-pass filter (in-place).
 * Requires a state for both y[n-1] and x[n-1]. */
typedef struct {
    float y_prev;   /* previous output */
    float x_prev;   /* previous input (for differencer) */
} RCHighpassState;

static inline void rc_highpass_ref(float *data, int count,
                                    float a,
                                    RCHighpassState *state) {
    float y = state->y_prev;
    float xp = state->x_prev;
    for (int i = 0; i < count; i++) {
        float x = data[i];
        y = a * (y + x - xp);
        xp = x;
        data[i] = y;
    }
    state->y_prev = y;
    state->x_prev = xp;
}

/* CPU reference: N-section distributed RC cable model.
 * Each section is one RC low-pass. N sections in series approximate
 * a distributed transmission line.
 *
 * total_R and total_C are the cable's aggregate resistance and
 * capacitance. Each section uses R/N and C/N.
 *
 * states: array of N floats (one y[n-1] per section). */
static inline void rc_cable_ref(float *data, int count,
                                 float total_R, float total_C,
                                 int num_sections,
                                 float sample_rate,
                                 float *states) {
    for (int s = 0; s < num_sections; s++) {
        float R_section = total_R / (float)num_sections;
        float C_section = total_C / (float)num_sections;
        float a, b;
        rc_lowpass_coeffs(&a, &b, R_section, C_section, sample_rate);
        rc_lowpass_ref(data, count, a, b, &states[s]);
    }
}

#endif /* RC_FILTER_REF_H */
