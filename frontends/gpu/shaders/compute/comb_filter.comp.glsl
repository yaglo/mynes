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
        // Use the vertically correlated side at a boundary; fall back to
        // horizontal separation if neither side matches. Low-band luma
        // and opposite-phase chroma both contribute to the decision.
        float low=signal_in[t]-b;
        float ep=abs(low-(signal_in[before]-band[before]))+abs(b+band[before]);
        float en=abs(low-(signal_in[after]-band[after]))+abs(b+band[after]);
        float wp=1.0-smoothstep(0.03,0.18,ep);
        float wn=1.0-smoothstep(0.03,0.18,en);
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
