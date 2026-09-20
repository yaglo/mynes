/* Route separately filtered source luma and chroma into the receiver.
 * The shared linear console/cable path obeys F(Y+C)-F(Y)=F(C).
 * This avoids another pair of full-rate cable filters for C. */
#version 450
layout(local_size_x=256) in;
layout(set=0,binding=0) readonly buffer Signal { float signal_in[]; };
layout(set=0,binding=1) readonly buffer SourceY { float y_in[]; };
layout(set=1,binding=0) writeonly buffer Luma { float y_out[]; };
layout(set=1,binding=1) writeonly buffer Chroma { float c_out[]; };
layout(set=2,binding=0) uniform Params { uint count; };
void main() {
    uint i=gl_GlobalInvocationID.x;
    if(i>=count) return;
    y_out[i]=y_in[i];
    c_out[i]=signal_in[i]-y_in[i];
}
