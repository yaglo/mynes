/*
 * Modulator / Demodulator — GPU Compute Shader
 * ===============================================
 *
 * Modes:
 *   0: Cosine multiply     y[n] = x[n] · cos(phase + n · dp)
 *   1: Sine multiply       y[n] = x[n] · sin(phase + n · dp)
 *   2: AM modulation       y[n] = (1 + mod_index · x[n]) · cos(phase + n · dp)
 *   3: I/Q demod (2 outputs): I[n] = x[n] · gain · cos, Q[n] = x[n] · gain · sin
 *
 * Phase and dp (phase increment per sample) are precomputed from
 * carrier_freq and sample_rate on the CPU:
 *   dp = 2π · carrier_freq / sample_rate
 *
 * Each sample is independent — trivially parallel.
 */

#version 450

layout(local_size_x = 256) in;

layout(set = 0, binding = 0) readonly buffer InputBuf   { float data_in[];  };
layout(set = 0, binding = 1) readonly buffer Reference { vec4 reference[]; };
layout(set = 1, binding = 0) writeonly buffer OutputBuf  { float data_out[]; };
/* Binding 1: second output for I/Q demod mode (Q channel). */
layout(set = 1, binding = 1) writeonly buffer OutputBuf2 { float data_out2[]; };

layout(set = 2, binding = 0) uniform Params {
    uint  count;             /* number of samples */
    uint  mode;              /* 0=cos, 1=sin, 2=AM, 3=IQ */
    float phase;             /* base phase (radians) */
    float dp;                /* phase increment per sample */
    float param_a;           /* mode-dependent: mod_index (mode 2), gain (mode 3) */
    uint  samples_per_line;  /* 0 = continuous (no per-line reset), >0 = reset phase per scanline */
    float active_offset;    /* phase advance per scanline (radians), e.g. 6 * 2π/12 = π */
};

#define TWO_PI 6.28318530718

void main() {
    uint tid = gl_GlobalInvocationID.x;
    if (tid >= count) return;

    float x = data_in[tid];

    /* Compute phase: if samples_per_line > 0, reset phase per scanline
     * to match the waveform generator's per-scanline phase offset. */
    float p;
    if (samples_per_line > 0u) {
        uint scanline = tid / samples_per_line;
        uint sample_in_line = tid % samples_per_line;
        p = phase + reference[scanline].x + (float(sample_in_line) - active_offset) * dp;
        x -= reference[scanline].y;
    } else {
        p = phase + float(tid) * dp;
    }

    switch (mode) {
        case 0u: /* cosine multiply */
            data_out[tid] = x * cos(p);
            break;

        case 1u: /* sine multiply */
            data_out[tid] = x * sin(p);
            break;

        case 2u: /* AM modulation */
        {
            float mod_index = param_a;
            data_out[tid] = (1.0 + mod_index * x) * cos(p);
            break;
        }

        case 3u: /* I/Q demodulation */
        {
            float gain = param_a;
            float s = x * gain;
            data_out[tid]  = s * cos(p);
            data_out2[tid] = s * sin(p);
            break;
        }

        default:
            data_out[tid] = x;
            break;
    }
}
