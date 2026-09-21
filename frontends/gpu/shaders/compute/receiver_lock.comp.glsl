/* Measure sync trailing edge, gated porch and burst after the analog path.
 * The search window rejects sub-black picture codes and isolated noise. */
#version 450
layout(local_size_x=256) in;
layout(set=0,binding=0) readonly buffer Raster { float raster[]; };
layout(set=1,binding=0) writeonly buffer Measurement { vec4 measurement[]; };
layout(set=2,binding=0) uniform Params {
    uint count, full_width, samples_per_dot, region;
};
const float TAU=6.28318530718;
void main() {
    uint line=gl_GlobalInvocationID.x;
    if(line>=count) return;
    uint base=line*full_width, spp=samples_per_dot;
    float black=0.0, tip=0.0;
    for(uint x=334u*spp;x<338u*spp;x++) black+=raster[base+x];
    black/=float(4u*spp);
    for(uint x=8u*spp;x<16u*spp;x++) tip+=raster[base+x];
    tip/=float(8u*spp);
    float threshold=(black+tip)*0.5;
    bool vertical=tip<black-0.08;
    for(uint dot=80u;dot<=300u;dot+=20u) vertical=vertical && raster[base+dot*spp]<threshold;
    if(vertical) { measurement[line]=vec4(0,black,-1,0); return; }
    float offset=0.0;
    bool found=false;
    if(tip<black-0.08) for(uint x=18u*spp;x<32u*spp;x++) {
        float a=raster[base+x-1u], b=raster[base+x];
        if(a<threshold && b>=threshold) {
            offset=float(x)-1.0+(threshold-a)/max(b-a,1e-6)-(25.0*float(spp)-0.5);
            found=true; break;
        }
    }
    if(!found) { measurement[line]=vec4(0,black,0,1000000); return; }
    int shift=int(round(offset));
    black=0.0;
    for(uint x=46u*spp;x<49u*spp;x++) black+=raster[base+uint(int(x)+shift)];
    black/=float(3u*spp);
    uint start=uint(int(31u*spp)+shift);
    uint n=(11u*spp/12u)*12u;
    float c=0.0,s=0.0;
    for(uint x=0u;x<n;x++) {
        float p=float(int(start+x)-int(65u*spp))*TAU/12.0;
        float v=raster[base+start+x]-black;
        c+=v*cos(p); s+=v*sin(p);
    }
    float amplitude=2.0*length(vec2(c,s))/float(n);
    float burst_axis=(region==1u) ? (((line&1u)==1u) ? 1.5 : 4.5) : 5.5;
    measurement[line]=vec4(atan(-s,c)-burst_axis*TAU/12.0,black,amplitude,offset);
}
