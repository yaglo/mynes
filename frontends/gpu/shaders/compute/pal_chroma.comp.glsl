/*
 * PAL Chroma Correction — GPU Compute Shader
 * ============================================
 *
 * Mirrors the PAL decoder model used in src/nes/composite.h:
 *
 *   1. Odd-line U compensation
 *      With demod_rotate = +3 (90 degrees), the raw sine-demod output
 *      flips sign on alternate lines under PAL's encoder-side V-phase
 *      alternation. Flip it back here so both fields land on a stable U.
 *
 *   2. 1H delay-line V averaging
 *      PAL's defining decoder trait: average the current V line with the
 *      previous line's V. This cancels small hue errors and gives PAL its
 *      characteristic vertically-soft chroma while leaving luma sharp.
 *
 * Input:
 *   v_raw[] = PAL V channel after chroma FIR
 *   u_raw[] = PAL U channel after chroma FIR
 *
 * Output:
 *   v_out[] = 1H-averaged V
 *   u_out[] = odd-line compensated U
 */

#version 450

layout(local_size_x = 256) in;

layout(set = 0, binding = 0) readonly buffer VRawBuf { float v_raw[]; };
layout(set = 0, binding = 1) readonly buffer URawBuf { float u_raw[]; };

layout(set = 1, binding = 0) writeonly buffer VOutBuf { float v_out[]; };
layout(set = 1, binding = 1) writeonly buffer UOutBuf { float u_out[]; };

layout(set = 2, binding = 0) uniform Params {
    uint count;
    uint samples_per_line;
};

void main() {
    uint tid = gl_GlobalInvocationID.x;
    if (tid >= count) return;

    float v = v_raw[tid];
    float u = u_raw[tid];

    if (samples_per_line > 0u) {
        uint line = tid / samples_per_line;

        /* PAL-CRT / composite.h parity compensation: odd lines invert U. */
        if ((line & 1u) != 0u) {
            u = -u;
        }

        /* First line has no delay-line history, so pass V through. */
        if (line > 0u) {
            v = 0.5 * (v + v_raw[tid - samples_per_line]);
        }
    }

    v_out[tid] = v;
    u_out[tid] = u;
}
