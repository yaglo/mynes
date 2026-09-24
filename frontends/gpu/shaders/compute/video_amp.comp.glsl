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
 * The receiver's flyback blanking forces the drive to blanking before the
 * output amplifiers: every sample the amplifier reads is weighted by the
 * part of it the unblanked raster covers (decode_window.glsl), so the
 * retrace reads as zero drive and the amplifier band-limits the step at the
 * raster's edge like any other.
 *
 * Dispatch: one thread per decode-window sample (total_pixels threads).
 */

#version 450
#extension GL_GOOGLE_include_directive : require

layout(local_size_x = 256) in;

layout(set = 0, binding = 0) readonly buffer RGBIn {
    float rgb_in[];   /* interleaved R,G,B,R,G,B,... */
};

layout(set = 1, binding = 0) writeonly buffer RGBOut {
    float rgb_out[];  /* interleaved R,G,B,R,G,B,... */
};

layout(set = 2, binding = 0) uniform Params {
    uint  total_pixels;       /* decode window: width * lines */
    uint  samples_per_line;   /* decode window width */
    uint  tap_count;          /* number of FIR taps (odd, max 31) */
    vec4 taps[32];           /* FIR coefficients (padded to vec4 alignment) */
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
    vec4  trace;              /* unblanked samples (xy) and rows (zw) */
};

#include "decode_window.glsl"

float drive(int idx, int line_start, uint row, uint channel) {
    return rgb_in[uint(idx) * 3u + channel] * trace_gate(float(idx - line_start), row, trace);
}

void main() {
    uint tid = gl_GlobalInvocationID.x;
    if (tid >= total_pixels) return;

    int halfN = int(tap_count) / 2;
    int spl = int(samples_per_line);
    int base = int(tid);

    /* Per-scanline boundary clamping. */
    int line_start = (base / spl) * spl;
    int line_end   = line_start + spl;
    uint row = uint(base / spl);

    /* Apply FIR independently to each channel. */
    float sum_r = 0.0, sum_g = 0.0, sum_b = 0.0;
    for (uint k = 0; k < tap_count; k++) {
        int idx = base - halfN + int(k);
        /* Mirror at scanline boundaries. */
        if (idx < line_start) idx = 2 * line_start - idx;
        if (idx >= line_end)  idx = 2 * line_end - idx - 2;
        idx = clamp(idx, line_start, line_end - 1);

        vec3 h = taps[k].rgb;
        float gate = trace_gate(float(idx - line_start), row, trace);
        uint src = uint(idx) * 3;
        sum_r += h.r * (rgb_in[src + 0] * gate);
        sum_g += h.g * (rgb_in[src + 1] * gate);
        sum_b += h.b * (rgb_in[src + 2] * gate);
    }

    /* §4.1 Velocity modulation + §5.7 asymmetric rise/fall — both
     * operate on the local luminance derivative, so compute it once
     * via centered differences of the neighbors already sampled. */
    if (velocity_mod > 0.001 || asym_rise_fall > 0.001) {
        int ip = clamp(base - 1, line_start, line_end - 1);
        int in_ = clamp(base + 1, line_start, line_end - 1);
        float lp = drive(ip, line_start, row, 0u) * 0.299
                 + drive(ip, line_start, row, 1u) * 0.587
                 + drive(ip, line_start, row, 2u) * 0.114;
        float ln = drive(in_, line_start, row, 0u) * 0.299
                 + drive(in_, line_start, row, 1u) * 0.587
                 + drive(in_, line_start, row, 2u) * 0.114;
        float dL = (ln - lp) * 0.5;
        /* Legacy "velocity modulation" approximation: this offsets
         * voltage with a derivative. It does not model scan velocity,
         * beam displacement or dwell, and is not equivalent to SVM. */
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
        int prev_start = line_start - spl;
        sum_r = mix(sum_r, drive(prev_col, prev_start, row - 1u, 0u), vertical_smear * 0.1);
        sum_g = mix(sum_g, drive(prev_col, prev_start, row - 1u, 1u), vertical_smear * 0.1);
        sum_b = mix(sum_b, drive(prev_col, prev_start, row - 1u, 2u), vertical_smear * 0.1);
    }

    uint dst = tid * 3;
    rgb_out[dst + 0] = sum_r;
    rgb_out[dst + 1] = sum_g;
    rgb_out[dst + 2] = sum_b;
}
