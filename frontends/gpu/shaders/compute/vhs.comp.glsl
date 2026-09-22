/* Equivalent recovered NTSC tape response, before TV sync/burst recovery.
 * This is a baseband model, not a magnetic tape or FM-demodulator simulation.
 * Noise, transport drift and localized carrier loss have independent controls.
 */
#version 450
layout(local_size_x = 256) in;
layout(set=0,binding=0) readonly buffer Input { float signal_in[]; };
layout(set=0,binding=1) readonly buffer Taps { vec4 taps[]; };
layout(set=1,binding=0) writeonly buffer Output { float signal_out[]; };
layout(set=2,binding=0) uniform Params {
    uint count, samples_per_line, tap_count, frame_seed;
    float delay_samples, timebase_samples, phase_radians, noise;
    float sample_rate, drift_frames, luma_noise_rms, chroma_noise_rms;
    float head_switch_samples, dropout_rate, dropout_depth, frame_rate;
};
float hash(uint v) {
    v^=v>>16; v*=0x7feb352du; v^=v>>15; v*=0x846ca68bu; v^=v>>16;
    return float(v>>8)*(2.0/16777216.0)-1.0;
}
// Continuous bounded transport field in tape time, independent of host pixels.
float drift(uint row, uint salt) {
    float time=float(frame_seed)/max(drift_frames,1.0);
    uint epoch=uint(floor(time));
    float t=smoothstep(0.0,1.0,fract(time));
    return mix(hash(row*0x85ebca6bu+epoch*0x9e3779b9u+salt),
               hash(row*0x85ebca6bu+(epoch+1u)*0x9e3779b9u+salt),t);
}
// Smooth horizontal noise envelope, with unit ensemble RMS at every phase.
// Correlation scale is in signal samples, not output/render pixels.
float grain(float x, float spacing, uint seed) {
    float p=x/max(spacing,1.0);
    uint cell=uint(floor(p));
    float t=smoothstep(0.0,1.0,fract(p));
    float variance=((1.0-t)*(1.0-t)+t*t)/3.0;
    return mix(hash(cell*0x85ebca6bu+seed),hash((cell+1u)*0x85ebca6bu+seed),t)
           *inversesqrt(variance);
}
// 256 outputs + 192 samples on either side covers the 129-tap filters,
// the bounded TOTAL timing displacement (32), and chroma delay (64).
shared float tile[640];
float sample_line(uint base, float x, int first) {
    float p=clamp(x,0.0,float(samples_per_line-1u));
    uint lo=uint(p),hi=min(lo+1u,samples_per_line-1u);
    return mix(tile[int(base+lo)-first],tile[int(base+hi)-first],fract(p));
}
void main() {
    uint i=gl_GlobalInvocationID.x;
    int first=int(gl_WorkGroupID.x*256u)-192;
    for(uint k=gl_LocalInvocationID.x;k<640u;k+=256u)
        tile[k]=signal_in[clamp(first+int(k),0,int(count)-1)];
    barrier();
    if(i>=count) return;
    uint line=i/samples_per_line,base=line*samples_per_line;
    uint seed=frame_seed*0x9e3779b9u;
    uint block=line/8u;
    float t=smoothstep(0.0,1.0,float(line%8u)/8.0);
    float wobble=mix(drift(block,17u),drift(block+1u,17u),t);
    float phase=phase_radians*mix(drift(block,719u),drift(block+1u,719u),t);
    // NES 240p recorded as successive fields. Switching affects the last
    // active lines; ordinary CRT overscan often hides this region.
    float switch_band=line<240u ? smoothstep(234.0,237.0,float(line)) : 0.0;
    float head=clamp(head_switch_samples,0.0,32.0)*switch_band*
        (0.65*hash((frame_seed/2u)+917u)+0.35*drift(0u,911u));
    float jitter=clamp(timebase_samples*(.8*wobble+.2*drift(line,43u))+head,-32.0,32.0);
    float cs=cos(phase),sn=sin(phase),y=0,c=0;
    float x=float(i-base)+jitter;
    for(int k=0;k<int(tap_count);k++) {
        float p=x+float(k-int(tap_count)/2);
        vec4 h=taps[k];
        y+=sample_line(base,p,first)*h.x;
        c+=sample_line(base,p-clamp(delay_samples,-64.0,64.0),first)*(h.y*cs-h.z*sn);
    }
    float n=.25*hash((i-1u)^seed)+.5*hash(i^seed)+.25*hash((i+1u)^seed);
    float playback_noise=noise*n; // Retain older presets' noise semantics.
    float fs=max(sample_rate,1.0);
    float px=float(i-base);
    uint row_seed=seed+line*0x27d4eb2du;
    if(luma_noise_rms>0.0) {
        float short_grain=grain(px,fs/5e6,row_seed+157u);
        float streak=grain(px,fs/3e5,row_seed+239u);
        playback_noise+=luma_noise_rms*(.9*short_grain+.4358899*streak);
    }
    if(chroma_noise_rms>0.0) {
        float a=6.28318530718*3579545.454545*px/fs;
        float ni=grain(px,fs/7e5,row_seed+401u);
        float nq=grain(px,fs/7e5,row_seed+811u);
        playback_noise+=chroma_noise_rms*(ni*cos(a)+nq*sin(a));
    }
    // Sparse, horizontal loss of recovered carrier. Deliberately an
    // adjustable phenomenological defect, not a claim to model FM threshold.
    float loss=0.0;
    if(dropout_rate>0.0 && .5+.5*hash(seed+991u)<dropout_rate/max(frame_rate,1.0)) {
        float row=floor((.5+.5*hash(seed+997u))*240.0);
        float center=(.5+.5*hash(seed+1009u))*float(samples_per_line);
        float length=fs*(1e-6+3e-6*(.5+.5*hash(seed+1013u)));
        float spot=max(0.0,1.0-abs(px-center)/max(length,1.0));
        loss=clamp(dropout_depth,0.0,1.0)*spot*max(0.0,1.0-abs(float(line)-row));
    }
    signal_out[i]=(y+c)*(1.0-loss)+playback_noise+loss*.08*n;
}
