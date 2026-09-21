/* Generic line oscillator and burst loop. Detector work is parallel; this
 * short serial pass carries receiver state through lines and absent bursts.
 * Loop constants describe a stable generic receiver, not a specific IC. */
#version 450
layout(local_size_x=1) in;
layout(set=0,binding=0) readonly buffer Measurement { vec4 measurement[]; };
layout(set=1,binding=0) buffer Reference { vec4 reference[]; };
layout(set=2,binding=0) uniform Params { uint count, full_width, samples_per_dot, region; };
const float TAU=6.28318530718;
float wrap(float p) { return mod(p+TAU*0.5,TAU)-TAU*0.5; }
void main() {
    vec4 state=reference[count];
    float advance=float(full_width%12u)*TAU/12.0;
    bool acquire=true;
    for(uint line=0u;line<count;line++) {
        vec4 m=measurement[line];
        state.x=wrap(state.x+advance);
        bool sync=abs(m.w)<8.0*float(samples_per_dot);
        if(m.z<0.0) { // vertical retrace: hold oscillator, reacquire after sync
            reference[line]=vec4(state.x,m.y,0,state.w);
            acquire=true;
            continue;
        }
        if(sync) {
            if(acquire && m.z>0.01) { state=m; acquire=false; }
            else {
                state.y=mix(state.y,m.y,0.35);
                state.w=mix(state.w,m.w,0.25);
                if(m.z>0.01) {
                    state.x=wrap(state.x+0.25*wrap(m.x-state.x));
                    state.z=mix(state.z,m.z,0.25);
                } else state.z*=0.65;
            }
        } else state.z*=0.65;
        reference[line]=state;
    }
    reference[count]=state;
}
