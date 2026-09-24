/* Keyed top-sync AGC: the loop, one workgroup for the frame.
 *
 * A set's AGC is one capacitor: a peak detector charges it during the sync
 * tip whenever the detected tip exceeds the reference, and a constant
 * current discharges it otherwise (TDA8362 objective specification: with
 * C = 2.2 uF the loop takes 2 ms to recover from a +52 dB step and 25 ms
 * from a -52 dB step). Here the charge is proportional to the tip's
 * excess over the reference, so the loop settles without hunting, with
 * the small-signal time constant attack_tau and a slew limit for large
 * steps; the discharge is the constant slew release_db_per_line. Under
 * snow the peak detector follows the noise peaks, not the mean: the tip
 * is taken as its mean plus noise_peak sigma over the sync window.
 *
 * The 256 lanes first take every line's sync-tip mean and sigma (dots 8
 * to 20) and porch (dots 46 to 49) in parallel; lane 0 then walks the
 * lines with the capacitor state carried from the previous frame, so
 * there is no per-line state and no per-frame reset, and writes one gain
 * per line for agc.comp to apply. gains[num_lines] holds the state. */
#version 450
layout(local_size_x = 256) in;
layout(set = 0, binding = 0) readonly buffer DataBuf { float data[]; };
layout(set = 1, binding = 0) buffer Gains { vec4 gains[]; };
layout(set = 2, binding = 0) uniform Params {
    uint  total_count;
    uint  samples_per_line;
    uint  num_lines;
    float target_level;         /* sync-to-porch amplitude the loop holds the tip at */
    float attack_coeff;         /* charge per line as a fraction of the excess, 1 - exp(-T_line / attack_tau) */
    float release_coeff;        /* discharge, dB per line */
    float min_gain;
    float max_gain;
    float attack_slew_db;       /* charge limit, dB per line, for large steps */
    float noise_peak;           /* sigmas the peak detector rides above the tip's mean */
    float pad0, pad1;
};
shared float tip_mean[320], tip_sigma[320], porch_mean[320];
const float DB_PER_OCTAVE = 6.02059991;   // 20 log10(2): dB from log2, GLSL has no log10
float to_db(float x) { return DB_PER_OCTAVE * log2(x); }
float from_db(float db) { return exp2(db / DB_PER_OCTAVE); }
void main() {
    uint spp = samples_per_line / 341u;
    for (uint line = gl_LocalInvocationID.x; line < num_lines && line < 320u; line += 256u) {
        uint start = line * samples_per_line;
        float s = 0.0, p = 0.0;
        for (uint x = 8u*spp; x < 20u*spp; x++) s += data[start+x];
        for (uint x = 46u*spp; x < 49u*spp; x++) p += data[start+x];
        float n = float(12u*spp), m = s / n, d2 = 0.0;
        /* Two passes: a one-pass variance cancels catastrophically on a clean tip. */
        for (uint x = 8u*spp; x < 20u*spp; x++) { float d = data[start+x] - m; d2 += d*d; }
        tip_mean[line] = m;
        tip_sigma[line] = sqrt(d2 / n);
        porch_mean[line] = p / float(3u*spp);
    }
    barrier();
    if (gl_LocalInvocationID.x != 0u) return;
    vec4 state = gains[num_lines];
    float gain_db = state.x;
    bool fresh = state.w == 0.0;
    for (uint line = 0u; line < num_lines && line < 320u; line++) {
        /* The detector sees the tip below the porch by the sync depth, with
         * the noise peaks riding on it. */
        float depth = max(porch_mean[line] - tip_mean[line] + noise_peak * tip_sigma[line], 0.0);
        float measured_db = depth > 1e-4 ? to_db(depth / target_level) : -60.0;
        if (fresh) { gain_db = -measured_db; fresh = false; }   /* a set switching on acquires within its attack */
        float excess = measured_db + gain_db;                   /* output tip over the reference, dB */
        if (excess > 0.0) gain_db -= min(attack_coeff * excess, attack_slew_db);
        else gain_db += min(release_coeff, -excess);           /* discharge, but not past the reference */
        gain_db = clamp(gain_db, to_db(min_gain), to_db(max_gain));
        gains[line] = vec4(from_db(gain_db), depth, tip_sigma[line], 0.0);
    }
    gains[num_lines] = vec4(gain_db, 0.0, 0.0, 1.0);
}
