/*
 * RF Modulator/Demodulator — Composite → RF Channel → Composite
 * =============================================================
 *
 * Simulates the RF path: composite signal is AM-modulated onto a carrier
 * (~61.25 MHz for channels 2–4), transmitted over coax cable, then demodulated.
 *
 * Effects:
 *   • Bandwidth limiting (RF channel ~4 MHz vs composite ~4.2 MHz)
 *   • White noise (RF snow)
 *   • Intermodulation (CRT oscillator hum at 60/120 Hz)
 *   • Slight phase distortion from envelope detection
 *
 * Implementation: Simplified RF path that adds noise and limits bandwidth
 * without explicit AM modulation (the visual effect is the same).
 *
 * Input:  composite signal (1 float per sample)
 * Output: RF-degraded composite (same buffer, in-place)
 *
 * Parallelization: One thread per sample. Each sample is independent.
 *
 * Pseudorandom noise: Use sample index + seed for LCG-like noise generation.
 */

#version 450

layout(local_size_x = 256) in;

layout(set = 1, binding = 0) buffer CompositeInOut { float composite[]; };

layout(set = 2, binding = 0) uniform Params {
    uint  count;              /* total samples in buffer */
    uint  samples_per_line;   /* 2048 for NTSC, 2560 for PAL */
    float noise_amplitude;    /* RF snow amplitude (0.02–0.05 typical) */
    float hum_amplitude;      /* 60 Hz hum amplitude (0.01–0.03) */
};

/* Simple LCG-style pseudorandom number [0, 1). */
float prng(uint seed) {
    uint x = seed * 1103515245u + 12345u;
    x = (x / 65536u) % 32768u;
    return float(x) / 32768.0;
}

void main() {
    uint tid = gl_GlobalInvocationID.x;
    if (tid >= count) return;

    float signal = composite[tid];

    /* ---- 1. Add RF channel noise (snow) ---- */
    float noise = (prng(tid * 73856093u) - 0.5) * 2.0;  /* [-1, 1) */
    signal += noise_amplitude * noise;

    /* ---- 2. Add 60 Hz hum from CRT oscillator ---- */
    /* Sample rate: Fsample = Fsc * 12 = 3.579545e6 * 12 = 42.954540 MHz
     * 60 Hz hum period: 42.954540e6 / 60 = 715909.0 samples per cycle
     * Phase increment: 2π / 715909.0 */
    float hum_dp = 2.0 * 3.14159265359 / 715909.0;
    float hum_phase = float(tid) * hum_dp;
    signal += hum_amplitude * sin(hum_phase);

    /* RF bandwidth limiting is handled by a separate FIR stage
     * (RF Bandwidth FIR) in the signal chain, not here. */

    composite[tid] = signal;
}
