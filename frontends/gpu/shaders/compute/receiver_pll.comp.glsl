/* Generic line oscillator and burst loop. Detector work is parallel; this
 * short serial pass carries receiver state through lines and absent bursts.
 * The black clamp is keyed: each line's back-porch measurement charges
 * the clamp with clamp_gain, 1 - exp(-1 / clamp lines), so a keyed clamp
 * capacitor settles over the TV's clamp time constant and line noise does
 * not become whole-line offsets. Burst constants remain generic.
 * h_response optionally constrains horizontal tracking to a specified
 * first-order time constant.
 *
 * h_pll selects a second-order horizontal PLL: e = sync - theta,
 * integrator += Ki g e, theta += Kp g e + integrator, with the detector gain
 * g raised for h_vblank_lines from vertical sync (TDA2579: head-change
 * jumps are restored within the vertical blanking). It free-runs through
 * lines without a sync edge and is never reset, so a VCR head switch bends
 * the top of the next field. Line n is deflected with the phase predicted
 * before its own sync is measured. The loop's integrator and V-blank count
 * live in reference[count + 1]. */
#version 450
layout(local_size_x=1) in;
layout(set=0,binding=0) readonly buffer Measurement { vec4 measurement[]; };
layout(set=1,binding=0) buffer Reference { vec4 reference[]; };
layout(set=2,binding=0) uniform Params {
    uint count, full_width, samples_per_dot, region;
    float h_response, h_kp, h_ki, h_vblank_gain;
    uint h_pll, h_vblank_lines; float clamp_gain; uint reserved1;
};
const float TAU=6.28318530718;
const float BURST_GAIN=0.05;
float wrap(float p) { return mod(p+TAU*0.5,TAU)-TAU*0.5; }
void main() {
    vec4 state=reference[count];
    vec4 loop=h_pll!=0u ? reference[count+1u] : vec4(0);
    float advance=float(full_width%12u)*TAU/12.0;
    /* The colour loop acquires once, when the set has never seen a burst;
     * from then on it tracks through retrace and across frames, so no
     * single noisy line at the top of a field sets the hue or the level. */
    bool acquire=state.z<=0.0;
    for(uint line=0u;line<count;line++) {
        vec4 m=measurement[line];
        state.x=wrap(state.x+advance);
        bool sync=abs(m.w)<8.0*float(samples_per_dot);
        /* A line gated from the flywheel has black and burst but no edge. */
        bool keyed=m.w>400000.0 && m.w<600000.0;
        float predicted=state.w;
        if(h_pll!=0u) {
            /* loop.x: integrator (samples per line), loop.y: lines since
             * vertical sync began, loop.z: 1 while in vertical sync. */
            bool vertical=m.z<0.0;
            if(vertical && loop.z==0.0) loop.y=0.0;
            loop.z=vertical ? 1.0 : 0.0;
            float g=loop.y<float(h_vblank_lines) ? h_vblank_gain : 1.0;
            loop.y=min(loop.y+1.0,1e6);
            if(sync && !vertical) {
                float e=m.w-state.w;
                loop.x+=h_ki*g*e;
                state.w+=h_kp*g*e+loop.x;
            } else state.w+=loop.x;
        }
        if(m.z<0.0) { // vertical retrace: hold the oscillator and the loop's state
            reference[line]=vec4(state.x,m.y,0,h_pll!=0u ? predicted : state.w);
            continue;
        }
        if(sync||keyed) {
            // The keyed clamp charges on every line with a sync; only a
            // receiver that has never locked takes the first line outright.
            // A specified horizontal loop keeps its state when the *colour*
            // loop reacquires, including across frame boundaries/retrace,
            // and so does the clamp: its capacitor holds through retrace.
            float horizontal=(h_pll!=0u || keyed) ? state.w : mix(state.w,m.w,h_response);
            bool fresh=state.y==0.0 && state.z==0.0 && state.w==0.0;
            state.y=fresh ? m.y : mix(state.y,m.y,clamp_gain);
            if(acquire && m.z>0.01 && sync) { state.xz=m.xz; if(fresh) state.w=m.w; acquire=false; }
            else {
                if(sync) state.w=mix(state.w,m.w,0.25);
                /* Burst APC and ACC: a first-order loop of about 20 lines
                 * (1.3 ms), the class of a one-chip decoder's application
                 * filter, instead of a quarter of each line's noisy
                 * measurement; at 30 dB CNR this holds the hue within a
                 * fraction of a degree line to line. */
                if(m.z>0.01) {
                    state.x=wrap(state.x+BURST_GAIN*wrap(m.x-state.x));
                    state.z=mix(state.z,m.z,BURST_GAIN);
                } else state.z*=0.65;
            }
            if(h_response>0.0 || h_pll!=0u || keyed) state.w=horizontal;
        } else state.z*=0.65;
        reference[line]=h_pll!=0u ? vec4(state.xyz,predicted) : state;
    }
    reference[count]=state;
    if(h_pll!=0u) reference[count+1u]=loop;
}
