/* NTSC line comb, operating only on the extracted chroma band.
 * Broadcast 1H = 2730 samples; NES lines contain 2728. The two-sample
 * horizontal offset is intentional. Y + C always reconstructs the input.
 * Modes describe generic topologies, not bit-exact commercial decoder ICs. */
#version 450
layout(local_size_x=256) in;
layout(set=0,binding=0) readonly buffer Signal { float signal_in[]; };
layout(set=0,binding=1) readonly buffer Band { float band[]; };
layout(set=1,binding=0) writeonly buffer Y { float y_out[]; };
layout(set=1,binding=1) writeonly buffer C { float c_out[]; };
layout(set=2,binding=0) uniform Params {
    uint count, samples_per_line, mode;
    float blend;
    uint delay_samples;
};
void main() {
    uint t=gl_GlobalInvocationID.x;
    if(t>=count) return;
    uint delay=delay_samples>0u ? delay_samples : samples_per_line;
    uint before=t>=delay ? t-delay : t;
    uint after=t+delay<count ? t+delay : t;
    float b=band[t], c=0.0;
    if(mode==1u) c=0.5*(b-band[before]); // two scanlines / one delay
    if(mode==3u) c=0.5*b-0.25*(band[before]+band[after]); // three / two delays
    if(mode==2u) {
        // Correlation can be same-phase (fine luma) OR opposite-phase
        // (chroma). Rejecting the former sends white detail into C.
        // Compare energy over a carrier cycle so the decision does not
        // switch at each carrier zero crossing. This is a generic
        // adaptive separator, not the proprietary MC141627 algorithm.
        vec2 low_error=vec2(0), same_error=vec2(0), opposite_error=vec2(0);
        int start=int(t/samples_per_line*samples_per_line);
        int end=min(start+int(samples_per_line),int(count))-1;
        for(int k=0;k<4;k++) {
            uint q=uint(clamp(int(t)+3*k-4,start,end));
            uint p=q>=delay ? q-delay : q;
            uint n=q+delay<count ? q+delay : q;
            float band_q=band[q];
            vec2 bands=vec2(band[p],band[n]);
            vec2 low_delta=vec2(signal_in[q]-band_q)
                -(vec2(signal_in[p],signal_in[n])-bands);
            low_error+=low_delta*low_delta;
            same_error+=(vec2(band_q)-bands)*(vec2(band_q)-bands);
            opposite_error+=(vec2(band_q)+bands)*(vec2(band_q)+bands);
        }
        vec2 error=sqrt(low_error*.25)
            +sqrt(min(same_error,opposite_error)*.25);
        float wp=1.0-smoothstep(0.03,0.18,error.x);
        float wn=1.0-smoothstep(0.03,0.18,error.y);
        if(before==t) wp=0.0;
        if(after==t) wn=0.0;
        float sum=wp+wn;
        float comb=0.5*(wp*(b-band[before])+wn*(b-band[after]))/max(sum,1e-6);
        c=mix(b,comb,min(sum,1.0));
    }
    c*=blend;
    c_out[t]=c;
    y_out[t]=signal_in[t]-c;
}
