/*
 * test_signals.h -- Synthetic test signal generator for GPU kernel verification
 *
 * Header-only. Generates standard test patterns as composite waveforms
 * that can be uploaded directly to the GPU video chain for debugging.
 */
#ifndef TEST_SIGNALS_H
#define TEST_SIGNALS_H

#include <math.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* Generate NTSC color bars test pattern as composite waveform.
 * Output: float buffer, samples_per_line * lines samples.
 * Standard SMPTE color bars: white, yellow, cyan, green, magenta, red, blue, black.
 *
 * Each bar is encoded as Y + chroma modulated at the subcarrier frequency.
 * subcarrier_phase_per_sample = 2*pi*Fsc/Fsample. */
static inline void test_signal_color_bars(float *waveform, int samples_per_line,
                                           int lines,
                                           float subcarrier_phase_per_sample) {
    /* 75% NTSC color bars: luma (Y) and chroma (amplitude, phase in degrees).
     * White, Yellow, Cyan, Green, Magenta, Red, Blue, Black.
     * Phase angles are relative to burst (0 deg = B-Y axis). */
    static const float bars_y[]     = { 0.770f, 0.563f, 0.421f, 0.214f,
                                         0.556f, 0.349f, 0.207f, 0.000f };
    static const float bars_c[]     = { 0.000f, 0.443f, 0.443f, 0.443f,
                                         0.443f, 0.443f, 0.443f, 0.000f };
    static const float bars_phase[] = { 0.0f, 167.1f, 283.5f, 240.7f,
                                         60.7f, 103.5f, 347.1f, 0.0f };
    const int num_bars = 8;
    const float deg2rad = (float)M_PI / 180.0f;

    for (int y = 0; y < lines; y++) {
        float *row = &waveform[y * samples_per_line];
        for (int x = 0; x < samples_per_line; x++) {
            int bar = (x * num_bars) / samples_per_line;
            if (bar >= num_bars) bar = num_bars - 1;

            float sc_phase = subcarrier_phase_per_sample * (float)x;
            float luma = bars_y[bar];
            float chroma = bars_c[bar]
                         * sinf(sc_phase + bars_phase[bar] * deg2rad);

            /* Composite signal: black pedestal + luma + chroma */
            row[x] = 0.075f + luma * 0.925f + chroma * 0.5f;
        }
    }
}

/* Generate a single-frequency sine wave for testing FIR/RC filters.
 * freq_normalized: frequency as fraction of sample rate (0..0.5) */
static inline void test_signal_sine(float *waveform, int total_samples,
                                     float freq_normalized, float amplitude) {
    float omega = 2.0f * (float)M_PI * freq_normalized;
    for (int i = 0; i < total_samples; i++) {
        waveform[i] = amplitude * sinf(omega * (float)i);
    }
}

/* Generate a frequency sweep (chirp) from f0 to f1.
 * Useful for verifying filter frequency response.
 * Linear sweep: instantaneous frequency increases linearly with sample index. */
static inline void test_signal_sweep(float *waveform, int total_samples,
                                      float f0_normalized, float f1_normalized,
                                      float amplitude) {
    /* Phase integral of linear chirp: phi(t) = 2*pi*(f0*t + (f1-f0)*t^2/(2*N)) */
    float n = (float)total_samples;
    for (int i = 0; i < total_samples; i++) {
        float t = (float)i;
        float phase = 2.0f * (float)M_PI * (f0_normalized * t
                      + (f1_normalized - f0_normalized) * t * t / (2.0f * n));
        waveform[i] = amplitude * sinf(phase);
    }
}

/* Generate an impulse (single sample = 1.0, rest = 0.0).
 * Useful for measuring filter impulse response. */
static inline void test_signal_impulse(float *waveform, int total_samples,
                                        int impulse_pos) {
    memset(waveform, 0, (size_t)total_samples * sizeof(float));
    if (impulse_pos >= 0 && impulse_pos < total_samples) {
        waveform[impulse_pos] = 1.0f;
    }
}

/* Generate a step function (0.0 before pos, 1.0 after).
 * Useful for measuring filter step response / settling time. */
static inline void test_signal_step(float *waveform, int total_samples,
                                     int step_pos) {
    for (int i = 0; i < total_samples; i++) {
        waveform[i] = (i >= step_pos) ? 1.0f : 0.0f;
    }
}

#endif /* TEST_SIGNALS_H */
