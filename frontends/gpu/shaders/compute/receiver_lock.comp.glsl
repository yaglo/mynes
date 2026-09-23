/* Sync separator, keyed black level and burst measurement after the
 * analog path, one thread per line.
 *
 * Slicing: the composite is averaged over one subcarrier cycle (12
 * samples) before the slicer, which keeps the burst and most of the noise
 * out of it, and the sync tip is the lowest such average over the first
 * 50 dots, so the slice level does not depend on where the sync arrived.
 * The trailing edge is the first rising crossing halfway between tip and
 * porch over dots 8 to 46, about 3 us early to 4 us late against the
 * nominal edge at dot 25 (a VCR moves lines by up to a few us). The first
 * pass takes the porch from the front porch window before the line's own
 * sync, capped at half the NES sync depth in case picture sits there; the
 * second pass slices against the back porch measured behind the edge it
 * found.
 *
 * Black: the mean of 3 whole subcarrier cycles (36 samples, 0.84 us) of
 * the back porch after the burst, from 20.5 to 25 dots behind the
 * trailing edge (the NES burst ends at 19, a standard burst at 16.5, and
 * standard blanking ends at 25.6). Over whole cycles what a band-limited
 * burst's tail leaves is the fraction of a cycle its slope covers, a few
 * hundredths of an IRE, so a VCR's timing does not turn into a level; the
 * 2C02's burst itself sits 3 IRE above blanking, which is why the window
 * does not cover it. receiver_pll integrates the measurement over lines
 * with the TV's clamp time constant. A vertical pulse is reported with
 * z = -1, a line without a sync edge with w = 1e6. */
#version 450
layout(local_size_x=32) in;
layout(set=0,binding=0) readonly buffer Raster { float raster[]; };
layout(set=1,binding=0) writeonly buffer Measurement { vec4 measurement[]; };
layout(set=2,binding=0) uniform Params {
    uint count, full_width, samples_per_dot, region;
};
const float TAU=6.28318530718;
const int CYCLE=12;               // samples per subcarrier cycle
const int BLACK_CYCLES=3;
const float NES_SYNC=264.0/788.0;

/* First rising crossing of level over x in [from, to) of the one-cycle
 * average centred on x (samples x-6 to x+5), sub-sample by linear
 * interpolation. A step at sample e crosses at e. Returns -1e9 if none. */
float crossing(uint base, int from, int to, float level) {
    float s=0.0;
    for(int k=from-7;k<from+5;k++) s+=raster[base+uint(k)];
    float a=s/float(CYCLE);
    for(int x=from;x<to;x++) {
        s+=raster[base+uint(x+5)]-raster[base+uint(x-7)];
        float b=s/float(CYCLE);
        if(a<level && b>=level) return float(x)-1.0+(level-a)/max(b-a,1e-6);
        a=b;
    }
    return -1e9;
}
void main() {
    uint line=gl_GlobalInvocationID.x;
    if(line>=count) return;
    uint base=line*full_width;
    int spp=int(samples_per_dot);
    float porch=0.0;
    for(int x=334*spp;x<338*spp;x++) porch+=raster[base+uint(x)];
    porch/=float(4*spp);
    // Sync tip: the lowest one-cycle average over the first 50 dots.
    float s=0.0;
    for(int k=0;k<CYCLE;k++) s+=raster[base+uint(k)];
    float tip=s;
    for(int x=CYCLE;x<50*spp;x++) { s+=raster[base+uint(x)]-raster[base+uint(x-CYCLE)]; tip=min(tip,s); }
    tip/=float(CYCLE);
    float threshold=tip+0.5*min(porch-tip,NES_SYNC);
    bool vertical=tip<porch-0.08;
    for(int dot=80;dot<=300;dot+=20) vertical=vertical && raster[base+uint(dot*spp)]<threshold;
    if(vertical) { measurement[line]=vec4(0,porch,-1,0); return; }
    if(tip>=porch-0.08) { measurement[line]=vec4(0,porch,0,1000000); return; }
    float edge=crossing(base,8*spp,46*spp,threshold);
    if(edge<-1e8) { measurement[line]=vec4(0,porch,0,1000000); return; }
    // Keyed black over whole cycles behind the edge, then slice again
    // against it.
    int start=int(round(edge))+20*spp+spp/2;
    float black=0.0;
    for(int x=0;x<BLACK_CYCLES*CYCLE;x++) black+=raster[base+uint(start+x)];
    black/=float(BLACK_CYCLES*CYCLE);
    float again=crossing(base,max(8*spp,int(edge)-spp),min(46*spp,int(edge)+spp+1),tip+0.5*(black-tip));
    if(again>-1e8) edge=again;
    float offset=edge-25.0*float(spp);
    int shift=int(round(offset));
    uint bstart=uint(int(31*spp)+shift);
    uint n=uint(11*spp/CYCLE)*uint(CYCLE);
    float c=0.0,sn=0.0;
    for(uint x=0u;x<n;x++) {
        float p=float(int(bstart+x)-int(65*spp))*TAU/12.0;
        float v=raster[base+bstart+x]-black;
        c+=v*cos(p); sn+=v*sin(p);
    }
    float amplitude=2.0*length(vec2(c,sn))/float(n);
    float burst_axis=(region==1u) ? (((line&1u)==1u) ? 1.5 : 4.5) : 5.5;
    measurement[line]=vec4(atan(-sn,c)-burst_axis*TAU/12.0,black,amplitude,offset);
}
