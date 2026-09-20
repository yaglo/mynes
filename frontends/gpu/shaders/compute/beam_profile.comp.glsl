/*
 * Electron Beam Profile — GPU Compute Shader
 * =========================================
 *
 * Consumes:
 *   - signal-resolution RGB (already matrix-decoded and horizontally blurred)
 *   - display-resolution deflection maps (landing X/Y + dwell + sigma scale)
 *
 * Produces:
 *   - display-resolution RGBA16F beam buffer
 *
 * All raster geometry, convergence, jitter, focus growth, and dwell
 * modulation now come from deflection.comp. This shader is responsible
 * only for beam deposition, hum/noise modulation, and packing.
 */

#version 450

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
    uint  signal_w;
    uint  out_w;
    uint  out_h;
    uint  rows_per_scanline;
    float sigma_narrow;
    float sigma_wide;
    float black_floor;
    float noise_level;
    uint  frame_counter;
    float hum_bar_amplitude;
    float bloom_gamma;
    float gamma, gamma_r, gamma_g, gamma_b;
};

float sample_rgb_channel_linear(float sx, int sy, uint channel) {
    if (sy < 0 || sy >= 240) return 0.0;

    int signal_w_i = max(int(signal_w), 1);
    float sx_max = float(signal_w_i - 1);

    /* Outside the landed raster we want the beam to taper to black,
     * not clamp to the edge pixel. Allow half a sample of grace for
     * interpolation, then return zero. */
    if (sx < -0.5 || sx > sx_max + 0.5) return 0.0;

    float sx_clamped = clamp(sx, 0.0, sx_max);
    int x0 = int(floor(sx_clamped));
    int x1 = min(x0 + 1, signal_w_i - 1);
    float tx = sx_clamped - float(x0);

    uint base = uint(sy) * signal_w;
    float a = rgb_in[(base + uint(x0)) * 3u + channel];
    float b = rgb_in[(base + uint(x1)) * 3u + channel];
    return mix(a, b, tx);
}

void main() {
    uint ox = gl_GlobalInvocationID.x;
    uint oy = gl_GlobalInvocationID.y;
    if (ox >= out_w || oy >= out_h) return;

    uint pix = oy * out_w + ox;
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

    uint rps = rows_per_scanline;
    uint out_h_clamped = max(out_h, 1u);

    float r_vy_clamped = clamp(r_vy, 0.0, float(out_h_clamped - 1u));
    float g_vy_clamped = clamp(g_vy, 0.0, float(out_h_clamped - 1u));
    float b_vy_clamped = clamp(b_vy, 0.0, float(out_h_clamped - 1u));

    float r_linef = r_vy_clamped / float(rps);
    float g_linef = g_vy_clamped / float(rps);
    float b_linef = b_vy_clamped / float(rps);

    float r_d = fract(r_linef) - 0.5;
    float g_d = fract(g_linef) - 0.5;
    float b_d = fract(b_linef) - 0.5;

    uint r_sy = clamp(uint(floor(r_linef)), 0u, 239u);
    uint g_sy = clamp(uint(floor(g_linef)), 0u, 239u);
    uint b_sy = clamp(uint(floor(b_linef)), 0u, 239u);

    float R = 0.0, G = 0.0, B = 0.0;

    for (int soff = -1; soff <= 1; soff++) {
        float lR = 0.0;
        float lG = 0.0;
        float lB = 0.0;

        int r_line = int(r_sy) + soff;
        int g_line = int(g_sy) + soff;
        int b_line = int(b_sy) + soff;

        lR = sample_rgb_channel_linear(r_center, r_line, 0u);
        lG = sample_rgb_channel_linear(g_center, g_line, 1u);
        lB = sample_rgb_channel_linear(b_center, b_line, 2u);

        float lY = clamp(0.299 * lR + 0.587 * lG + 0.114 * lB, 0.0, 1.0);
        float bloom_t = pow(lY, bloom_gamma);
        float sv = (sigma_narrow + (sigma_wide - sigma_narrow) * bloom_t)
                 * focus_scale;
        float inv2s = 1.0 / max(2.0 * sv * sv, 1e-5);

        float rd = r_d - float(soff);
        float gd = g_d - float(soff);
        float bd = b_d - float(soff);

        // Gun voltage becomes light before spatial beam deposition.
        // Unit-area spots conserve current as focus/bloom changes width.
        float energy = 1.0 / max(2.50662827463 * sv, 0.001);
        R += pow(max(lR,0.0),gamma+gamma_r) * exp(-(rd * rd) * inv2s) * energy;
        G += pow(max(lG,0.0),gamma+gamma_g) * exp(-(gd * gd) * inv2s) * energy;
        B += pow(max(lB,0.0),gamma+gamma_b) * exp(-(bd * bd) * inv2s) * energy;
    }

    R *= dwell;
    G *= dwell;
    B *= dwell;

    uint sy = g_sy;
    if (hum_bar_amplitude > 0.0) {
        float hum_phase = float(sy) / 240.0 * 6.283185 + float(frame_counter) * 0.006;
        float hum_wave = sin(hum_phase)
                       + 0.40 * sin(2.0 * hum_phase + 0.8)
                       + 0.15 * sin(3.0 * hum_phase + 1.5);
        float hum = 1.0 - hum_bar_amplitude * (0.5 + 0.35 * hum_wave);
        R *= hum;
        G *= hum;
        B *= hum;
    }

    if (noise_level > 0.0) {
        uint sr = ox * 1999u + oy * 7919u + frame_counter * 6271u;
        uint sg = ox * 2999u + oy * 8923u + frame_counter * 4517u;
        uint sb = ox * 3989u + oy * 9341u + frame_counter * 7211u;
        sr ^= sr >> 16u; sr *= 0x45d9f3bu; sr ^= sr >> 16u;
        sg ^= sg >> 16u; sg *= 0x45d9f3bu; sg ^= sg >> 16u;
        sb ^= sb >> 16u; sb *= 0x45d9f3bu; sb ^= sb >> 16u;
        float luma = 0.299 * R + 0.587 * G + 0.114 * B;
        float noise_scale = noise_level * (1.0 - 0.8 * clamp(luma, 0.0, 1.0));
        R += (float(sr & 0xFFFFu) / 65535.0 - 0.5) * noise_scale;
        G += (float(sg & 0xFFFFu) / 65535.0 - 0.5) * noise_scale;
        B += (float(sb & 0xFFFFu) / 65535.0 - 0.5) * noise_scale;
    }

    R = min(max(R, pow(max(black_floor,0.0),gamma)), 4.0);
    G = min(max(G, pow(max(black_floor,0.0),gamma)), 4.0);
    B = min(max(B, pow(max(black_floor,0.0),gamma)), 4.0);

    uint idx = pix * 2u;
    rgba_out[idx + 0u] = packHalf2x16(vec2(R, G));
    rgba_out[idx + 1u] = packHalf2x16(vec2(B, 1.0));
}
