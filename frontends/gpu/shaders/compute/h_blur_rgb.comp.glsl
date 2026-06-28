/*
 * Horizontal Gaussian Blur on Interleaved RGB — GPU Compute Shader
 * ==================================================================
 *
 * Pre-blurs the signal-resolution RGB buffer (2048×240, interleaved
 * R,G,B,R,G,B,...) to eliminate the expensive h-blur inner loop from
 * the beam profile shader. One thread per signal sample.
 *
 * Dispatch: ceil(signal_w/256) × 240 × 1
 */

#version 450

layout(local_size_x = 256) in;

layout(set = 0, binding = 0) buffer RGBIn {
    float rgb_in[];   /* [sample * 3 + channel] */
};

layout(set = 0, binding = 1) buffer RGBOut {
    float rgb_out[];  /* [sample * 3 + channel] */
};

layout(set = 1, binding = 0) uniform Params {
    uint  signal_w;       /* samples per scanline (2048) */
    uint  num_lines;      /* number of scanlines (240) */
    float sigma;          /* blur sigma in signal samples */
};

void main() {
    uint sx = gl_GlobalInvocationID.x;
    uint sy = gl_GlobalInvocationID.y;
    if (sx >= signal_w || sy >= num_lines) return;

    float hs = max(sigma, 0.5);
    int h_radius = int(ceil(hs * 3.0));  /* 3σ covers 99.7% of Gaussian energy */
    h_radius = min(h_radius, 24);
    float inv_2hs2 = 1.0 / (2.0 * hs * hs);

    float sr = 0.0, sg = 0.0, sb = 0.0;
    float wt = 0.0;

    uint base = sy * signal_w;
    int cx = int(sx);

    for (int dx = -h_radius; dx <= h_radius; dx++) {
        float w = exp(-float(dx * dx) * inv_2hs2);
        int x = clamp(cx + dx, 0, int(signal_w) - 1);
        uint idx = (base + uint(x)) * 3u;
        sr += rgb_in[idx + 0u] * w;
        sg += rgb_in[idx + 1u] * w;
        sb += rgb_in[idx + 2u] * w;
        wt += w;
    }

    float inv_wt = 1.0 / wt;
    uint out_idx = (base + sx) * 3u;
    rgb_out[out_idx + 0u] = sr * inv_wt;
    rgb_out[out_idx + 1u] = sg * inv_wt;
    rgb_out[out_idx + 2u] = sb * inv_wt;
}
