/*
 * Temporal Blit — Motion-Adaptive 3D Comb + Phosphor Persistence
 * ================================================================
 *
 * Reads current + previous beam output buffers (float16x4 packed as
 * two uint32 per pixel), detects motion, and applies temporal
 * blending only in static regions where NTSC dot crawl is visible.
 *
 * This implements the core of a PVM-style 3D comb filter:
 *   - Static pixels: full temporal blend → cancels dot crawl perfectly
 *   - Moving pixels: no blend → no ghosting on sprites/scrolling
 *   - Transition zone: smoothstep ramp between the two regimes
 *
 * The motion detector compares per-pixel luminance between frames.
 * Dot crawl is a low-amplitude high-frequency pattern (~5-10% of
 * signal), while real motion produces larger changes. The threshold
 * separates the two.
 *
 * Per-channel phosphor persistence weights model differential P22
 * phosphor decay (green persists ~2x longer than red/blue).
 *
 * Dispatch: ceil(width/16) × ceil(height/16) × 1
 */

#version 450

layout(local_size_x = 16, local_size_y = 16) in;

layout(set = 0, binding = 0) readonly buffer CurBuf  { uint cur_buf[];  };
layout(set = 0, binding = 1) readonly buffer PrevBuf { uint prev_buf[]; };

layout(set = 1, binding = 0, rgba16f) writeonly uniform image2D out_tex;

layout(set = 2, binding = 0) uniform Params {
    uint  width;
    uint  height;
    float blend_factor;       /* base blend for dot crawl cancellation (0.0-0.5) */
    float blend_r;            /* red phosphor persistence weight (0.0-1.0) */
    float blend_g;            /* green phosphor persistence weight (0.0-1.0) */
    float blend_b;            /* blue phosphor persistence weight (0.0-1.0) */
    float motion_threshold;   /* luminance delta below which pixel is "static" */
};

void main() {
    uint x = gl_GlobalInvocationID.x;
    uint y = gl_GlobalInvocationID.y;
    if (x >= width || y >= height) return;

    uint idx = (y * width + x) * 2u;

    vec2 rg = unpackHalf2x16(cur_buf[idx]);
    vec2 ba = unpackHalf2x16(cur_buf[idx + 1u]);

    /* Blend if either dot-crawl blend OR any per-channel persistence is active. */
    float max_blend = max(blend_factor, max(max(blend_r, blend_g), blend_b));
    if (max_blend > 0.001) {
        vec2 rg_p = unpackHalf2x16(prev_buf[idx]);
        vec2 ba_p = unpackHalf2x16(prev_buf[idx + 1u]);

        /* Motion detection: compare luminance between frames.
         * Use max delta across a 3x3 neighborhood to catch smooth scrolling
         * where single-pixel comparison misses sub-threshold motion.
         * On a real CRT, phosphor decays in ~2ms so each frame is independent;
         * on sample-and-hold displays we need to explicitly detect motion
         * to avoid freezing the previous frame's dot crawl onto scrolled content. */
        float motion_mask = 1.0;
        if (motion_threshold > 0.0) {
            float max_delta = 0.0;
            for (int dy = -1; dy <= 1; dy++) {
                for (int dx = -1; dx <= 1; dx++) {
                    int sx = clamp(int(x) + dx, 0, int(width) - 1);
                    int sy = clamp(int(y) + dy, 0, int(height) - 1);
                    uint nx = uint(sx);
                    uint ny = uint(sy);
                    uint ni = (ny * width + nx) * 2u;
                    vec2 nc = unpackHalf2x16(cur_buf[ni]);
                    vec2 nca = unpackHalf2x16(cur_buf[ni + 1u]);
                    vec2 np = unpackHalf2x16(prev_buf[ni]);
                    vec2 npa = unpackHalf2x16(prev_buf[ni + 1u]);
                    float lc = 0.299 * nc.x + 0.587 * nc.y + 0.114 * nca.x;
                    float lp = 0.299 * np.x + 0.587 * np.y + 0.114 * npa.x;
                    max_delta = max(max_delta, abs(lc - lp));
                }
            }
            motion_mask = 1.0 - smoothstep(motion_threshold * 0.5,
                                            motion_threshold * 1.5, max_delta);
        }
        /* motion_threshold <= 0: no detection, always blend (old behavior) */

        /* Two independent blending effects:
         *   blend_factor: uniform inter-frame blend for dot crawl cancellation.
         *     Motion mask suppresses it (prevents ghosting on scrolling content).
         *   blend_r/g/b: per-channel phosphor persistence (color trails).
         *     NOT motion-masked — persistence IS motion smear, that's the effect. */
        float crawl_r = blend_factor * motion_mask;
        float crawl_g = blend_factor * motion_mask;
        float crawl_b = blend_factor * motion_mask;
        float bf_r = max(crawl_r, blend_r);
        float bf_g = max(crawl_g, blend_g);
        float bf_b = max(crawl_b, blend_b);
        /* Clamp to valid mix range. */
        bf_r = clamp(bf_r, 0.0, 0.95);
        bf_g = clamp(bf_g, 0.0, 0.95);
        bf_b = clamp(bf_b, 0.0, 0.95);

        rg.x = mix(rg.x, rg_p.x, bf_r);  /* R */
        rg.y = mix(rg.y, rg_p.y, bf_g);  /* G */
        ba.x = mix(ba.x, ba_p.x, bf_b);  /* B */
    }

    imageStore(out_tex, ivec2(x, y), vec4(rg.x, rg.y, ba.x, 1.0));
}
