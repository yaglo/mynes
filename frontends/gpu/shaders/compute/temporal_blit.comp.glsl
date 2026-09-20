/* Frame-sampled phosphor decay in linear light. Optional motion-adaptive
 * display smoothing is separate from persistence; it is not a TV 3D comb. */
#version 450
layout(local_size_x = 16, local_size_y = 16) in;
layout(set = 0, binding = 0) readonly buffer CurBuf { uint cur_buf[]; };
layout(set = 0, binding = 1) readonly buffer PrevBuf { uint prev_buf[]; };
layout(set = 1, binding = 0, rgba16f) writeonly uniform image2D out_tex;
layout(set = 1, binding = 1) buffer HistoryBuf { uint history_buf[]; };
layout(set = 2, binding = 0) uniform Params {
    uint width;
    uint height;
    float blend_factor;
    float blend_r;
    float blend_g;
    float blend_b;
    float motion_threshold;
    uint history_valid;
};
vec3 current(uint i) {
    return vec3(unpackHalf2x16(cur_buf[i]), unpackHalf2x16(cur_buf[i+1u]).x);
}
vec3 previous(uint i) {
    return vec3(unpackHalf2x16(prev_buf[i]), unpackHalf2x16(prev_buf[i+1u]).x);
}
void main() {
    uvec2 p = gl_GlobalInvocationID.xy;
    if (p.x >= width || p.y >= height) return;
    uint i = (p.y * width + p.x) * 2u;
    vec3 drive = current(i);
    /* Avoid 18 neighborhood loads per pixel when smoothing is disabled. */
    if (history_valid != 0u && blend_factor > 0.0) {
        float motion = 1.0;
        if (motion_threshold > 0.0) {
            float delta = 0.0;
            for (int y = -1; y <= 1; ++y) for (int x = -1; x <= 1; ++x) {
                ivec2 q = clamp(ivec2(p) + ivec2(x,y), ivec2(0), ivec2(width,height)-1);
                uint j = (uint(q.y) * width + uint(q.x)) * 2u;
                vec3 d = abs(current(j) - previous(j));
                delta = max(delta, max(d.r, max(d.g, d.b)));
            }
            motion = 1.0 - smoothstep(motion_threshold * 0.5, motion_threshold * 1.5, delta);
        }
        drive = mix(drive, previous(i), clamp(blend_factor * motion, 0.0, 0.5));
    }
    vec3 light = max(drive, vec3(0.0));
    vec3 decay = clamp(vec3(blend_r, blend_g, blend_b), vec3(0.0), vec3(0.9999));
    if (history_valid != 0u && max(decay.r, max(decay.g, decay.b)) > 0.0) {
        vec3 history = vec3(unpackHalf2x16(history_buf[i]), unpackHalf2x16(history_buf[i+1u]).x);
        light = mix(light, history, decay);
    }
    history_buf[i] = packHalf2x16(light.rg);
    history_buf[i+1u] = packHalf2x16(vec2(light.b, 1.0));
    imageStore(out_tex, ivec2(p), vec4(max(light, vec3(0.0)), 1.0));
}
