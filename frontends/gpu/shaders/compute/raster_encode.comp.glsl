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
    float sync_level, burst_amp;
    uint burst_sine;
    vec4 backdrop[3], gray_backdrop[3];
    /* 1: each line's border waveforms follow the picture's samples in
     * picture[] (dac_2c02.comp.glsl, border_table); 0: backdrop throughout. */
    uint border_table;
};
/* Border voltage at carrier phase p of raster line `line`: left of the
 * picture (with the hue-0 pulse at dot 49) or right of it. */
float border_value(uint line, uint dot, uint x, uint start, int phase) {
    if (border_table == 0u)
        return dot == 49u ? gray_backdrop[phase/4][phase%4] : backdrop[phase/4][phase%4];
    uint base = 240u * active_width + line * 36u;
    uint part = dot == 49u ? 12u : (x < start ? 0u : 24u);
    return picture[base + part + uint(phase)];
}
float border_mean(uint line, uint x, uint start) {
    float y = 0.0;
    for (int k = 0; k < 12; k++) y += border_table == 0u ? backdrop[k/4][k%4] : border_value(line, 50u, x, start, k);
    return y / 12.0;
}
void main() {
    uint i=gl_GlobalInvocationID.x;
    if(i>=count) return;
    uint line=i/full_width, x=i%full_width;
    uint dot=x/samples_per_dot;
    uint start=65u*samples_per_dot;
    float value=0.0;
    // Levels relative to blanking and white: terminated 2C02 measurements by
    // default (-264/788 sync, a 212/-164 square burst), or an encoder IC's
    // -40 IRE sync and 40 IRE sine burst at the same 210-degree phase as the
    // 2C02's hue-8 square wave (see encoder_rgb.comp.glsl).
    float sync=sync_level;
    if(dot<25u) value=sync;
    if(dot>=29u && dot<44u) {
        float p=phase_base+float(line)*line_phase+(float(x)-float(start));
        if(burst_sine!=0u) {
            value=-burst_amp*cos(6.28318530718*(p-1.0)/12.0);
        } else {
            float hue=(region==1u) ? (((line&1u)==1u) ? 4.0 : 7.0) : 8.0;
            bool high=mod(p+hue,12.0)<6.0;
            value=high ? 212.0/788.0 : -164.0/788.0;
        }
    }
    bool vertical_sync = region == 0u ? (line >= 245u && line < 248u)
                                     : (line >= 270u && line < 273u);
    bool picture_line = line < 240u && (region == 0u || line > 0u);
    // NTSC border voltage follows the backdrop. PAL blanks its border.
    if (region == 0u && line < 242u && dot >= 49u && dot < 332u) {
        int phase = int(mod(phase_base + float(line)*line_phase + float(x)-float(start), 12.0));
        value = border_value(line, dot, x, start, phase);
    }
    if(picture_line && x>=start && x<start+active_width) {
        bool pal_border = region == 1u && (x < start+2u*samples_per_dot || x >= start+active_width-2u*samples_per_dot);
        value = pal_border ? 0.0 : picture[line*active_width+x-start];
    }
    // NES uses three whole-line sync pulses: no interlace equalizing half-lines.
    if (vertical_sync) value = dot < 318u ? sync : 0.0;
    raster[i]=value;
    if (separate_yc != 0u) {
        float y=dot<25u ? sync : 0.0; // Sync travels on Y; burst travels on C.
        if (picture_line && x>=start && x<start+active_width) {
            bool pal_border = region == 1u && (x < start+2u*samples_per_dot || x >= start+active_width-2u*samples_per_dot);
            y = pal_border ? 0.0 : source_y[line*active_width+x-start];
        }
        if (region == 0u && line < 242u && dot >= 50u && dot < 332u &&
            (!picture_line || x < start || x >= start+active_width)) {
            y = border_mean(line, x, start);
        }
        if (vertical_sync) y = value;
        raster_y[i]=y;
    }
}
