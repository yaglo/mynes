/*
 * Delay Line — GPU Compute Shader
 * =================================
 *
 * Multiple modes:
 *   0: Pure delay          y[n] = x[n - D]
 *   1: Ghost (additive)    y[n] = x[n] + level · x[n - D]
 *   2: Comb add            y[n] = (curr[n] + prev[n]) · scale
 *   3: Comb subtract       y[n] = (curr[n] - prev[n]) · scale
 *
 * All modes are trivially parallel — one thread per sample.
 */

#version 450

layout(local_size_x = 256) in;

layout(set = 0, binding = 0) readonly buffer InputBuf  { float data_in[];  };
layout(set = 0, binding = 1) readonly buffer PrevBuf   { float data_prev[]; };
layout(set = 1, binding = 0) writeonly buffer OutputBuf { float data_out[]; };

layout(set = 2, binding = 0) uniform Params {
    uint  count;         /* number of samples */
    uint  mode;          /* 0=delay, 1=ghost, 2=comb_add, 3=comb_sub */
    int   delay_samples; /* delay in samples (modes 0,1) */
    float level;         /* ghost amplitude (mode 1) or scale (modes 2,3) */
};

void main() {
    uint tid = gl_GlobalInvocationID.x;
    if (tid >= count) return;

    float y;

    switch (mode) {
        case 0u: /* pure delay */
        {
            int src = int(tid) - delay_samples;
            y = (src >= 0) ? data_in[src] : 0.0;
            break;
        }

        case 1u: /* ghost: original + delayed attenuated copy */
        {
            float x = data_in[tid];
            int src = int(tid) - delay_samples;
            float ghost = (src >= 0) ? data_in[src] : 0.0;
            y = x + level * ghost;
            break;
        }

        case 2u: /* comb add: (curr + prev) * scale */
            y = (data_in[tid] + data_prev[tid]) * level;
            break;

        case 3u: /* comb subtract: (curr - prev) * scale */
            y = (data_in[tid] - data_prev[tid]) * level;
            break;

        default:
            y = data_in[tid];
            break;
    }

    data_out[tid] = y;
}
