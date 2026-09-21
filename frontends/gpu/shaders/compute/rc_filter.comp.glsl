/*
 * RC Filter — GPU Compute Shader (Sequential Per-Scanline)
 * ==========================================================
 *
 * First-order IIR lowpass/highpass: y[n] = a·y[n-1] + b·x[n]
 *
 * Each thread processes one complete scanline sequentially (2048 samples).
 * With 240 scanlines dispatched in parallel, this fully utilizes the GPU
 * while avoiding the complexity and artifacts of parallel prefix scan.
 *
 * Warm-start: y[-1] = x[0] (assumes the filter was settled before the
 * scanline began). This eliminates the startup ramp artifact.
 *
 * Workgroup: 256 threads. Dispatch: ceil(num_lines / 256) workgroups.
 * In-place: reads and writes the same buffer.
 */

#version 450

layout(local_size_x = 256) in;

layout(set = 1, binding = 0) buffer DataBuf {
    float data[];
};

/* Carry buffer not used in sequential mode but kept for API compatibility. */
layout(set = 1, binding = 1) buffer CarryBuf {
    vec2 carry[];
};

layout(set = 2, binding = 0) uniform Params {
    float a;             /* IIR coefficient (alpha = exp(-2pi·fc/fs)) */
    float b;             /* 1 - a */
    uint  total_count;   /* total samples in buffer */
    uint  block_offset;  /* not used in sequential mode */
    uint  samples_per_line; /* samples per scanline (2048 for NTSC) */
    uint  num_lines;     /* number of scanlines (240) */
    float nonlinear_tau_samples;
    float pad;
};

void main() {
    uint line = gl_GlobalInvocationID.x;
    if (line >= num_lines) return;

    uint start = line * samples_per_line;
    uint end = start + samples_per_line;
    if (end > total_count) end = total_count;
    if (start >= total_count) return;

    /* Warm-start: assume filter was settled at x[0]. */
    float y_prev = data[start];
    float source_prev=y_prev;

    for (uint i = start; i < end; i++) {
        float x = data[i];
        if(nonlinear_tau_samples>0.0) {
            // Voltage-dependent output impedance, NESdev 2C02G estimate.
            // Convert blank-relative units back to absolute 0..1.1 V.
            float ratio=max((x*788.0+312.0)/1100.0,0.0);
            float input_weight=1.0/(1.0+ratio*nonlinear_tau_samples);
            source_prev=mix(source_prev,x,input_weight);
            x=source_prev;
        }
        float y = a * y_prev + b * x;
        data[i] = y;
        y_prev = y;
    }
}
