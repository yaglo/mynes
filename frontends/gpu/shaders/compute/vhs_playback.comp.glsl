/* VHS playback (V2): one threadgroup per raster line, 224 threads x 16
 * samples at 12 fsc, starting 856 samples (20 us) before the line so the
 * de-emphasis has settled.
 *
 * Dropout compensator: a masked sample takes the recovered luma one glass
 * delay line earlier (227.5 carrier cycles, 2730 samples), up to four
 * lines back, with a click at each switch between unrelated FM signals.
 * Then de-emphasis, the noise canceller (low band plus a limited high
 * band), the two-tap aperture and the Y delay line. Chroma: 1H comb over
 * the colour-under envelope and up-conversion onto the fixed carrier grid.
 */
#version 450
#extension GL_GOOGLE_include_directive : require
#extension GL_KHR_shader_subgroup_basic : require
#extension GL_KHR_shader_subgroup_shuffle_relative : require
layout(local_size_x = 224) in;

layout(set=0,binding=0) readonly buffer Luma { float y12[]; };
layout(set=0,binding=1) readonly buffer Chroma { vec2 c3[]; };
layout(set=0,binding=2) readonly buffer Mask { uint dropout_mask[]; };
layout(set=0,binding=3) readonly buffer Coeffs { vec4 coeff[]; };
layout(set=1,binding=0) writeonly buffer Output { float signal_out[]; };

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
const int PRE = 856;
const int GLASS = 2730;

/* 3 fsc chroma kept for this line's output: the comb result from 8
 * samples before the line (cubic support and the chroma delay) onwards. */
const int C_PRE = 8, C_LEN = SPL3 + 16;

shared float ysh[TILE];
shared vec2 comb[C_LEN];

uint mask_word(int n) {
    n = wrap_sample(n);
    int l = n / SPL, x = n - l * SPL;
    return dropout_mask[l * MASK_WORDS + x / 16] >> uint(x % 16);
}
bool masked(int n) { return (mask_word(n) & 1u) != 0u; }
vec2 c3_at(int j) {
    const int N = LINES * SPL3;
    return c3[j < 0 ? j + N : (j >= N ? j - N : j)];
}
vec2 cubic2(vec2 p0, vec2 p1, vec2 p2, vec2 p3, float t) {
    return p1 + 0.5 * t * (p2 - p0 + t * (2.0 * p0 - 5.0 * p1 + 4.0 * p2 - p3 + t * (3.0 * (p1 - p2) + p3 - p0)));
}

void main() {
    const vec2 carrier[12] = vec2[12](
        vec2(1.0, 0.0), vec2(0.8660254, 0.5), vec2(0.5, 0.8660254), vec2(0.0, 1.0),
        vec2(-0.5, 0.8660254), vec2(-0.8660254, 0.5), vec2(-1.0, 0.0), vec2(-0.8660254, -0.5),
        vec2(-0.5, -0.8660254), vec2(0.0, -1.0), vec2(0.5, -0.8660254), vec2(0.8660254, -0.5));
    uint T = thread_index();
    int line = int(gl_WorkGroupID.x);
    int q0 = int(T) * 16, n0 = line * SPL - PRE + q0;

    float y[16];
    for (int k = 0; k < 16; k++) y[k] = y12[wrap_sample(n0 + k)];
    /* Most threads see no dropout: the mask words around them are zero. */
    bool any = false;
    if (doc != 0u)
        for (int i = -24; i < 16; i += 8) any = any || (mask_word(n0 + i) & 0xffu) != 0u;
    if (any) {
        bool m[40];
        for (int i = 0; i < 40; i++) m[i] = masked(n0 - 24 + i);
        for (int k = 0; k < 16; k++) {
            int n = n0 + k;
            if (m[24 + k]) {
                int src = n - GLASS;
                for (int j = 1; j < 4 && masked(src); j++) src -= GLASS;
                y[k] = y12[wrap_sample(src)];
            }
            /* Switching between two FM signals jumps the phase at random;
             * the discriminator turns it into an impulse through the
             * playback low-pass. */
            for (int i = 0; i < 24; i++) {
                int a = 24 + k - i;
                if (m[a] != m[a - 1]) {
                    float jump = (2.0 * uniform01(noise_key(19u, n - i)) - 1.0) * 3.14159265;
                    y[k] += coeff[CLICK + i / 4][i % 4] * click_scale * jump;
                }
            }
        }
    }
    iir_real16(F_DE, y);
    if (canceller_limit > 0.0) {
        float low[16] = y;
        iir_real16(F_CANC, low);
        for (int k = 0; k < 16; k++) y[k] -= clamp(y[k] - low[k], -canceller_limit, canceller_limit);
    }
    for (int k = 0; k < 16; k++) ysh[q0 + k] = y[k];
    /* 1H comb on the colour-under envelope: this line and the glass delay
     * line, 682.5 samples back at 3 fsc (half-sample cubic weights). */
    int m0 = line * SPL3 - C_PRE;
    for (int i = int(T); i < C_LEN; i += 224) {
        int m = m0 + i - int(floor(c_delay));
        vec2 prev = 0.5625 * (c3_at(m - 683) + c3_at(m - 682)) - 0.0625 * (c3_at(m - 684) + c3_at(m - 681));
        comb[i] = 0.5 * playback_acc * (c3_at(m) + prev);
    }
    barrier();
    /* Two-tap aperture, written back over its input. */
    int d = int(sharp_d);
    float sharp[16];
    for (int k = 0; k < 16; k++) {
        int c = clamp(q0 + k, d, TILE - 1 - d);
        float e = ysh[c] - 0.5 * (ysh[c - d] + ysh[c + d]);
        if (detail_limit > 0.0) e = clamp(e, -detail_limit, detail_limit);
        sharp[k] = ysh[c] + sharpness * e;
    }
    barrier();
    for (int k = 0; k < 16; k++) ysh[q0 + k] = sharp[k];
    barrier();

    float cfrac = fract(c_delay);
    for (int k = 0; k < 16; k++) {
        int x = q0 + k - PRE;
        if (x < 0) continue;
        /* Y delay line: luma read y_delay samples back through the centred
         * aperture, with cubic interpolation. */
        float c = float(q0 + k) - y_delay;
        int i = int(floor(c)); float t = c - float(i);
        float p0 = ysh[max(i - 1, 0)], p1 = ysh[i], p2 = ysh[i + 1], p3 = ysh[min(i + 2, TILE - 1)];
        float luma = p1 + 0.5 * t * (p2 - p0 + t * (2.0 * p0 - 5.0 * p1 + 4.0 * p2 - p3 + t * (3.0 * (p1 - p2) + p3 - p0)));
        /* Chroma at 12 fsc from the combed 3 fsc envelope, then the carrier
         * from the crystal reference on the fixed grid. */
        float cm = float(x) * 0.25 - cfrac + float(C_PRE);
        int j = int(floor(cm)); float u = cm - float(j);
        vec2 cz = cubic2(comb[j - 1], comb[j], comb[j + 1], comb[j + 2], u);
        int n = line * SPL + x;
        vec2 ref = carrier[n % 12];
        signal_out[n] = (luma + cz.x * ref.x - cz.y * ref.y) * out_scale;
    }
}
