/* NTSC VHS recording/playback equivalent recovered response. The original
 * composite signal supplies both paths, so cross-colour is not removed by
 * using ideal RGB. Sync, porch and burst traverse the same timing/filtering.
 * No magnetic-domain, dropout, head-switch or FM-threshold simulation. */
#version 450
layout(local_size_x = 256) in;
layout(set = 0, binding = 0) readonly buffer Input { float signal_in[]; };
layout(set = 0, binding = 1) readonly buffer Taps { vec4 taps[]; };
layout(set = 1, binding = 0) writeonly buffer Output { float signal_out[]; };
layout(set = 2, binding = 0) uniform Params {
    uint count, samples_per_line, tap_count, frame_seed;
    float delay_samples, timebase_samples, phase_radians, noise;
};
float hash(uint v) {
    v^=v>>16; v*=0x7feb352du; v^=v>>15; v*=0x846ca68bu; v^=v>>16;
    return float(v>>8)*(2.0/16777216.0)-1.0;
}
shared float tile[640]; // output group + filter, timing and envelope-delay halo
float sample_line(uint base,float x,int first) {
    float p=clamp(x,0.0,float(samples_per_line-1u));
    uint lo=uint(p),hi=min(lo+1u,samples_per_line-1u);
    return mix(tile[int(base+lo)-first],tile[int(base+hi)-first],fract(p));
}
void main() {
    uint i=gl_GlobalInvocationID.x;
    int first=int(gl_WorkGroupID.x*256u)-192;
    for(uint k=gl_LocalInvocationID.x;k<640u;k+=256u)
        tile[k]=signal_in[clamp(first+int(k),0,int(count)-1)];
    barrier();
    if(i>=count) return;
    uint line=i/samples_per_line,base=line*samples_per_line;
    uint seed=frame_seed*0x9e3779b9u;
    // Smooth line-correlated residual, with no short frame-phase noise cycle.
    uint block=line/8u;
    float t=smoothstep(0.0,1.0,float(line%8u)/8.0);
    float wobble=mix(hash(seed+block),hash(seed+block+1u),t);
    float jitter=clamp(timebase_samples,0.0,32.0)*wobble;
    float phase=phase_radians*mix(hash(seed+block+719u),hash(seed+block+720u),t);
    float cs=cos(phase),sn=sin(phase),y=0,c=0;
    float x=float(i-base)+jitter;
    for(int k=0;k<int(tap_count);k++) {
        float p=x+float(k-int(tap_count)/2);
        vec4 h=taps[k];
        y+=sample_line(base,p,first)*h.x;
        c+=sample_line(base,p-clamp(delay_samples,-64.0,64.0),first)*(h.y*cs-h.z*sn);
    }
    // Short spatial filtering avoids one-sample white speckle. This is a
    // bounded playback-noise approximation, not an FM demodulator model.
    float n=.25*hash((i-1u)^seed)+.5*hash(i^seed)+.25*hash((i+1u)^seed);
    signal_out[i]=y+c+noise*n;
}
