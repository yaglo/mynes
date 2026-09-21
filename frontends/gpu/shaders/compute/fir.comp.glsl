/*
 * FIR Convolution — GPU Compute Shader
 * ======================================
 *
 * y[n] = Σ h[k] · x[n-k]  for k = 0..tap_count-1
 *
 * Each output sample is an independent dot product — trivially parallel.
 * One thread per output sample. Mirror boundary at frame/line edges.
 *
 * Note: beam-edge fade/blanking is done on the CPU in
 * waveform_apply_beam_edges (tv.beam_edge_fade / beam_edge_overshoot)
 * rather than as a zero-pad here — the FIR's per-line mirror is what
 * prevents a kernel-sized dark stripe on every scanline's left and
 * right edges that interacts with the comb filter to produce visible
 * horizontal banding.
 *
 * Supports decimation: output[n] = FIR(input[n * decimation_ratio]).
 */

#version 450

layout(local_size_x = 256) in;

layout(set = 0, binding = 0) readonly buffer InputBuf  { float data_in[];  };
layout(set = 0, binding = 1) readonly buffer TapsBuf   { float taps[];     };
layout(set = 1, binding = 0) writeonly buffer OutputBuf { float data_out[]; };

layout(set = 2, binding = 0) uniform Params {
    uint input_count;        /* number of input samples */
    uint output_count;       /* number of output samples */
    uint tap_count;          /* number of FIR taps (odd) */
    uint decimation_ratio;   /* 1 = no decimation */
    uint samples_per_line;   /* 0 = no per-line boundary, >0 = clamp per scanline */
};

/* Reuse overlapping input windows for the video filters. Keep the direct
 * path for longer filters and decimation; neither needs a fixed tap limit. */
shared float tile[320]; // 256 outputs plus a halo for up to 65 taps

void main() {
    uint tid = gl_GlobalInvocationID.x;
    int halfN = int(tap_count) / 2;
    int first = int(gl_WorkGroupID.x * 256u) - halfN;
    bool tiled = decimation_ratio == 1u && tap_count > 0u && tap_count <= 65u;
    if (tiled) {
        for (uint k = gl_LocalInvocationID.x; k < 256u + 2u * uint(halfN); k += 256u)
            tile[k] = data_in[clamp(first + int(k), 0, int(input_count) - 1)];
        /* Inactive lanes in a partial workgroup must also fill the halo. */
        barrier();
    }
    if (tid >= output_count) return;

    int base = int(tid * decimation_ratio);
    float sum = 0.0;
    int line_start = samples_per_line > 0u
        ? (base / int(samples_per_line)) * int(samples_per_line) : 0;
    int line_end = samples_per_line > 0u
        ? line_start + int(samples_per_line) : int(input_count);

    /* Most samples need no edge reflection. Preserve the original tap order
     * and arithmetic, including for asymmetric and even-length filters. */
    if (base - halfN >= line_start && base - halfN + int(tap_count) <= line_end) {
        if (tiled) {
            for (uint k = 0; k < tap_count; k++)
                sum += taps[k] * tile[base - halfN + int(k) - first];
        } else {
            for (uint k = 0; k < tap_count; k++)
                sum += taps[k] * data_in[base - halfN + int(k)];
        }
        data_out[tid] = sum;
        return;
    }

    if (samples_per_line > 0u) {
        /* Per-scanline FIR: mirror boundary at scanline edges. Mirroring
         * keeps the local content at the line boundary so the FIR's
         * sum stays smooth, preventing banding artifacts that show up
         * when the comb filter compares adjacent lines. */
        for (uint k = 0; k < tap_count; k++) {
            int idx = base - halfN + int(k);
            if (idx < line_start) idx = 2 * line_start - idx;
            if (idx >= line_end)  idx = 2 * line_end - idx - 1;
            idx = clamp(idx, line_start, line_end - 1);
            sum += taps[k] * (tiled ? tile[idx - first] : data_in[idx]);
        }
    } else {
        /* Global FIR: clamp at frame boundaries (original behavior). */
        for (uint k = 0; k < tap_count; k++) {
            int idx = base - halfN + int(k);
            if (idx < 0) idx = -idx;
            if (idx >= int(input_count)) idx = 2 * int(input_count) - idx - 1;
            idx = clamp(idx, 0, int(input_count) - 1);
            sum += taps[k] * (tiled ? tile[idx - first] : data_in[idx]);
        }
    }

    data_out[tid] = sum;
}
