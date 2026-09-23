/* Burst-referenced quadrature detector. Multiplication by 2 restores the
 * amplitude lost by averaging cos^2/sin^2 in the following lowpass FIRs. */
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
    float burst_reference;  /* burst amplitude the ACC holds, blanking to white = 1 */
};

#define TWO_PI 6.28318530718

void main() {
    uint tid = gl_GlobalInvocationID.x;
    if (tid >= count) return;

    float x = data_in[tid];
    float burst_gain = 1.0;

    /* Compute phase: if samples_per_line > 0, reset phase per scanline
     * to match the waveform generator's per-scanline phase offset. */
    float p;
    if (samples_per_line > 0u) {
        uint scanline = tid / samples_per_line;
        uint sample_in_line = tid % samples_per_line;
        p = phase + reference[scanline].x + (float(sample_in_line) - active_offset) * dp;
        x -= reference[scanline].y;
        // Automatic chroma control holds the burst at the standard amplitude
        // and scales the chroma with it, so cable and receiver attenuation
        // cancel. The reference is the standard's burst, not the console's:
        // the 2C02's burst is a square wave of 47.7 IRE peak to peak with a
        // 60.7 IRE fundamental, against the 40 IRE the receiver expects, so
        // a receiver shows the console at two thirds of the chroma a decoder
        // normalised to the console's own burst would give.
        float received = reference[scanline].z;
        burst_gain = received > 0.01 ? clamp(burst_reference / received, 0.25, 4.0) : 0.0;
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
            float gain = 2.0 * param_a * burst_gain;
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
