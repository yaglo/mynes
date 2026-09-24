/* Sync separator, keyed black level and burst measurement after the
 * analog path, one thread per line.
 *
 * Coarse pass: the composite averaged over one subcarrier cycle (12
 * samples), which keeps the burst and most noise out, sliced at 50% of
 * the pulse measured from the front porch before the line's own sync
 * (capped at half the NES sync depth in case picture sits there; the tip
 * itself is the minimum of a noisy mean and reads low under snow). The
 * first rising crossing over dots 8 to 46, about 3 us early to 4 us late
 * against the nominal edge at dot 25, finds the sync however far a VCR
 * has moved the line. A crossing counts only when the pulse lies before
 * it and the porch after it: under snow the averaged tip crosses the
 * level inside the pulse, and such a crossing has no porch behind it.
 *
 * Flywheel: a set gates its burst detector and clamp from the horizontal
 * flywheel, and the separator only feeds that loop. The previous frame's
 * loop position for the line (the reference buffer receiver_pll writes)
 * keys the gates when no plausible edge is found: the line then reports
 * KEYED instead of an offset, so the loops take its black and burst but
 * leave the horizontal state alone.
 *
 * Fine pass: the tip is the mean of 8 dots of the pulse's interior, 9 to
 * 17 dots before the coarse edge (the lowest average alone reads low under
 * noise), black is keyed behind it, and a half-dot mean within a dot of
 * the coarse edge is sliced at the same level. With the sync where the
 * NES puts it this is the slice of fixed windows at dots 8-16 and 46-49;
 * the windows follow the sync when a VCR moves it.
 *
 * Black: the mean of 2 whole subcarrier cycles (24 samples) from 21 to 24
 * dots after the trailing edge, between the NES burst's end (19 dots after
 * the edge) and its border (24 dots after, where a standard back porch
 * ends too). Over whole cycles a band-limited burst's tail leaves only the
 * fraction of a cycle its slope covers, which alternates with the
 * carrier's 120 degrees per line; the VHS deck's tail reaches 20 dots
 * after the edge, where a window would double the whole-line noise.
 * receiver_pll integrates the measurement over lines with the TV's clamp
 * time constant. A vertical pulse is reported with z = -1, a line without
 * a sync edge with w = 1e6, a line gated from the flywheel with w = 5e5. */
#version 450
layout(local_size_x=32) in;
layout(set=0,binding=0) readonly buffer Raster { float raster[]; };
layout(set=0,binding=1) readonly buffer Reference { vec4 reference[]; };
layout(set=1,binding=0) writeonly buffer Measurement { vec4 measurement[]; };
layout(set=2,binding=0) uniform Params {
    uint count, full_width, samples_per_dot, region;
};
const float TAU=6.28318530718;
const int CYCLE=12;               // samples per subcarrier cycle
const int BLACK_START_DOTS=21;
const float NES_SYNC=264.0/788.0;
const float KEYED=500000.0;

/* First rising crossing of level over x in [from, to) of the one-cycle
 * average centred on x (samples x-6 to x+5), sub-sample by linear
 * interpolation. A step at sample e crosses at e - 0.5, as the raw
 * crossing below does. Returns -1e9 if none. */
float crossing_cycle(uint base, int from, int to, float level) {
    float s=0.0;
    for(int k=from-7;k<from+5;k++) s+=raster[base+uint(k)];
    float a=s/float(CYCLE);
    for(int x=from;x<to;x++) {
        s+=raster[base+uint(x+5)]-raster[base+uint(x-7)];
        float b=s/float(CYCLE);
        if(a<level && b>=level) return float(x)-1.5+(level-a)/max(b-a,1e-6);
        a=b;
    }
    return -1e9;
}
/* The same on a four-sample mean, half a dot, about the edge's own rise
 * time: a separator slices a band-limited signal, and under snow a raw
 * sample crosses the level a dot early. A step at sample e crosses at
 * e - 0.5, as the cycle crossing does. */
float crossing_raw(uint base, int from, int to, float level) {
    float s=raster[base+uint(from-3)]+raster[base+uint(from-2)]+raster[base+uint(from-1)]+raster[base+uint(from)];
    float a=s*0.25;
    for(int x=from+1;x<to+1;x++) {
        s+=raster[base+uint(x)]-raster[base+uint(x-4)];
        float b=s*0.25;
        if(a<level && b>=level) return float(x)-2.5+(level-a)/max(b-a,1e-6);
        a=b;
    }
    return -1e9;
}
float window_mean(uint base, int from, int to) {
    float s=0.0;
    for(int x=from;x<to;x++) s+=raster[base+uint(x)];
    return s/float(max(to-from,1));
}
/* A trailing sync edge has the pulse's interior before it and the back
 * porch after it, both a few sigma from the slice level under snow. */
bool plausible_edge(uint base, int c0, int spp, float level) {
    float before=window_mean(base,max(c0-9*spp,0),max(c0-spp,1));
    float after=window_mean(base,c0+spp,c0+5*spp);
    return before<level && after>=level;
}
/* The first plausible crossing in [from, to); up to six candidates. */
float plausible_crossing(uint base, int from, int to, int spp, float level) {
    for(int attempt=0;attempt<6 && from<to;attempt++) {
        float c=crossing_cycle(base,from,to,level);
        if(c<-1e8) return c;
        if(plausible_edge(base,int(c),spp,level)) return c;
        from=int(c)+1;
    }
    return -1e9;
}
float keyed_black(uint base, float edge, int spp) {
    int start=int(round(edge+0.5))+BLACK_START_DOTS*spp;
    float black=0.0;
    for(int x=0;x<2*CYCLE;x++) black+=raster[base+uint(start+x)];
    return black/float(2*CYCLE);
}
void main() {
    uint line=gl_GlobalInvocationID.x;
    if(line>=count) return;
    uint base=line*full_width;
    int spp=int(samples_per_dot);
    float porch=0.0;
    for(int x=334*spp;x<338*spp;x++) porch+=raster[base+uint(x)];
    porch/=float(4*spp);
    float s=0.0;
    for(int k=0;k<CYCLE;k++) s+=raster[base+uint(k)];
    float tip=s;
    for(int x=CYCLE;x<50*spp;x++) { s+=raster[base+uint(x)]-raster[base+uint(x-CYCLE)]; tip=min(tip,s); }
    tip/=float(CYCLE);
    float threshold=porch-0.5*min(porch-tip,NES_SYNC);
    bool vertical=tip<porch-0.08;
    for(int dot=80;dot<=300;dot+=20) vertical=vertical && raster[base+uint(dot*spp)]<threshold;
    if(vertical) { measurement[line]=vec4(0,porch,-1,0); return; }
    /* The flywheel's position for this line, from the previous frame's loop. */
    bool locked=reference[count].z>0.0 && abs(reference[line].w)<8.0*float(spp);
    float predicted=25.0*float(spp)-0.5+(locked ? reference[line].w : 0.0);
    bool keyed=false;
    float coarse=-1e9;
    if(tip<porch-0.08) coarse=plausible_crossing(base,8*spp,46*spp,spp,threshold);
    if(coarse<-1e8) {
        if(!locked) { measurement[line]=vec4(0,porch,0,1000000); return; }
        keyed=true; coarse=predicted;
    }
    int c0=int(coarse);
    int t0=max(c0-17*spp,0), t1=max(c0-9*spp,t0+1);
    float pulse=0.0;
    for(int x=t0;x<t1;x++) pulse+=raster[base+uint(x)];
    pulse/=float(t1-t0);
    float edge=coarse;
    if(!keyed) {
        edge=crossing_raw(base,max(8*spp,c0-spp),min(46*spp,c0+spp+1),porch-0.5*min(porch-pulse,NES_SYNC));
        if(edge<-1e8) edge=coarse;
    }
    float black=keyed_black(base,edge,spp);
    float offset=edge-(25.0*float(spp)-0.5);
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
    measurement[line]=vec4(atan(-sn,c)-burst_axis*TAU/12.0,black,amplitude,keyed ? KEYED : offset);
}
