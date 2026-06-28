/*
 * Modulator / Demodulator — CPU Reference Implementation
 * ========================================================
 *
 * y[n] = x[n] · cos(2π · f_carrier · n / f_sample + φ)
 *
 * Point-wise multiply with a generated carrier. Trivially parallel.
 *
 * Used for:
 *   - NTSC chroma modulation (3.579545 MHz subcarrier)
 *   - NTSC chroma demodulation (synchronous detection for I and Q)
 *   - RF modulation (AM envelope onto Ch 3/4 carrier)
 *   - RF demodulation (envelope detection)
 *   - PAL chroma with per-line V-phase inversion
 */

#ifndef MODULATOR_REF_H
#define MODULATOR_REF_H

#include <math.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* Modulate: multiply signal by a cosine carrier.
 * y[n] = x[n] · cos(2π · freq · n / sample_rate + phase)
 * phase is updated in-place for streaming (preserves carrier continuity). */
static inline void modulate_cos_ref(float *data, int count,
                                     float freq, float sample_rate,
                                     float *phase) {
    float dp = 2.0f * (float)M_PI * freq / sample_rate;
    float p = *phase;
    for (int n = 0; n < count; n++) {
        data[n] *= cosf(p);
        p += dp;
    }
    /* Wrap to avoid float precision loss. */
    while (p > 2.0f * (float)M_PI) p -= 2.0f * (float)M_PI;
    *phase = p;
}

/* Modulate with sine carrier (for Q channel demodulation).
 * y[n] = x[n] · sin(2π · freq · n / sample_rate + phase) */
static inline void modulate_sin_ref(float *data, int count,
                                     float freq, float sample_rate,
                                     float *phase) {
    float dp = 2.0f * (float)M_PI * freq / sample_rate;
    float p = *phase;
    for (int n = 0; n < count; n++) {
        data[n] *= sinf(p);
        p += dp;
    }
    while (p > 2.0f * (float)M_PI) p -= 2.0f * (float)M_PI;
    *phase = p;
}

/* AM modulation: y[n] = (1 + m · x[n]) · cos(carrier)
 * m = modulation index (0..1). Used for RF video modulator. */
static inline void modulate_am_ref(float *data, int count,
                                    float carrier_freq, float sample_rate,
                                    float mod_index,
                                    float *phase) {
    float dp = 2.0f * (float)M_PI * carrier_freq / sample_rate;
    float p = *phase;
    for (int n = 0; n < count; n++) {
        data[n] = (1.0f + mod_index * data[n]) * cosf(p);
        p += dp;
    }
    while (p > 2.0f * (float)M_PI) p -= 2.0f * (float)M_PI;
    *phase = p;
}

/* Synchronous demodulator: multiply by reference carrier, then lowpass.
 * This is the I/Q extraction step. Caller runs:
 *   1. Copy signal → I_buf, Q_buf
 *   2. modulate_cos_ref(I_buf, ..., carrier_freq, ...)   → I channel
 *   3. modulate_sin_ref(Q_buf, ..., carrier_freq, ...)   → Q channel
 *   4. fir_convolve_ref(I_buf, ..., chroma_lowpass_taps)  → bandwidth-limited I
 *   5. fir_convolve_ref(Q_buf, ..., chroma_lowpass_taps)  → bandwidth-limited Q
 *
 * The demodulated I and Q are the color-difference signals, scaled by 2×
 * (the factor of 2 comes from the trig identity cos²θ = (1+cos2θ)/2;
 * the lowpass kills the cos2θ term, leaving 1/2 · signal, so multiply
 * by 2 to recover unity gain).
 *
 * This is a helper that does steps 1-3 for both channels in one pass. */
static inline void demodulate_iq_ref(const float *signal, int count,
                                      float *out_i, float *out_q,
                                      float carrier_freq, float sample_rate,
                                      float *phase,
                                      float gain) {
    float dp = 2.0f * (float)M_PI * carrier_freq / sample_rate;
    float p = *phase;
    for (int n = 0; n < count; n++) {
        float s = signal[n] * gain;
        out_i[n] = s * cosf(p);
        out_q[n] = s * sinf(p);
        p += dp;
    }
    while (p > 2.0f * (float)M_PI) p -= 2.0f * (float)M_PI;
    *phase = p;
}

#endif /* MODULATOR_REF_H */
