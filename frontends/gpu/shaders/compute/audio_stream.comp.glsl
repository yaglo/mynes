#version 450
// Audio is temporal, not a set of independent scanlines. Keep every IIR,
// oscillator and noise state across blocks; CPU and GPU use the same layout.
layout(local_size_x = 1) in;
layout(set = 0, binding = 0) readonly buffer Input { float src[]; };
layout(set = 1, binding = 0) buffer Output { float dst[]; };
layout(set = 2, binding = 0) uniform Params {
    uint count; uint flags; uint pad0; uint pad1;
    vec4 rc[6];
    vec4 speaker[4];
    vec4 effects;
    vec4 harmonics;
    vec4 clip;
};
float state[16];
float rail_clip(float y, float window, float knee) {
    float lin = window * (1.0 - knee), a = abs(y);
    if (a <= lin) return y;
    float o = lin + window * knee * tanh((a - lin) / (window * knee));
    return y < 0.0 ? -o : o;
}
float filter_rc(int i, float x) {
    float y = rc[i].z != 0.0 ? rc[i].x * (state[2*i+1] + x - state[2*i])
                             : rc[i].x * state[2*i+1] + rc[i].y * x;
    state[2*i] = x; state[2*i+1] = y;
    return rc[i].w != 0.0 ? y : x;
}
void main() {
    for (int i=0; i<16; ++i) state[i] = src[i];
    float phase = src[16];
    uint rng = floatBitsToUint(src[17]);
    if (rng == 0u) rng = 42u;
    for (uint n=0u; n<count; ++n) {
        float y = src[18u+n];
        for (int i=0; i<3; ++i) y = filter_rc(i,y);
        if ((flags & 1u) != 0u) y = tanh(y * effects.x) / effects.x;
        if ((flags & 16u) != 0u) y = rail_clip(y, clip.x, clip.y);
        if ((flags & 2u) != 0u) y += effects.y * (sin(phase) + harmonics.x*sin(2.0*phase) + harmonics.y*sin(3.0*phase));
        phase += effects.z;
        if (phase >= 6.28318530718) phase -= 6.28318530718;
        rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5;
        if ((flags & 4u) != 0u) y += (float(rng >> 8)/8388608.0 - 1.0)*effects.w;
        for (int i=3; i<6; ++i) y = filter_rc(i,y);
        if ((flags & 8u) != 0u) for (int i=0; i<2; ++i) {
            vec4 b = speaker[2*i];
            float z = b.x*y + state[12+2*i];
            state[12+2*i] = b.y*y - b.w*z + state[13+2*i];
            state[13+2*i] = b.z*y - speaker[2*i+1].x*z;
            y = z;
        }
        dst[18u+n] = y;
    }
    for (int i=0; i<16; ++i) dst[i] = state[i];
    dst[16] = phase; dst[17] = uintBitsToFloat(rng);
}
