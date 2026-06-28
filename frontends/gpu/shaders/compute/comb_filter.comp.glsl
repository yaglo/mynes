/*
 * Comb Filter — Y/C Separator for Composite NTSC Signal
 * =======================================================
 *
 * NTSC composite signal encodes luminance (Y) and chrominance (C) in
 * overlapping frequency bands. A comb filter separates them by exploiting
 * the phase relationship of the color subcarrier (3.579545 MHz) across
 * scanlines: the subcarrier inverts phase every line.
 *
 * By subtracting adjacent scanlines, the low-frequency luma cancels out
 * (same phase), leaving the high-frequency chroma (opposite phase). Then
 * the chroma is subtracted from the original to recover luma.
 *
 * Modes:
 *   0: Bypass      Y = signal, C = 0  (used for Direct/RGB modes)
 *   1: 1-line      C = (signal[n] - signal[n-spl]) / 2
 *                  Y = (signal[n] + signal[n-spl]) / 2
 *   2: 2-line      C = (signal[n] - 2·signal[n-spl] + signal[n-2spl]) / 4
 *                  Y = signal[n] - C
 *   3: 3-line      Weighted 3-line comb (better phase preservation)
 *
 * Input:  composite signal (1 float per sample)
 * Output: Y (luma) and C (chroma) buffers
 *
 * Parallelization: One thread per sample. Y/C are independent once
 * the scanline delay is available. No shared memory, trivially parallel.
 *
 * Edge handling: First scanline(s) have no prior data — zero-pad.
 */

#version 450

layout(local_size_x = 256) in;

layout(set = 0, binding = 0) readonly buffer SignalBuf { float signal_in[]; };
layout(set = 1, binding = 0) writeonly buffer YBuf     { float y_out[];     };
layout(set = 1, binding = 1) writeonly buffer CBuf     { float c_out[];     };

layout(set = 2, binding = 0) uniform Params {
    uint count;              /* total samples in buffer */
    uint samples_per_line;   /* 2048 for NTSC, 2560 for PAL */
    uint mode;               /* 0=bypass, 1=1line, 2=2line, 3=3line */
    float blend;             /* comb strength (0..1, 1.0 = full) */
};

void main() {
    uint tid = gl_GlobalInvocationID.x;
    if (tid >= count) return;

    float signal = signal_in[tid];
    float y, c;

    switch (mode) {
        case 0u: /* Bypass: no comb filtering */
        {
            y = signal;
            c = 0.0;
            break;
        }

        case 1u: /* 1-line comb: Y = average (same between lines), C = difference (inverts) */
        {
            int prev_idx = int(tid) - int(samples_per_line);
            float prev_signal = (prev_idx >= 0) ? signal_in[prev_idx] : signal;

            /* In NTSC, subcarrier inverts 180° each scanline:
             *   signal[n]   = Y + C
             *   signal[n-1] = Y - C  (phase inverted)
             * Perfect comb: Y = (sig + prev)/2, C = (sig - prev)/2
             *
             * blend controls comb effectiveness (notch depth):
             *   blend=1.0: perfect separation (ideal comb, PVM)
             *   blend=0.8: imperfect (cheap TV notch filter, ~20 dB rejection)
             *              residual subcarrier leaks into Y → visible interference
             *   blend=0.0: no separation (Y = raw composite) */
            float avg = (signal + prev_signal) * 0.5;
            float diff = (signal - prev_signal) * 0.5;
            y = mix(signal, avg, blend);
            c = blend * diff;
            break;
        }

        case 2u: /* 2-line comb: uses lines N and N-2 (same phase) */
        {
            int prev1_idx = int(tid) - int(samples_per_line);
            int prev2_idx = int(tid) - 2 * int(samples_per_line);

            float prev1 = (prev1_idx >= 0) ? signal_in[prev1_idx] : signal;
            float prev2 = (prev2_idx >= 0) ? signal_in[prev2_idx] : signal;

            /* Use 1H-delayed line (opposite phase) for Y/C, but weight
             * with the 2H-delayed line for vertical detail preservation.
             * avg12 = average of opposite-phase lines (cancels chroma)
             * Use prev2 to detect vertical edges and reduce comb artifacts. */
            float avg1 = (signal + prev1) * 0.5;       /* 1-line average (chroma cancels) */
            float diff1 = (signal - prev1) * 0.5;      /* 1-line difference (luma cancels) */
            /* Vertical edge detector: if prev2 ≈ signal (same content), comb is safe.
             * If prev2 ≠ signal, there's vertical detail — reduce comb. */
            float vert_diff = abs(signal - prev2);
            float vert_blend = blend * (1.0 - clamp(vert_diff * 4.0, 0.0, 0.5));
            y = mix(signal, avg1, vert_blend);
            c = vert_blend * diff1;
            break;
        }

        case 3u: /* 3-line comb: uses 4 scanlines, best quality */
        {
            int prev1_idx = int(tid) - int(samples_per_line);
            int prev2_idx = int(tid) - 2 * int(samples_per_line);
            int prev3_idx = int(tid) - 3 * int(samples_per_line);

            float prev1 = (prev1_idx >= 0) ? signal_in[prev1_idx] : signal;
            float prev2 = (prev2_idx >= 0) ? signal_in[prev2_idx] : signal;
            float prev3 = (prev3_idx >= 0) ? signal_in[prev3_idx] : signal;

            /* 4-line average: chroma cancels over 2 complete phase cycles.
             * blend controls effectiveness — same as 1-line. */
            float avg4 = (signal + prev1 + prev2 + prev3) * 0.25;
            y = mix(signal, avg4, blend);
            c = blend * (signal - avg4);
            break;
        }

        default:
            y = signal;
            c = 0.0;
            break;
    }

    y_out[tid] = y;
    c_out[tid] = c;
}
