#version 450
// Audio is temporal, not a set of independent scanlines. Keep every IIR,
// oscillator and noise state across blocks; CPU and GPU use the same layout.
layout(local_size_x = 1) in;
layout(set = 0, binding = 0) readonly buffer Input { float src[]; };
layout(set = 1, binding = 0) buffer Output { float dst[]; };
layout(set = 2, binding = 0) uniform Params {
    uint count; uint flags; uint pad0; uint pad1;
    vec4 rc[5];
    vec4 speaker[4];
    vec4 effects;
    vec4 harmonics;
};
float state[14];
float filter_rc(int i, float x) {
    float y = rc[i].z != 0.0 ? rc[i].x * (state[2*i+1] + x - state[2*i])
                             : rc[i].x * state[2*i+1] + rc[i].y * x;
    state[2*i] = x; state[2*i+1] = y;
    return rc[i].w != 0.0 ? y : x;
}
void main() {
    for (int i=0; i<14; ++i) state[i] = src[i];
    float phase = src[14];
    uint rng = floatBitsToUint(src[15]);
    if (rng == 0u) rng = 42u;
    for (uint n=0u; n<count; ++n) {
        float y = src[16u+n];
        for (int i=0; i<3; ++i) y = filter_rc(i,y);
        if ((flags & 1u) != 0u) y = tanh(y * effects.x) / effects.x;
        if ((flags & 2u) != 0u) y += effects.y * (sin(phase) + harmonics.x*sin(2.0*phase) + harmonics.y*sin(3.0*phase));
        phase += effects.z;
        if (phase >= 6.28318530718) phase -= 6.28318530718;
        rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5;
        if ((flags & 4u) != 0u) y += (float(rng >> 8)/8388608.0 - 1.0)*effects.w;
        for (int i=3; i<5; ++i) y = filter_rc(i,y);
        if ((flags & 8u) != 0u) for (int i=0; i<2; ++i) {
            vec4 b = speaker[2*i];
            float z = b.x*y + state[10+2*i];
            state[10+2*i] = b.y*y - b.w*z + state[11+2*i];
            state[11+2*i] = b.z*y - speaker[2*i+1].x*z;
            y = z;
        }
        dst[16u+n] = y;
    }
    for (int i=0; i<14; ++i) dst[i] = state[i];
    dst[14] = phase; dst[15] = uintBitsToFloat(rng);
}
