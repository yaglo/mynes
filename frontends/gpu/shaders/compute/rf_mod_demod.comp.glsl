/* Equivalent-baseband negative-AM envelope detector with complex channel
 * noise. The separate video FIR approximates the combined channel response.
 * No explicit RF carrier sampling, VSB/IF asymmetry or intercarrier sound. */
#version 450

layout(local_size_x = 256) in;

layout(set = 1, binding = 0) buffer CompositeInOut { float composite[]; };

layout(set = 2, binding = 0) uniform Params {
    uint  count;              /* total samples in buffer */
    uint  samples_per_line;   /* 2048 for NTSC, 2560 for PAL */
    float noise_amplitude;    /* per-axis complex Gaussian noise RMS / carrier */
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

    // Sync tip is unit carrier and reference white 12.5%. Map the NES's
    // nonstandard sync/white voltage span without changing its black level.
    const float gain=.875/(1.0+264.0/788.0), bias=.125+gain;
    float carrier=max(bias-gain*signal,0.0);
    uint seed=tid ^ (frame_seed*0x9e3779b9u);
    float radius=sqrt(-2.0*log(max(prng(seed),0.00000006)))*noise_amplitude;
    float angle=6.28318530718*prng(seed ^ 0x68bc21ebu);
    vec2 noisy=vec2(carrier,0)+radius*vec2(cos(angle),sin(angle));
    signal=(bias-length(noisy))/gain;

    uint line = tid / samples_per_line;
    uint sample_in_line = tid % samples_per_line;
    float time_sample = float(line * full_line_samples + sample_in_line);
    float phase = hum_phase + time_sample * (6.28318530718 * hum_hz / sample_rate);
    signal += hum_amplitude * sin(phase);

    /* RF bandwidth limiting is handled by a separate FIR stage
     * (RF Bandwidth FIR) in the signal chain, not here. */

    composite[tid] = signal;
}
