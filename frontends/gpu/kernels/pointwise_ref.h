/*
 * Pointwise Transfer Function — CPU Reference Implementation
 * ============================================================
 *
 * y[n] = f(x[n])   — each sample is independent.
 *
 * Used for: DAC voltage curves, gamma/degamma, transistor saturation
 * (tanh soft-clip), diode clipping, color matrix multiply, nonlinear
 * mixer LUTs, PSU hum injection, gain/offset.
 */

#ifndef POINTWISE_REF_H
#define POINTWISE_REF_H

#include <math.h>

/* Linear gain + offset: y = gain * x + offset */
static inline void pointwise_gain_ref(float *data, int count,
                                       float gain, float offset) {
    for (int i = 0; i < count; i++)
        data[i] = data[i] * gain + offset;
}

/* Soft clip (tanh saturation): y = tanh(x * drive) / tanh(drive)
 * drive = 1.0 is nearly linear; drive = 4.0 is heavy compression.
 * Models amplifier saturation, DAC nonlinearity, speaker distortion. */
static inline void pointwise_softclip_ref(float *data, int count,
                                           float drive) {
    if (drive < 0.01f) return;
    float inv_tanh_drive = 1.0f / tanhf(drive);
    for (int i = 0; i < count; i++)
        data[i] = tanhf(data[i] * drive) * inv_tanh_drive;
}

/* Hard clip: y = clamp(x, lo, hi)
 * Models beam current limits (can't go below zero / above cathode max). */
static inline void pointwise_hardclip_ref(float *data, int count,
                                           float lo, float hi) {
    for (int i = 0; i < count; i++) {
        if (data[i] < lo) data[i] = lo;
        if (data[i] > hi) data[i] = hi;
    }
}

/* Power curve (gamma): y = pow(x, exponent)
 * Models CRT phosphor response (gamma ≈ 2.2-2.5). */
static inline void pointwise_gamma_ref(float *data, int count,
                                        float exponent) {
    for (int i = 0; i < count; i++) {
        float x = data[i];
        data[i] = (x >= 0.0f) ? powf(x, exponent) : -powf(-x, exponent);
    }
}

/* Cubic DAC nonlinearity: y = x + k * (x³ - x)
 * k = 0 is linear; k = 1 is maximum cubic warping.
 * Models the NES 2A03's resistor ladder deviations. */
static inline void pointwise_dac_nonlin_ref(float *data, int count,
                                             float k) {
    if (k < 0.001f) return;
    for (int i = 0; i < count; i++) {
        float x = data[i];
        data[i] = x + k * (x * x * x - x);
    }
}

/* Additive sinusoidal injection (PSU hum, interference):
 * y[n] = x[n] + amplitude * sin(2π * freq * n / sample_rate + phase) */
static inline void pointwise_hum_ref(float *data, int count,
                                      float amplitude, float freq,
                                      float sample_rate, float *phase) {
    if (amplitude < 1e-6f) return;
    float dp = 2.0f * 3.14159265f * freq / sample_rate;
    float p = *phase;
    for (int i = 0; i < count; i++) {
        data[i] += amplitude * sinf(p);
        p += dp;
    }
    /* Wrap phase to avoid float precision loss over time. */
    while (p > 2.0f * 3.14159265f) p -= 2.0f * 3.14159265f;
    *phase = p;
}

/* LUT-based transfer: y = lut[clamp(x * scale + offset, 0, lut_size-1)]
 * Used for DAC voltage tables, palette lookups. */
static inline void pointwise_lut_ref(float *data, int count,
                                      const float *lut, int lut_size,
                                      float scale, float offset) {
    for (int i = 0; i < count; i++) {
        float idx_f = data[i] * scale + offset;
        int idx = (int)idx_f;
        if (idx < 0) idx = 0;
        if (idx >= lut_size) idx = lut_size - 1;
        data[i] = lut[idx];
    }
}

/* 3×3 matrix multiply (per-pixel): [r,g,b] = M × [y,i,q] + bias
 * Used for YIQ→RGB decode, color temperature, per-gun drive/cutoff. */
static inline void pointwise_matrix3_ref(float *r, float *g, float *b,
                                          const float *y, const float *i, const float *q,
                                          int count,
                                          const float m[3][3],
                                          const float bias[3]) {
    for (int n = 0; n < count; n++) {
        float yv = y[n], iv = i[n], qv = q[n];
        r[n] = m[0][0] * yv + m[0][1] * iv + m[0][2] * qv + bias[0];
        g[n] = m[1][0] * yv + m[1][1] * iv + m[1][2] * qv + bias[1];
        b[n] = m[2][0] * yv + m[2][1] * iv + m[2][2] * qv + bias[2];
    }
}

#endif /* POINTWISE_REF_H */
