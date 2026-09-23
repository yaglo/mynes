/* VHS tape (V1): one threadgroup per raster line, 224 threads x 16 samples
 * at 12 fsc, from 512 samples before the line to 344 after it.
 *
 * Record: keyed AGC (sync to -40 IRE), Y low-pass with colour trap, IEC
 * pre-emphasis and white/dark clip, FM deviation. Colour: band-pass, product
 * detector on the fixed carrier grid, record ACC to a 20 IRE burst.
 * Playback: the recorded frequency and colour envelope are read at t - tau(t)
 * from the host timing table, so the chroma content moves with the
 * transport while its carrier does not. The FM carrier gets tape modulation
 * noise, dropout spacing loss, the record/tape and playback RF responses and
 * white RF noise, then a limiter and pulse-count discriminator. Writes the
 * playback luma (before de-emphasis), the colour-under envelope before the
 * 1H comb, and the dropout detector mask.
 */
#version 450
#extension GL_GOOGLE_include_directive : require
#extension GL_KHR_shader_subgroup_basic : require
#extension GL_KHR_shader_subgroup_shuffle_relative : require
layout(local_size_x = 224) in;

layout(set=0,binding=0) readonly buffer Input { float signal_in[]; };
layout(set=0,binding=1) readonly buffer Coeffs { vec4 coeff[]; };
layout(set=0,binding=2) readonly buffer LineTable { vec4 line_table[]; };
layout(set=0,binding=3) readonly buffer Defects { vec4 defects[]; };
layout(set=1,binding=0) writeonly buffer Luma { float y12[]; };
layout(set=1,binding=1) writeonly buffer Chroma { vec2 c3[]; };
layout(set=1,binding=2) writeonly buffer Mask { uint dropout_mask[]; };

layout(set=2,binding=0) uniform Params {
    uint frame, defect_count, doc, total;
    float in_gain, dark_clip, white_clip, f_sync;
    float hz_per_pct, sample_rate, mod_noise_sigma, rf_noise_sigma;
    float env_norm, doc_on, doc_off, chroma_noise_sigma;
    float burst_target, burst_norm, spacing_db, colour_under_hz;
    float canceller_limit, sharpness, detail_limit, y_delay;
    float c_delay, out_scale, playback_acc, click_scale;
    float sharp_d, reserved0, reserved1, reserved2;
};

#include "vhs_common.glsl"

const int TILE = 3584;
const int PRE = 512;
const int MAX_TILE_DEFECTS = 32;

/* Recorded carrier frequency, 16-bit over the clip range (31 Hz steps),
 * two samples per word, and the colour-under envelope at 3 fsc as half
 * floats. Each thread owns a run of 8 (4) words; one pad word per run
 * keeps a simdgroup's accesses on distinct banks. */
shared uint f_rec[TILE / 2 + TILE / 16];
shared uint c_rec[TILE / 4 + TILE / 16];
int f_addr(int w) { return w + (w >> 3); }
int c_addr(int i) { return i + (i >> 2); }
shared vec4 tile_defect[MAX_TILE_DEFECTS];
shared vec2 tile_valid[MAX_TILE_DEFECTS];
shared uint tile_defects;
shared vec2 burst_part[128];

/* Table rows for lines line-1 .. line+2, kept in registers: a = timing,
 * b = RF level and noise, c = chroma phase and RF phase jump. */
vec4 a0, a1, a2, a3, b0, b1, b2, b3, c0, c1, c2;
vec4 pick(int r, vec4 x0, vec4 x1, vec4 x2) { return r == 0 ? x0 : (r == 1 ? x1 : x2); }

/* Timing (samples), RF level (dB) and noise gain at tile position q. The
 * next line's start closes each line; on the switch line the old head
 * runs to x_switch and the new head starts there at 0 dB. */
void transport_at(int q, out float tau, out float rf_db, out float noise_gain) {
    int x = q - PRE, r = x < 0 ? 0 : (x < SPL ? 1 : 2);
    float xl = float(x - (r - 1) * SPL);
    vec4 a = pick(r, a0, a1, a2), b = pick(r, b0, b1, b2);
    float next_tau = r == 0 ? a1.x : (r == 1 ? a2.x : a3.x);
    float next_db = r == 0 ? b1.x : (r == 1 ? b2.x : b3.x);
    if (a.w >= 0.0 && xl >= a.w) {
        float t = (xl - a.w) / (float(SPL) - a.w);
        tau = mix(a.z, next_tau, t); rf_db = mix(0.0, next_db, t); noise_gain = b.w;
    } else {
        float t = a.w >= 0.0 ? xl / a.w : xl / float(SPL);
        tau = mix(a.x, a.w >= 0.0 ? a.y : next_tau, t);
        rf_db = mix(b.x, a.w >= 0.0 ? b.y : next_db, t);
        noise_gain = b.z;
    }
}
/* Line segment of tile position q: row, and which side of a switch. */
int segment(int q) {
    int x = q - PRE, r = x < 0 ? 0 : (x < SPL ? 1 : 2);
    vec4 a = pick(r, a0, a1, a2);
    return 2 * r + (a.w >= 0.0 && float(x - (r - 1) * SPL) >= a.w ? 1 : 0);
}
/* Head-to-tape spacing (um) of the defects under the playing head. */
float spacing_at(float n) {
    float sp = 0.0;
    for (uint i = 0u; i < tile_defects; i++) {
        vec4 d = tile_defect[i];
        vec2 v = tile_valid[i];
        float k = n - d.x;
        if (n < v.x || n >= v.y || k < 0.0 || k > d.y) continue;
        float t = min(1.0, min(k, d.y - k) / d.w);
        sp = max(sp, d.z * (0.5 - 0.5 * cos(3.14159265 * t)));
    }
    return sp;
}
float f_lo, f_span;
float f_rec_at(int i) {
    i = clamp(i, 0, TILE - 1);
    vec2 pair = unpackUnorm2x16(f_rec[f_addr(i >> 1)]);
    return f_lo + f_span * ((i & 1) == 0 ? pair.x : pair.y);
}
float cubic(float x) {
    int i = int(floor(x)); float t = x - float(i);
    float p0 = f_rec_at(i - 1), p1 = f_rec_at(i), p2 = f_rec_at(i + 1), p3 = f_rec_at(i + 2);
    return p1 + 0.5 * t * (p2 - p0 + t * (2.0 * p0 - 5.0 * p1 + 4.0 * p2 - p3 + t * (3.0 * (p1 - p2) + p3 - p0)));
}
vec2 c_rec_at(int i) { return unpackHalf2x16(c_rec[c_addr(clamp(i, 0, TILE / 4 - 1))]); }
vec2 cubic_c(float x) {
    int i = int(floor(x)); float t = x - float(i);
    vec2 p0 = c_rec_at(i - 1), p1 = c_rec_at(i), p2 = c_rec_at(i + 1), p3 = c_rec_at(i + 2);
    return p1 + 0.5 * t * (p2 - p0 + t * (2.0 * p0 - 5.0 * p1 + 4.0 * p2 - p3 + t * (3.0 * (p1 - p2) + p3 - p0)));
}

void main() {
    const vec2 carrier[12] = vec2[12](
        vec2(1.0, 0.0), vec2(0.8660254, 0.5), vec2(0.5, 0.8660254), vec2(0.0, 1.0),
        vec2(-0.5, 0.8660254), vec2(-0.8660254, 0.5), vec2(-1.0, 0.0), vec2(-0.8660254, -0.5),
        vec2(-0.5, -0.8660254), vec2(0.0, -1.0), vec2(0.5, -0.8660254), vec2(0.8660254, -0.5));
    uint T = thread_index();
    f_lo = f_sync + dark_clip * hz_per_pct;
    f_span = (white_clip - dark_clip) * hz_per_pct;
    int line = int(gl_WorkGroupID.x);
    int q0 = int(T) * 16, n0 = line * SPL - PRE + q0;
    a0 = line_table[3 * line]; b0 = line_table[3 * line + 1]; c0 = line_table[3 * line + 2];
    a1 = line_table[3 * line + 3]; b1 = line_table[3 * line + 4]; c1 = line_table[3 * line + 5];
    a2 = line_table[3 * line + 6]; b2 = line_table[3 * line + 7]; c2 = line_table[3 * line + 8];
    int last = min(line + 3, LINES + 1);
    a3 = line_table[3 * last]; b3 = line_table[3 * last + 1];
    if (T == 0u) tile_defects = 0u;
    barrier();
    /* Dropouts that can reach this tile, and the burst of 16 nearby lines
     * for the record ACC (a slow loop; the NES burst does not change). */
    float tile_lo = float(line * SPL - PRE), tile_hi = tile_lo + float(TILE);
    if (T < defect_count && T < 64u) {
        vec4 d = defects[2u * T], v = defects[2u * T + 1u];
        float lo = max(d.x, v.x), hi = min(d.x + d.y, v.y);
        if (hi > tile_lo && lo < tile_hi) {
            uint slot = atomicAdd(tile_defects, 1u);
            if (slot < uint(MAX_TILE_DEFECTS)) { tile_defect[slot] = d; tile_valid[slot] = v.xy; }
        }
    }
    if (T >= 32u && T < 32u + 16u * 8u) {
        /* 16 lines x 96 samples, one carrier cycle-aligned 12 per thread. */
        int i = int(T) - 32, l = (line - 8 + i / 8 + LINES) % LINES, x0 = 240 + 12 * (i % 8);
        vec2 acc = vec2(0);
        for (int x = 0; x < 12; x++) acc += signal_in[l * SPL + x0 + x] * carrier[(l * SPL + x0 + x) % 12];
        burst_part[i] = acc;
    }
    barrier();
    if (T == 0u) tile_defects = min(tile_defects, uint(MAX_TILE_DEFECTS));
    float peak = 0.0, sum = 0.0, used = 0.0, burst[16];
    for (int i = 0; i < 16; i++) {
        vec2 acc = vec2(0);
        for (int j = 0; j < 8; j++) acc += burst_part[8 * i + j];
        burst[i] = 2.0 / 96.0 * length(acc) * in_gain;
        peak = max(peak, burst[i]);
    }
    for (int i = 0; i < 16; i++) if (burst[i] > 0.5 * peak) { sum += burst[i]; used += 1.0; }
    float acc_gain = sum > 1.0 ? burst_target * burst_norm * used / sum : 1.0;

    /* ---- record ---- */
    float x[16];
    for (int k = 0; k < 16; k++) x[k] = signal_in[wrap_sample(n0 + k)] * in_gain;
    {
        float bp[16] = x;
        iir_real16(F_CREC, bp);
        vec2 u[16];
        for (int k = 0; k < 16; k++) {
            int ph = (n0 + k) % 12; ph = ph < 0 ? ph + 12 : ph;
            u[k] = 2.0 * bp[k] * vec2(carrier[ph].x, -carrier[ph].y);
        }
        /* Six-sample box (null at 2 fsc), then every fourth sample. */
        vec4 prev = previous_thread(vec4(u[11] + u[12] + u[13] + u[14] + u[15], u[15]));
        float g = acc_gain / 6.0;
        int c = c_addr(4 * int(T));
        c_rec[c] = packHalf2x16(g * (prev.xy + u[0]));
        c_rec[c + 1] = packHalf2x16(g * (prev.zw + u[0] + u[1] + u[2] + u[3] + u[4]));
        c_rec[c + 2] = packHalf2x16(g * (u[3] + u[4] + u[5] + u[6] + u[7] + u[8]));
        c_rec[c + 3] = packHalf2x16(g * (u[7] + u[8] + u[9] + u[10] + u[11] + u[12]));
    }
    iir_real16(F_YREC, x);
    {
        float pe[16] = x;
        iir_real16(F_PRE, pe);
        for (int k = 0; k < 16; k += 2) {
            vec2 pct = clamp((vec2(pe[k], pe[k + 1]) + 40.0) * (100.0 / 140.0), dark_clip, white_clip);
            f_rec[f_addr((q0 + k) >> 1)] = packUnorm2x16((pct - dark_clip) / (white_clip - dark_clip));
        }
    }
    barrier();

    /* ---- transport and FM carrier ---- */
    float mod_noise[16], phase[16], amp[16], noise_gain[16];
    for (int k = 0; k < 16; k += 2) {
        vec2 g = mod_noise_sigma * gauss2(noise_key(11u, n0 + k));
        mod_noise[k] = g.x; mod_noise[k + 1] = g.y;
    }
    iir_real16(F_MOD, mod_noise);
    /* Timing is linear within a line segment: evaluate the block's ends and
     * interpolate unless a line start or the head switch falls inside. */
    float tau_a, db_a, g_a, tau_b, db_b, g_b;
    transport_at(q0, tau_a, db_a, g_a);
    transport_at(q0 + 15, tau_b, db_b, g_b);
    bool linear = segment(q0) == segment(q0 + 15);
    for (int k = 0; k < 16; k++) {
        int q = q0 + k;
        float tau, rf_db;
        if (linear) {
            float t = float(k) / 15.0;
            tau = mix(tau_a, tau_b, t); rf_db = mix(db_a, db_b, t); noise_gain[k] = g_a;
        } else transport_at(q, tau, rf_db, noise_gain[k]);
        float f = cubic(float(q) - tau) + mod_noise[k];
        float db = rf_db - spacing_db * spacing_at(float(n0 + k)) * f;
        amp[k] = exp2(db * 0.16609640474);
        phase[k] = f / sample_rate;
    }
    {
        float s = 0.0, loc[16];
        for (int k = 0; k < 16; k++) { s = fract(s + phase[k]); loc[k] = s; }
        float c = carry_fract(s);
        for (int k = 0; k < 16; k++) phase[k] = loc[k] + c;
    }
    /* The heads' signals are unrelated in phase: a jump at the switch. */
    for (int r = 0; r < 3; r++) {
        vec4 a = pick(r, a0, a1, a2);
        if (a.w < 0.0) continue;
        float qs = float(PRE + (r - 1) * SPL) + a.w, jump = pick(r, c0, c1, c2).y / TWO_PI;
        for (int k = 0; k < 16; k++) if (float(q0 + k) >= qs) phase[k] += jump;
    }
    vec2 z[16];
    for (int k = 0; k < 16; k++) {
        float a = TWO_PI * fract(phase[k]);
        z[k] = amp[k] * vec2(cos(a), sin(a));
    }
    iir_complex16(F_RF_REC, z);
    for (int k = 0; k < 16; k++)
        z[k] += rf_noise_sigma * noise_gain[k] * gauss2(noise_key(13u, n0 + k));
    iir_complex16(F_RF_PB, z);

    /* ---- dropout detector: integrated envelope with hysteresis ---- */
    {
        float e[16];
        for (int k = 0; k < 16; k++) e[k] = length(z[k]) * env_norm;
        iir_real16(F_ENV, e);
        int last = -1;
        for (int k = 0; k < 16; k++) {
            if (e[k] < doc_on) last = 2 * (q0 + k) + 1;
            else if (e[k] > doc_off) last = 2 * (q0 + k);
        }
        int state = carry_max(last);
        uint bits = 0u;
        for (int k = 0; k < 16; k++) {
            if (e[k] < doc_on) state = 2 * (q0 + k) + 1;
            else if (e[k] > doc_off) state = 2 * (q0 + k);
            if (state >= 0 && (state & 1) == 1) bits |= 1u << uint(k);
        }
        int word = int(T) - PRE / 16;
        if (word >= 0 && word < MASK_WORDS) dropout_mask[line * MASK_WORDS + word] = bits;
    }

    /* ---- limiter and pulse-count discriminator ---- */
    vec4 zp = previous_thread(vec4(z[15], 0.0, 0.0));
    float y[16];
    for (int k = 0; k < 16; k++) {
        vec2 prev = k == 0 ? (T == 0u ? z[0] : zp.xy) : z[k - 1];
        vec2 w = cmul(z[k], vec2(prev.x, -prev.y));
        float f = abs(atan(w.y, w.x)) * sample_rate / TWO_PI;
        y[k] = (f - f_sync) / hz_per_pct * 1.4 - 40.0;
    }
    iir_real16(F_YPB, y);
    for (int k = 0; k < 16; k++) {
        int xpos = q0 + k - PRE;
        if (xpos >= 0 && xpos < SPL) y12[line * SPL + xpos] = y[k];
    }

    /* ---- colour-under on tape, playback band-pass ---- */
    vec2 cc[4];
    for (int j = 0; j < 4; j++) {
        int q = q0 + 4 * j;
        float tau, rf_db, noise_gain;
        transport_at(q, tau, rf_db, noise_gain);
        vec2 c = cubic_c((float(q) - tau) * 0.25);
        c *= exp2(-spacing_db * spacing_at(float(n0 + 4 * j)) * colour_under_hz * 0.16609640474);
        /* The burst is recorded 6 dB hot: half the noise relative to it. */
        float xr = mod(float(q - PRE) - tau, float(SPL));
        float g = xr >= 240.0 && xr < 336.0 ? 0.5 : 1.0;
        int n3 = (line * SPL - PRE) / 4 + int(T) * 4 + j;
        cc[j] = c + chroma_noise_sigma * g * gauss2(noise_key(17u, n3));
    }
    iir_complex4(F_CPB, cc);
    /* Residual phase of the up-conversion (APC/AFC), before the comb. */
    vec2 rot = vec2(cos(c1.x), sin(c1.x));
    for (int j = 0; j < 4; j++) {
        int x3 = int(T) * 4 + j - PRE / 4;
        if (x3 >= 0 && x3 < SPL3) c3[line * SPL3 + x3] = cmul(cc[j], rot);
    }
}
