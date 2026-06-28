/*
 * FIR Convolution — CPU Reference Implementation
 * ================================================
 *
 * y[n] = Σ h[k] · x[n-k]  for k = 0..N-1
 *
 * Each output sample is an independent dot product with the coefficient
 * array. Parallel across output samples on GPU.
 *
 * Used for: bandwidth limiting (Y, chroma), anti-alias decimation
 * (1.79 MHz → 48 kHz), demodulation carrier multiply + filter,
 * cable frequency response shaping.
 *
 * The existing composite.h has comp_fir_symmetric with NEON/AVX2
 * intrinsics. This reference is simpler (no SIMD, no symmetry
 * exploitation) for GPU verification.
 */

#ifndef FIR_REF_H
#define FIR_REF_H

#include <stdlib.h>
#include <string.h>
#include <math.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* Standard FIR convolution (in-place via scratch buffer).
 * Edge handling: mirror boundary (same as comp_fir_symmetric). */
static inline void fir_convolve_ref(float *data, int count,
                                     const float *taps, int num_taps) {
    if (num_taps < 1 || count < 1) return;
    /* Use a stack-allocated scratch for reasonable sizes. */
    float scratch[8192];
    float *out = (count <= 8192) ? scratch : (float *)malloc(count * sizeof(float));
    int half = num_taps / 2;

    for (int n = 0; n < count; n++) {
        float sum = 0.0f;
        for (int k = 0; k < num_taps; k++) {
            int idx = n - half + k;
            /* Mirror boundary: reflect at edges. */
            if (idx < 0) idx = -idx;
            if (idx >= count) idx = 2 * count - idx - 2;
            if (idx < 0) idx = 0;
            if (idx >= count) idx = count - 1;
            sum += taps[k] * data[idx];
        }
        out[n] = sum;
    }
    memcpy(data, out, count * sizeof(float));
    if (out != scratch) free(out);
}

/* FIR with decimation: output has count/ratio samples.
 * Used for downsampling (e.g., 1.79 MHz → 48 kHz). */
static inline void fir_decimate_ref(const float *input, int input_count,
                                     float *output, int output_count,
                                     const float *taps, int num_taps,
                                     int decimation_ratio) {
    int half = num_taps / 2;
    for (int n = 0; n < output_count; n++) {
        int base = n * decimation_ratio;
        float sum = 0.0f;
        for (int k = 0; k < num_taps; k++) {
            int idx = base - half + k;
            if (idx < 0) idx = 0;
            if (idx >= input_count) idx = input_count - 1;
            sum += taps[k] * input[idx];
        }
        output[n] = sum;
    }
}

/* Design a Hamming-windowed sinc lowpass FIR.
 * cutoff: normalized frequency (0..0.5, in cycles/sample).
 * num_taps: must be odd. Taps are normalized to unit DC gain. */
static inline void fir_design_lowpass(float *taps, int num_taps,
                                       float cutoff) {
    int half = num_taps / 2;
    float sum = 0.0f;
    for (int k = 0; k < num_taps; k++) {
        int m = k - half;
        float sinc = (m == 0)
            ? 2.0f * cutoff
            : sinf(2.0f * (float)M_PI * cutoff * (float)m)
              / ((float)M_PI * (float)m);
        float w = 0.54f - 0.46f * cosf(2.0f * (float)M_PI * (float)k / (float)(num_taps - 1));
        taps[k] = sinc * w;
        sum += taps[k];
    }
    float inv = 1.0f / sum;
    for (int k = 0; k < num_taps; k++) taps[k] *= inv;
}

#endif /* FIR_REF_H */
