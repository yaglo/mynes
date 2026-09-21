/* CRT gun/rail loading after RGB amplification. Fast video-rail recovery
 * makes causal horizontal streaks; the slower shared supply follows line
 * current and carries through retrace. Generic RC response, not a named TV. */
#version 450
layout(local_size_x=256) in;
layout(set=1,binding=0) buffer RGB { float rgb[]; };
layout(set=1,binding=1) buffer Load { float load_map[]; };
layout(set=2,binding=0) uniform Params {
    uint width, spp;
    float gamma, strength, dot_seconds;
    uint mode;
    float elapsed_frames;
    uint region;
    float black_droop, recovery_us;
    vec2 pad;
};
const uint LINE_MEANS=256u*240u;
void main() {
    uint line=gl_GlobalInvocationID.x;
    if(mode==1u) {
        if(line!=0u) return;
        float state=load_map[LINE_MEANS+240u];
        float line_seconds=dot_seconds*341.0;
        float blank_lines=region==1u ? 72.0 : 22.0;
        state*=exp(-line_seconds*blank_lines/0.002);
        // Approximate unobserved pictures using the current picture load.
        // Recovery uses elapsed emulated time rather than monitor refresh.
        for(uint row=0u;row<240u;row++) {
            float target=load_map[LINE_MEANS+row]*(256.0/341.0);
            float tau=target>state ? 0.001 : 0.002;
            state=mix(target,state,exp(-line_seconds*max(elapsed_frames,1.0)/tau));
            load_map[LINE_MEANS+row]=state;
        }
        load_map[LINE_MEANS+240u]=state;
        return;
    }
    if(line>=240u) return;
    float rail=0.0, total=0.0, bias=0.0;
    float response=1.0-exp(-dot_seconds/0.000012); // 12 us video-rail recovery
    float bias_response=1.0-exp(-dot_seconds/(max(recovery_us,1.0)*1e-6));
    for(uint pixel=0u;pixel<256u;pixel++) {
        float current=0.0, voltage=0.0;
        for(uint s=0u;s<spp;s++) {
            uint i=(line*width+pixel*spp+s)*3u;
            vec3 drive=clamp(vec3(rgb[i],rgb[i+1u],rgb[i+2u]),0.0,1.0);
            voltage+=dot(drive,vec3(.299,.587,.114));
            vec3 emitted=max(drive-black_droop*bias,0.0)*max(1.0-strength*rail,0.1);
            current+=dot(pow(emitted,vec3(gamma)),vec3(1.0/3.0));
        }
        current/=float(spp);
        voltage/=float(spp);
        total+=current;
        rail=mix(rail,current,response);
        // Incomplete DC restoration leaves a fraction of the coupling
        // capacitor's charge in the gun bias. Retrace clamps the next line.
        bias=mix(bias,voltage,bias_response);
        load_map[line*256u+pixel]=rail;
        // The loaded video rail reduces gun drive voltage. The nonlinear
        // voltage-to-light response belongs to the subsequent beam stage.
        float gain=max(1.0-strength*rail,0.1);
        for(uint s=0u;s<spp;s++) {
            uint i=(line*width+pixel*spp+s)*3u;
            rgb[i]=max(0.0,rgb[i]-black_droop*bias)*gain;
            rgb[i+1u]=max(0.0,rgb[i+1u]-black_droop*bias)*gain;
            rgb[i+2u]=max(0.0,rgb[i+2u]-black_droop*bias)*gain;
        }
    }
    load_map[LINE_MEANS+line]=total/256.0;
}
