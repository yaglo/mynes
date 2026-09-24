/* Convert gun voltage to emitted current once, before spatial beam spread. */
#version 450
layout(local_size_x=256) in;
layout(set=0,binding=0) readonly buffer Voltage { float voltage[]; };
layout(set=1,binding=0) writeonly buffer Current { float current[]; };
layout(set=2,binding=0) uniform Params {
    uint count;
    float gamma_r, gamma_g, gamma_b;
    uint samples_per_line, frame_seed;
    float noise_level, samples_per_pixel;
    float black_floor;
    float apl_bias;
};
uint mix32(uint seed) {
    seed ^= seed >> 16u; seed *= 0x7feb352du;
    seed ^= seed >> 15u; seed *= 0x846ca68bu; seed ^= seed >> 16u;
    return seed;
}
float noise(uint seed) { return float(mix32(seed) & 65535u) / 65535.0 - 0.5; }
void main() {
    uint i=gl_GlobalInvocationID.x;
    if(i>=count) return;
    vec3 v=vec3(voltage[i*3u],voltage[i*3u+1u],voltage[i*3u+2u]);
    // Receiver voltage noise is tied to scan time, never drawable pixels.
    // Smooth at the video bandwidth, then let gun transfer and both spot
    // axes shape it. RF snow itself is generated earlier in the RF stage.
    if(noise_level > 0.0) {
        // Nested mixing of frame, line and position. The old linear seed
        // (line*7919 + frame*6271 + x*1999) made frame f+2 a copy of frame
        // f shifted by 85 lines and 343 steps, so the grain repeated.
        float sample_x=float(i % samples_per_line)/max(samples_per_pixel*0.5,1.0);
        uint row=mix32(mix32(frame_seed ^ 0x9e3779b9u) ^ (i/samples_per_line));
        uint x=uint(floor(sample_x));
        float n=mix(noise(row ^ mix32(x)),noise(row ^ mix32(x+1u)),smoothstep(0.0,1.0,fract(sample_x)));
        v=max(v+vec3(noise_level*n),0.0);
    }
    // Residual gun drive must deposit light through the same spot as the
    // picture. Adding a luminous floor after deposition fills scanline gaps.
    vec3 drive=max(max(v,vec3(max(black_floor,0.0)))+apl_bias,vec3(0.0));
    vec3 light=pow(drive,vec3(gamma_r,gamma_g,gamma_b));
    current[i*3u]=light.r; current[i*3u+1u]=light.g; current[i*3u+2u]=light.b;
}
