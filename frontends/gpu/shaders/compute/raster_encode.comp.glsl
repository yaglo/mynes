/* Expand active PPU voltage samples into complete horizontal lines.
 * The line buffer starts at the preceding H-sync (PPU dot 277), so the
 * receiver processes sync and burst before the associated active picture.
 * NTSC dot timings: NESdev NTSC_video / reverse-engineered PPU H decoder. */
#version 450
layout(local_size_x=256) in;
layout(set=0,binding=0) readonly buffer Active { float picture[]; };
layout(set=0,binding=1) readonly buffer SourceY { float source_y[]; };
layout(set=1,binding=1) writeonly buffer RasterY { float raster_y[]; };
layout(set=1,binding=0) writeonly buffer Raster { float raster[]; };
layout(set=2,binding=0) uniform Params {
    uint count, full_width, active_width, samples_per_dot;
    float phase_base, line_phase;
    uint region, lines, separate_yc;
};
void main() {
    uint i=gl_GlobalInvocationID.x;
    if(i>=count) return;
    uint line=i/full_width, x=i%full_width;
    uint dot=x/samples_per_dot;
    uint start=65u*samples_per_dot;
    float value=0.0;
    // Levels relative to blanking and white; terminated 2C02 measurements.
    float sync=-264.0/788.0;
    if(dot<25u) value=sync;
    if(dot>=29u && dot<44u) {
        float p=phase_base+float(line)*line_phase+(float(x)-float(start));
        float hue=(region==1u) ? (((line&1u)==1u) ? 4.0 : 7.0) : 8.0;
        bool high=mod(p+hue,12.0)<6.0;
        value=high ? 212.0/788.0 : -164.0/788.0;
    }
    if(line<240u && x>=start && x<start+active_width)
        value=picture[line*active_width+x-start];
    raster[i]=value;
    if (separate_yc != 0u) {
        float y=dot<25u ? sync : 0.0; // Sync travels on Y; burst travels on C.
        if (line<240u && x>=start && x<start+active_width)
            y=source_y[line*active_width+x-start];
        raster_y[i]=y;
    }
}
