/* Horizontal spreading of linear gun current. Each source deposits its
 * own normalized spot, which broadens with its current. Evaluating width
 * at the destination instead would create/destroy energy at bright edges. */
#version 450
layout(local_size_x = 256) in;
layout(set=0,binding=0) buffer RGBIn { float rgb_in[]; };
layout(set=0,binding=1) buffer RGBOut { float rgb_out[]; };
layout(set=1,binding=0) uniform Params {
    uint signal_w, num_lines, radius;
    float growth;
    vec4 weights[9], wide_weights[9];
};
vec3 read_rgb(uint base,int x) {
    uint i=(base+uint(clamp(x,0,int(signal_w)-1)))*3u;
    return vec3(rgb_in[i],rgb_in[i+1u],rgb_in[i+2u]);
}
vec3 deposit(vec3 current, uint dx) {
    float narrow=weights[dx/4u][dx%4u];
    if(growth<=0.0) return current*narrow;
    float wide=wide_weights[dx/4u][dx%4u];
    return current*mix(vec3(narrow),vec3(wide),clamp(current,0.0,1.0));
}
void main() {
    uint sx=gl_GlobalInvocationID.x, sy=gl_GlobalInvocationID.y;
    if(sx>=signal_w || sy>=num_lines) return;
    uint base=sy*signal_w;
    vec3 current=deposit(read_rgb(base,int(sx)),0u);
    for(uint dx=1u;dx<=radius;dx++) {
        current+=deposit(read_rgb(base,int(sx)-int(dx)),dx)
                +deposit(read_rgb(base,int(sx)+int(dx)),dx);
    }
    uint out_idx=(base+sx)*3u;
    rgb_out[out_idx]=current.r;
    rgb_out[out_idx+1u]=current.g;
    rgb_out[out_idx+2u]=current.b;
}
