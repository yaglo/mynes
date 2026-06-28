/*
 * Delay Line — CPU Reference Implementation
 * ===========================================
 *
 * y[n] = x[n - D]
 *
 * Pure offset read. Trivially parallel on GPU. Used for:
 *   - Comb filter (1H delay = 1 scanline of samples)
 *   - Multipath ghosting (delayed copy at reduced amplitude)
 *   - Echo / reflection in cables
 *   - 1H delay-line V-averaging in PAL decoder
 */

#ifndef DELAY_REF_H
#define DELAY_REF_H

#include <stdlib.h>
#include <string.h>

/* Simple delay: shift the buffer by D samples (zero-fill the gap).
 * In-place via scratch. */
static inline void delay_ref(float *data, int count, int delay_samples) {
    if (delay_samples <= 0 || delay_samples >= count) return;
    float scratch[8192];
    float *tmp = (count <= 8192) ? scratch : (float *)malloc(count * sizeof(float));
    memcpy(tmp, data, count * sizeof(float));
    memset(data, 0, delay_samples * sizeof(float));
    memcpy(data + delay_samples, tmp, (count - delay_samples) * sizeof(float));
    if (tmp != scratch) free(tmp);
}

/* Ghosting: add a delayed, attenuated copy of the signal to itself.
 * y[n] = x[n] + ghost_level · x[n - D]
 * Models impedance-mismatch reflections in cables. */
static inline void delay_ghost_ref(float *data, int count,
                                    int delay_samples,
                                    float ghost_level) {
    if (ghost_level < 1e-6f || delay_samples <= 0 || delay_samples >= count) return;
    /* Must read from a copy to avoid feeding back. */
    float scratch[8192];
    float *orig = (count <= 8192) ? scratch : (float *)malloc(count * sizeof(float));
    memcpy(orig, data, count * sizeof(float));
    for (int n = delay_samples; n < count; n++) {
        data[n] += ghost_level * orig[n - delay_samples];
    }
    if (orig != scratch) free(orig);
}

/* Comb filter (additive): y[n] = x[n] + x[n - D]
 * For Y/C separation: Y = (line + prev_line) / 2 (chroma cancels).
 * prev_line points to the PREVIOUS scanline's samples. */
static inline void delay_comb_add_ref(float *output,
                                       const float *current_line,
                                       const float *prev_line,
                                       int count,
                                       float scale) {
    for (int n = 0; n < count; n++) {
        output[n] = (current_line[n] + prev_line[n]) * scale;
    }
}

/* Comb filter (subtractive): y[n] = x[n] - x[n - D]
 * For Y/C separation: C = (line - prev_line) / 2 (luma cancels). */
static inline void delay_comb_sub_ref(float *output,
                                       const float *current_line,
                                       const float *prev_line,
                                       int count,
                                       float scale) {
    for (int n = 0; n < count; n++) {
        output[n] = (current_line[n] - prev_line[n]) * scale;
    }
}

#endif /* DELAY_REF_H */
