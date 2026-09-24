/* Complex equivalent IF, followed by the set's video detector.
 * Centred FIR has a common removed group delay. It reads a separate buffer:
 * in-place filtering would race neighbouring invocations.
 *
 * The carrier sits at zero frequency of this complex baseband, so a PLL
 * vision IF locked to it (a synchronous detector, every one-chip VIF since
 * the late 1970s) recovers the in-phase component v.x and rejects the
 * quadrature that the Nyquist slope makes of the vestigial sideband; a
 * diode envelope detector (older sets) takes length(v) and carries that
 * quadrature into the picture as a level error on chroma and edges. The
 * loop's own phase noise is invisible and not carried. */
#version 450
layout(local_size_x = 256) in;
layout(set = 0, binding = 0) readonly buffer CarrierIn { vec2 carrier[]; };
layout(set = 0, binding = 1) readonly buffer Taps { vec2 taps[]; };
layout(set = 1, binding = 0) writeonly buffer VideoOut { float video[]; };
layout(set = 2, binding = 0) uniform Params {
    uint count, samples_per_line, tap_count, detector;
};
shared vec2 tile[352]; // 256 outputs + the 97-tap filter halo
void main() {
    uint i=gl_GlobalInvocationID.x;
    int first=int(gl_WorkGroupID.x*256u)-48;
    for(uint k=gl_LocalInvocationID.x;k<352u;k+=256u)
        tile[k]=carrier[clamp(first+int(k),0,int(count)-1)];
    barrier();
    if(i>=count) return;
    int x=int(i%samples_per_line),base=int(i)-x;
    int middle=int(tap_count)/2;
    vec2 v=tile[int(i)-first]*taps[middle].x;
    // A real IF transfer has even real and odd imaginary impulse parts.
    // Pair symmetric taps: half the coefficient reads and multiplications.
    for(int d=1;d<=middle;d++) {
        vec2 a=tile[base+clamp(x+d,0,int(samples_per_line)-1)-first];
        vec2 b=tile[base+clamp(x-d,0,int(samples_per_line)-1)-first];
        vec2 h=taps[middle+d],sum=a+b,difference=a-b;
        v+=h.x*sum+h.y*vec2(-difference.y,difference.x);
    }
    const float gain=.875/(1.0+264.0/788.0),bias=.125+gain;
    float detected=detector==1u ? v.x : length(v);
    video[i]=(bias-detected)/gain;
}
