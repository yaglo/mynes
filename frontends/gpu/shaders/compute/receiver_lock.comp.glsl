/* Burst-gated phase detector and back-porch DC restoration.
 * Reference is measured AFTER the console/cable/RF stages, never supplied
 * by the encoder. No burst => no chroma; black pixels do not kill color. */
#version 450
layout(local_size_x=256) in;
layout(set=0,binding=0) readonly buffer Raster { float raster[]; };
layout(set=1,binding=0) writeonly buffer Reference { vec4 reference[]; };
layout(set=2,binding=0) uniform Params {
    uint count, full_width, samples_per_dot, region;
};
const float TAU=6.28318530718;
void main() {
    uint line=gl_GlobalInvocationID.x;
    if(line>=count) return;
    uint base=line*full_width;
    // Ignore edges of the porch and burst to allow the analogue filters to settle.
    float black=0.0;
    uint b0=46u*samples_per_dot, b1=49u*samples_per_dot;
    for(uint x=b0;x<b1;x++) black+=raster[base+x];
    black/=float(b1-b0);
    // An integer number of carrier cycles excludes DC from the phase estimate.
    uint start=31u*samples_per_dot;
    uint n=((42u*samples_per_dot-start)/12u)*12u;
    float c=0.0,s=0.0;
    for(uint x=0u;x<n;x++) {
        float p=float(int(start+x)-int(65u*samples_per_dot))*TAU/12.0;
        float v=raster[base+start+x]-black;
        c+=v*cos(p); s+=v*sin(p);
    }
    float amplitude=2.0*length(vec2(c,s))/float(n);
    float burst_axis=(region==1u) ? (((line&1u)==1u) ? 1.5 : 4.5) : 5.5;
    float phase=atan(-s,c)-burst_axis*TAU/12.0;
    reference[line]=vec4(phase,black,amplitude,0.0);
}
