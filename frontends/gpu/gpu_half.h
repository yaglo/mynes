/*
 * gpu_half.h -- IEEE 754 half-precision → single-precision conversion
 *
 * SDL3 exposes SDL_HalfToFloat in newer headers but the vendored SDL
 * snapshot we build against doesn't, so this header is the single
 * source of truth. All GPU-side readbacks of RGBA16F beam / waveform
 * data should go through gpu_half_to_float().
 *
 * The conversion is branch-light, handles zero / denormal / NaN / inf
 * correctly, and matches hardware behaviour bit-for-bit for all 2^16
 * half-float inputs (see tests/test_gpu_half.c).
 */
#ifndef GPU_HALF_H
#define GPU_HALF_H

#include <stdint.h>
#include <string.h>

static inline float gpu_half_to_float(uint16_t h) {
    uint32_t sign     = (uint32_t)(h & 0x8000u) << 16;
    uint32_t exp      = (h >> 10) & 0x1Fu;
    uint32_t mantissa = h & 0x03FFu;

    uint32_t bits;
    if (exp == 0u) {
        if (mantissa == 0u) {
            /* ±0 */
            bits = sign;
        } else {
            /* Subnormal: normalize by shifting mantissa into bit 10 and
             * decrementing the exponent accordingly. */
            uint32_t e = 127u - 14u;
            while ((mantissa & 0x0400u) == 0u) {
                mantissa <<= 1;
                e--;
            }
            mantissa &= 0x03FFu;
            bits = sign | (e << 23) | (mantissa << 13);
        }
    } else if (exp == 0x1Fu) {
        /* ±Inf or NaN — propagate mantissa into the float32 payload. */
        bits = sign | (0xFFu << 23) | (mantissa << 13);
    } else {
        /* Normalized: rebias exponent (127-15) and widen mantissa. */
        bits = sign | ((exp + (127u - 15u)) << 23) | (mantissa << 13);
    }

    float out;
    memcpy(&out, &bits, sizeof(out));
    return out;
}

#endif /* GPU_HALF_H */
