/*
 * Halation Blur — Separable Gaussian (Fragment Shader)
 * =====================================================
 *
 * Two-pass separable Gaussian blur for CRT halation simulation.
 * Light scatters through the glass faceplate, creating a soft glow
 * around bright areas.
 *
 * Pass 1 (horizontal): extracts bright pixels (if do_threshold == 1)
 *                       and blurs horizontally.
 * Pass 2 (vertical):   blurs the result vertically. No thresholding.
 *
 * sigma = radius * 0.4 gives a kernel that decays to ~1% at the edge.
 *
 * Dispatch: bind fullscreen.vert, draw 3 vertices per pass.
 * Pass 1: direction = (1/w, 0), do_threshold = 1
 * Pass 2: direction = (0, 1/h), do_threshold = 0
 */

#version 450

layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 frag_color;

layout(set = 0, binding = 0) uniform sampler2D tex_input;

layout(set = 1, binding = 0) uniform BlurParams {
    vec2  direction;        /* (1/w, 0) for H; (0, 1/h) for V */
    int   radius;           /* kernel radius in texels (e.g. 12-32) */
    float threshold;        /* brightness threshold for extraction */
    int   do_threshold;     /* 1 = extract bright pixels first pass */
};

void main() {
    float sigma = float(radius) * 0.4;
    float inv_2sigma2 = 1.0 / (2.0 * sigma * sigma);

    /*
     * Center sample.
     */
    vec3 center = texture(tex_input, uv).rgb;

    if (do_threshold == 1) {
        /* Extract: only keep pixels above the brightness threshold. */
        float luma = dot(center, vec3(0.2126, 0.7152, 0.0722));
        float extract = max(luma - threshold, 0.0) / max(1.0 - threshold, 0.001);
        center *= extract;
    }

    /* Gaussian weight for center tap (offset = 0). */
    float w0 = 1.0;
    vec3 accum = center * w0;
    float weight_sum = w0;

    /*
     * Symmetric taps: sample both +i and -i with the same weight.
     * This halves the number of exp() calls.
     */
    for (int i = 1; i <= radius; i++) {
        float fi = float(i);
        float w = exp(-fi * fi * inv_2sigma2);

        vec2 offset = direction * fi;
        vec3 s_pos = texture(tex_input, uv + offset).rgb;
        vec3 s_neg = texture(tex_input, uv - offset).rgb;

        if (do_threshold == 1) {
            float luma_p = dot(s_pos, vec3(0.2126, 0.7152, 0.0722));
            float luma_n = dot(s_neg, vec3(0.2126, 0.7152, 0.0722));
            float ext_p = max(luma_p - threshold, 0.0) / max(1.0 - threshold, 0.001);
            float ext_n = max(luma_n - threshold, 0.0) / max(1.0 - threshold, 0.001);
            s_pos *= ext_p;
            s_neg *= ext_n;
        }

        accum += (s_pos + s_neg) * w;
        weight_sum += 2.0 * w;
    }

    frag_color = vec4(accum / weight_sum, 1.0);
}
