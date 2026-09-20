/* Baseband approximation of receiver noise and mains pickup. The separate
 * RF FIR supplies bandwidth loss; this does not simulate a tuner or AM/VSB. */
#version 450

layout(local_size_x = 256) in;

layout(set = 1, binding = 0) buffer CompositeInOut { float composite[]; };

layout(set = 2, binding = 0) uniform Params {
    uint  count;              /* total samples in buffer */
    uint  samples_per_line;   /* 2048 for NTSC, 2560 for PAL */
    float noise_amplitude;    /* RF snow amplitude (0.02–0.05 typical) */
    float hum_amplitude;
    uint frame_seed;
    uint full_line_samples;
    float sample_rate;
    float hum_phase;
    float hum_hz;
};

/* Integer avalanche hash: independent noise for each sample and frame. */
float prng(uint x) {
    x ^= x >> 16; x *= 0x7feb352du;
    x ^= x >> 15; x *= 0x846ca68bu;
    x ^= x >> 16;
    return float(x >> 8) * (1.0 / 16777216.0);
}

void main() {
    uint tid = gl_GlobalInvocationID.x;
    if (tid >= count) return;

    float signal = composite[tid];

    /* ---- 1. Add RF channel noise (snow) ---- */
    float noise = (prng(tid ^ (frame_seed * 0x9e3779b9u)) - 0.5) * 2.0;  /* [-1, 1) */
    signal += noise_amplitude * noise;

    uint line = tid / samples_per_line;
    uint sample_in_line = tid % samples_per_line;
    float time_sample = float(line * full_line_samples + sample_in_line);
    float phase = hum_phase + time_sample * (6.28318530718 * hum_hz / sample_rate);
    signal += hum_amplitude * sin(phase);

    /* RF bandwidth limiting is handled by a separate FIR stage
     * (RF Bandwidth FIR) in the signal chain, not here. */

    composite[tid] = signal;
}
