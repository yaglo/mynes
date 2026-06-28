/*
 * Video Amplifier — Per-Channel RGB Bandwidth Limiter
 * =====================================================
 *
 * Stage 10: Applies independent horizontal FIR lowpass to each R, G, B
 * channel in the interleaved RGB buffer. Simulates the TV's video
 * amplifier driving the electron guns — each gun has limited bandwidth
 * that softens horizontal color transitions independently.
 *
 * PVM: ~6 MHz per channel (sharp)
 * Consumer: ~3-4 MHz (soft colors)
 * RF: ~2-3 MHz (very soft)
 *
 * Input:  interleaved RGB floats (R,G,B,R,G,B,...) at signal resolution
 * Output: interleaved RGB floats with per-channel lowpass applied
 *
 * The shader reads from one buffer and writes to another (not in-place)
 * to avoid read-after-write hazards. The caller swaps the buffers.
 *
 * Per-scanline boundary clamping prevents color bleeding across lines.
 *
 * Dispatch: one thread per pixel (total_pixels threads).
 */

#version 450

layout(local_size_x = 256) in;

layout(set = 0, binding = 0) readonly buffer RGBIn {
    float rgb_in[];   /* interleaved R,G,B,R,G,B,... */
};

layout(set = 1, binding = 0) writeonly buffer RGBOut {
    float rgb_out[];  /* interleaved R,G,B,R,G,B,... */
};

layout(set = 2, binding = 0) uniform Params {
    uint  total_pixels;       /* samples_per_line * 240 */
    uint  samples_per_line;   /* 2048 NTSC */
    uint  tap_count;          /* number of FIR taps (odd, max 9) */
    float taps[12];           /* FIR coefficients (padded to vec4 alignment) */
    /* §4.1 Velocity modulation: offset sample position by local
     * luminance derivative. Dark→bright beam decelerates (edge
     * widens); bright→dark accelerates (edge sharpens). Zero = off. */
    float velocity_mod;
    /* §5.7 Asymmetric rise/fall. Real video amps have different time
     * constants going up vs down; modeled as an extra signed
     * correction proportional to the local derivative. Positive =
     * stronger rise softening, 0 = symmetric (classical FIR). */
    float asym_rise_fall;
    /* §5.7 Vertical smearing — scanline-to-scanline feedback from
     * amplifier parasitic capacitance. Tiny amount of the previous
     * line's value bleeds into this line's start. 0 = none. */
    float vertical_smear;
};

void main() {
    uint tid = gl_GlobalInvocationID.x;
    if (tid >= total_pixels) return;

    int halfN = int(tap_count) / 2;
    int spl = int(samples_per_line);
    int base = int(tid);

    /* Per-scanline boundary clamping. */
    int line_start = (base / spl) * spl;
    int line_end   = line_start + spl;

    /* Apply FIR independently to each channel. */
    float sum_r = 0.0, sum_g = 0.0, sum_b = 0.0;
    for (uint k = 0; k < tap_count; k++) {
        int idx = base - halfN + int(k);
        /* Mirror at scanline boundaries. */
        if (idx < line_start) idx = 2 * line_start - idx;
        if (idx >= line_end)  idx = 2 * line_end - idx - 2;
        idx = clamp(idx, line_start, line_end - 1);

        uint src = uint(idx) * 3;
        float h = taps[k];
        sum_r += h * rgb_in[src + 0];
        sum_g += h * rgb_in[src + 1];
        sum_b += h * rgb_in[src + 2];
    }

    /* §4.1 Velocity modulation + §5.7 asymmetric rise/fall — both
     * operate on the local luminance derivative, so compute it once
     * via centered differences of the neighbors already sampled. */
    if (velocity_mod > 0.001 || asym_rise_fall > 0.001) {
        int ip = clamp(base - 1, line_start, line_end - 1);
        int in_ = clamp(base + 1, line_start, line_end - 1);
        float lp = rgb_in[ip * 3 + 0] * 0.299
                 + rgb_in[ip * 3 + 1] * 0.587
                 + rgb_in[ip * 3 + 2] * 0.114;
        float ln = rgb_in[in_ * 3 + 0] * 0.299
                 + rgb_in[in_ * 3 + 1] * 0.587
                 + rgb_in[in_ * 3 + 2] * 0.114;
        float dL = (ln - lp) * 0.5;
        /* Velocity modulation: bright→dark (dL<0) ⇒ beam accelerates,
         * narrows the transition; visually equivalent to adding a
         * derivative-proportional lift. */
        vec3 vm = vec3(dL) * velocity_mod;
        sum_r += vm.r; sum_g += vm.g; sum_b += vm.b;
        /* Asymmetric amp: rising slopes lag (slower τ), falling
         * slopes pass through or overshoot. Net: subtract a fraction
         * of positive dL (lag), add on negative dL (overshoot). */
        if (asym_rise_fall > 0.001) {
            float rise = max(dL, 0.0) * asym_rise_fall;
            float fall = max(-dL, 0.0) * asym_rise_fall * 0.5;
            sum_r += fall - rise; sum_g += fall - rise; sum_b += fall - rise;
        }
    }

    /* §5.7 Vertical smearing: previous scanline's output bleeds into
     * this line through amplifier parasitic capacitance. Decays
     * fastest at the line's leading edge; approximate with a small
     * constant blend of the previous line's same column. */
    if (vertical_smear > 0.001 && line_start >= spl) {
        int prev_col = line_start - spl + (base - line_start);
        sum_r += rgb_in[prev_col * 3 + 0] * vertical_smear * 0.1;
        sum_g += rgb_in[prev_col * 3 + 1] * vertical_smear * 0.1;
        sum_b += rgb_in[prev_col * 3 + 2] * vertical_smear * 0.1;
        sum_r *= (1.0 - vertical_smear * 0.1);
        sum_g *= (1.0 - vertical_smear * 0.1);
        sum_b *= (1.0 - vertical_smear * 0.1);
    }

    uint dst = tid * 3;
    rgb_out[dst + 0] = sum_r;
    rgb_out[dst + 1] = sum_g;
    rgb_out[dst + 2] = sum_b;
}
