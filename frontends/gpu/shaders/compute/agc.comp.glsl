/*
 * Automatic Gain Control — GPU Compute Shader (Sequential Per-Scanline)
 * ======================================================================
 *
 * Normalizes composite signal amplitude after the RF mod/demod stage.
 * Each thread processes one complete scanline: measures sync-to-porch amplitude,
 * computes a target gain, smooths it with an asymmetric attack/release
 * envelope, and applies it uniformly across the line.
 *
 * Attack/release asymmetry:
 *   - Signal gets louder (gain decreases) → fast attack (quick response)
 *   - Signal gets quieter (gain increases) → slow release (avoids pumping)
 *
 * Frame-to-frame continuity: the carry buffer persists the smoothed gain
 * from the previous frame so the envelope never resets between frames.
 *
 * Workgroup: 256 threads. Dispatch: ceil(num_lines / 256) workgroups.
 * In-place: reads and writes the same composite buffer.
 */

#version 450

layout(local_size_x = 256) in;

/* Composite signal — in-place read/write. */
layout(set = 1, binding = 0) buffer DataBuf {
    float data[];
};

/* Per-scanline carry state across frames.
 * vec2 per line: x = smoothed_gain, y = peak (for debugging). */
layout(set = 1, binding = 1) buffer CarryBuf {
    vec2 carry[];
};

layout(set = 2, binding = 0) uniform Params {
    uint  total_count;      /* total samples in buffer */
    uint  samples_per_line; /* complete 341-dot lines */
    uint  num_lines;        /* number of scanlines (240) */
    float target_level;     /* desired sync-to-porch amplitude */
    float attack_coeff;     /* smoothing for gain decrease (e.g. 0.1) */
    float release_coeff;    /* smoothing for gain increase (e.g. 0.02) */
    float min_gain;         /* floor to prevent over-amplification (e.g. 0.5) */
    float max_gain;         /* ceiling to prevent over-attenuation (e.g. 2.0) */
};

void main() {
    uint line = gl_GlobalInvocationID.x;
    if (line >= num_lines) return;

    uint start = line * samples_per_line;
    uint end   = start + samples_per_line;
    if (end > total_count) end = total_count;
    if (start >= total_count) return;

    // Key the detector to sync and porch; picture content must not pump gain.
    uint spp = samples_per_line / 341u;
    float tip = 0.0, porch = 0.0;
    for (uint x=8u*spp; x<20u*spp; x++) tip += data[start+x];
    for (uint x=46u*spp; x<49u*spp; x++) porch += data[start+x];
    tip /= float(12u*spp);
    porch /= float(3u*spp);
    float peak = max(porch-tip, 0.0);

    /* ---- Pass 2: Compute and smooth gain ---- */
    /* Noise gate: if peak is below noise floor, don't amplify.
     * Just pass through at unity gain to avoid boosting noise. */
    float desired_gain = (peak < 0.02) ? 1.0 : target_level / peak;
    desired_gain = clamp(desired_gain, min_gain, max_gain);

    /* Load previous frame's smoothed gain from carry buffer. */
    float prev_gain = carry[line].x;

    /* First frame: carry is zero-initialized, bootstrap to desired. */
    if (prev_gain <= 0.0) {
        prev_gain = desired_gain;
    }

    /* Asymmetric envelope: fast attack, slow release. */
    float coeff = (desired_gain < prev_gain) ? attack_coeff : release_coeff;
    float smoothed_gain = prev_gain + coeff * (desired_gain - prev_gain);

    /* ---- Pass 3: Apply gain to all samples in the scanline ---- */
    for (uint i = start; i < end; i++) {
        data[i] *= smoothed_gain;
    }

    /* Store state for next frame. */
    carry[line] = vec2(smoothed_gain, peak);
}
