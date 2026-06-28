/*
 * Matrix Decode (Y + 2 chroma axes -> RGB) — GPU Compute Shader
 * =================================================
 *
 * Stage 9: Converts separated Y plus two decoded chroma axes into RGB.
 * Each output pixel is an independent 3×3 matrix multiply:
 *
 *   R = m[0][0]·Y + m[0][1]·C1 + m[0][2]·C2 + bias[0]
 *   G = m[1][0]·Y + m[1][1]·C1 + m[1][2]·C2 + bias[1]
 *   B = m[2][0]·Y + m[2][1]·C1 + m[2][2]·C2 + bias[2]
 *
 * The matrix coefficients fold in: decode matrix, saturation boost,
 * warm phosphor tint, black level, and ×1.0 scale (output is float,
 * not uint8). The caller precomputes the folded matrix from:
 *   - NTSC YIQ->RGB or PAL YUV->RGB coefficients
 *   - Color temperature (warm_r, warm_g, warm_b multipliers)
 *   - Per-gun drive/cutoff adjustments
 *
 * Trivially parallel: one thread per sample.
 *
 * Input:  3 float buffers (Y, chroma 1, chroma 2) at signal resolution
 * Output: 3 float buffers (R, G, B) at signal resolution
 *         (interleaved as R,G,B,R,G,B,... for efficient texture upload)
 */

#version 450

layout(local_size_x = 256) in;

layout(set = 0, binding = 0) readonly buffer YBuf { float y_in[]; };
layout(set = 0, binding = 1) readonly buffer IBuf { float i_in[]; };
layout(set = 0, binding = 2) readonly buffer QBuf { float q_in[]; };
layout(set = 1, binding = 0) writeonly buffer RGBBuf { float rgb_out[]; };

layout(set = 2, binding = 0) uniform Params {
    uint  count;           /* total samples */
    /* 3×3 decode matrix (row-major) */
    float m00, m01, m02;   /* R row */
    float m10, m11, m12;   /* G row */
    float m20, m21, m22;   /* B row */
    /* Per-channel bias (black level + warm tint folded in) */
    float bias_r, bias_g, bias_b;
    /* Colour killer: zero chroma when luma < threshold.
     * Real TVs disable chroma decoder on dark signals to suppress
     * colour noise. 0 = disabled, typical threshold ~0.05-0.10. */
    float color_killer_threshold;
    /* Chroma delay: shift I/Q relative to Y by this many samples.
     * Real TVs had chroma FIR group delay > luma FIR group delay,
     * causing color to trail brightness on horizontal transitions.
     * Typical: 2-6 samples at signal resolution. 0 = no delay. */
    int   chroma_delay;
    uint  samples_per_line;  /* for per-scanline boundary clamping */
};

void main() {
    uint tid = gl_GlobalInvocationID.x;
    if (tid >= count) return;

    float Y = y_in[tid];

    /* Read I/Q from a delayed position (color trails behind brightness). */
    int delayed_tid = int(tid);
    if (chroma_delay != 0 && samples_per_line > 0u) {
        uint line_start = (tid / samples_per_line) * samples_per_line;
        uint line_end = line_start + samples_per_line;
        delayed_tid = clamp(int(tid) - chroma_delay,
                            int(line_start), int(line_end) - 1);
    }
    float I = i_in[delayed_tid];
    float Q = q_in[delayed_tid];

    /* Colour killer: suppress chroma below threshold to prevent
     * colour noise in dark areas. Smooth transition over small range. */
    if (color_killer_threshold > 0.0) {
        float kill = smoothstep(color_killer_threshold * 0.7,
                                color_killer_threshold * 1.3, Y);
        I *= kill;
        Q *= kill;
    }

    float R = m00 * Y + m01 * I + m02 * Q + bias_r;
    float G = m10 * Y + m11 * I + m12 * Q + bias_g;
    float B = m20 * Y + m21 * I + m22 * Q + bias_b;

    /* Clamp to [0, 1] for SDR. HDR path would skip this. */
    R = clamp(R, 0.0, 1.0);
    G = clamp(G, 0.0, 1.0);
    B = clamp(B, 0.0, 1.0);

    /* Interleaved RGB output. */
    rgb_out[tid * 3 + 0] = R;
    rgb_out[tid * 3 + 1] = G;
    rgb_out[tid * 3 + 2] = B;
}
