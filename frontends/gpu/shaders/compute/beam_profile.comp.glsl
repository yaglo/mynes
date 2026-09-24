/*
 * Electron Beam Profile — GPU Compute Shader
 * =========================================
 *
 * Consumes:
 *   - decode-window RGB (already matrix-decoded and horizontally blurred;
 *     decode_window.h: width samples by lines rows, the console picture at
 *     picture_x, picture_row)
 *   - display-resolution deflection maps (landing X/Y + dwell + sigma scale)
 *
 * Produces:
 *   - display-resolution RGBA16F beam buffer
 *
 * All raster geometry, convergence, jitter, focus growth, and dwell
 * modulation now come from deflection.comp. This shader is responsible
 * only for beam deposition, hum modulation, and packing.
 */

#version 450
#extension GL_GOOGLE_include_directive : require

layout(local_size_x = 16, local_size_y = 16) in;

layout(set = 0, binding = 0) readonly buffer RGBIn {
    float rgb_in[];
};

layout(set = 0, binding = 1) readonly buffer DeflectionX {
    float deflection_x[];
};

layout(set = 0, binding = 2) readonly buffer DeflectionY {
    float deflection_y[];
};

/* Output: RGBA as two packed half2 per pixel.
 * rgba_out[pixel*2+0] = packHalf2x16(R, G)
 * rgba_out[pixel*2+1] = packHalf2x16(B, 1.0) */
layout(set = 1, binding = 0) writeonly buffer RGBAOut {
    uint rgba_out[];
};

layout(set = 2, binding = 0) uniform Params {
    uint  width;
    uint  out_w;
    uint  out_h;
    uint  rows_per_scanline;
    float sigma_narrow;
    float sigma_wide;
    uint  frame_counter;
    float hum_bar_amplitude;
    float bloom_gamma;
    float gamma, gamma_r, gamma_g, gamma_b;
    uint monitor_model, lines;
    int  picture_x, picture_row;
    uint picture_w, picture_h;
    float lines_per_row;      /* raster lines per output row */
};

float sample_rgb_channel_linear(float sx, int sy, uint channel) {
    if (sy < 0 || sy >= int(lines)) return 0.0;

    int signal_w_i = max(int(width), 1);
    float sx_max = float(signal_w_i - 1);

    /* Outside the landed raster we want the beam to taper to black,
     * not clamp to the edge pixel. Allow half a sample of grace for
     * interpolation, then return zero. */
    if (sx < -0.5 || sx > sx_max + 0.5) return 0.0;

    float sx_clamped = clamp(sx, 0.0, sx_max);
    int x0 = int(floor(sx_clamped));
    int x1 = min(x0 + 1, signal_w_i - 1);
    float tx = sx_clamped - float(x0);

    uint base = uint(sy) * width;
    float a = rgb_in[(base + uint(x0)) * 3u + channel];
    float b = rgb_in[(base + uint(x1)) * 3u + channel];
    return mix(a, b, tx);
}

// Integral of a unit-area Gaussian over a drawable pixel. Point sampling
// narrow spots loses energy at small sizes and makes scanlines shimmer.
float erf_approx(float x) {
    float t = 1.0 / (1.0 + 0.3275911 * abs(x));
    float p = (((((1.061405429*t - 1.453152027)*t) + 1.421413741)*t
                - 0.284496736)*t + 0.254829592)*t;
    return sign(x) * (1.0 - p * exp(-x*x));
}
float beam_coverage(float distance, float sigma, float width) {
    float scale = 0.70710678118 / max(sigma, 0.01);
    return max(0.0, 0.5 * (erf_approx((distance + 0.5*width)*scale)
                         - erf_approx((distance - 0.5*width)*scale)) / width);
}

#include "fw900_profile.glsl"

void main() {
    uint ox = gl_GlobalInvocationID.x;
    uint oy = gl_GlobalInvocationID.y;
    if (ox >= out_w || oy >= out_h) return;

    uint pix = oy * out_w + ox;
    if (monitor_model == 1u) {
        vec3 light = fw900_render(pix);
        rgba_out[pix*2u] = packHalf2x16(light.rg);
        rgba_out[pix*2u+1u] = packHalf2x16(vec2(light.b,1.0));
        return;
    }
    uint didx = pix * 4u;

    float r_center = deflection_x[didx + 0u];
    float g_center = deflection_x[didx + 1u];
    float b_center = deflection_x[didx + 2u];
    float dwell    = deflection_x[didx + 3u];

    float r_vy        = deflection_y[didx + 0u];
    float g_vy        = deflection_y[didx + 1u];
    float b_vy        = deflection_y[didx + 2u];
    float focus_scale = deflection_y[didx + 3u];

    if (dwell <= 1e-4) {
        uint idx0 = pix * 2u;
        rgba_out[idx0 + 0u] = packHalf2x16(vec2(0.0));
        rgba_out[idx0 + 1u] = packHalf2x16(vec2(0.0, 1.0));
        return;
    }

    /* The deflection map lands each gun in decode-window lines; row r of
     * the window spans r to r + 1. Beyond the decoded rows nothing is
     * sampled, so the raster ends where the beam's spot does. */
    float r_d = fract(r_vy) - 0.5;
    float g_d = fract(g_vy) - 0.5;
    float b_d = fract(b_vy) - 0.5;

    int r_sy = int(floor(r_vy));
    int g_sy = int(floor(g_vy));
    int b_sy = int(floor(b_vy));

    float R = 0.0, G = 0.0, B = 0.0;

    int radius = min(4, int(ceil(3.0 * clamp(max(sigma_narrow,2.0*sigma_wide-sigma_narrow)*focus_scale,0.05,1.0)
                               + 0.5*lines_per_row)));
    for (int soff = -radius; soff <= radius; soff++) {
        float lR = 0.0;
        float lG = 0.0;
        float lB = 0.0;

        int r_line = r_sy + soff;
        int g_line = g_sy + soff;
        int b_line = b_sy + soff;

        lR = sample_rgb_channel_linear(r_center, r_line, 0u);
        lG = sample_rgb_channel_linear(g_center, g_line, 1u);
        lB = sample_rgb_channel_linear(b_center, b_line, 2u);

        // Space charge broadens each gun's own spot. Shared supply/focus
        // changes are already carried by the deflection map; summing RGB
        // here would make a regulated green gun widen when red turns on.
        vec3 exponent = vec3(bloom_gamma) / max(vec3(gamma) + vec3(gamma_r,gamma_g,gamma_b),vec3(1.0));
        vec3 bloom_t = min(pow(max(vec3(lR,lG,lB),vec3(0.0)),exponent),vec3(2.0));
        vec3 sv = clamp(mix(vec3(sigma_narrow),vec3(sigma_wide),bloom_t)
                        * focus_scale,0.05,1.0);
        float pixel_width = lines_per_row;

        float rd = r_d - float(soff);
        float gd = g_d - float(soff);
        float bd = b_d - float(soff);

        // Both axes spread linear emitted current; focus conserves energy.
        R += lR * beam_coverage(rd, sv.r, pixel_width);
        G += lG * beam_coverage(gd, sv.g, pixel_width);
        B += lB * beam_coverage(bd, sv.b, pixel_width);
    }

    R *= dwell;
    G *= dwell;
    B *= dwell;

    if (hum_bar_amplitude > 0.0) {
        float hum_phase = float(g_sy - picture_row) / 240.0 * 6.283185 + float(frame_counter) * 0.006;
        float hum_wave = sin(hum_phase)
                       + 0.40 * sin(2.0 * hum_phase + 0.8)
                       + 0.15 * sin(3.0 * hum_phase + 1.5);
        float hum = 1.0 - hum_bar_amplitude * (0.5 + 0.35 * hum_wave);
        R *= hum;
        G *= hum;
        B *= hum;
    }

    uint idx = pix * 2u;
    rgba_out[idx + 0u] = packHalf2x16(vec2(R, G));
    rgba_out[idx + 1u] = packHalf2x16(vec2(B, 1.0));
}
