/* Integrate beam light over each reduced pixel before glass scattering.
 * Sampling one point aliases the raster into the halo: a four-pixel-high
 * scanline can be missed completely or counted twice as the window moves. */
#version 450
layout(location=0) in vec2 uv;
layout(location=0) out vec4 frag_color;
layout(set=2,binding=0) uniform sampler2D tex_input;
layout(set=3,binding=0) uniform ReduceParams {
    vec2 output_size;
    float input_gamma;
};
void main() {
    vec2 source_size=vec2(textureSize(tex_input,0));
    vec2 footprint=source_size/output_size;
    // Snap to the output texel to avoid accumulating interpolator error.
    vec2 lo=floor(uv*output_size)*footprint;
    vec2 hi=lo+footprint;
    // Integer reductions can integrate adjacent 2x2 blocks with one linear
    // sample. This is exact for linear input and avoids 16 fetches at 4:1.
    if(input_gamma<=0.0 && all(equal(footprint,floor(footprint)))) {
        vec3 light=vec3(0.0);
        for(int y=0;y<int(footprint.y);y+=2) for(int x=0;x<int(footprint.x);x+=2) {
            vec2 size=min(vec2(2.0),footprint-vec2(x,y));
            vec2 center=(lo+vec2(x,y)+0.5*size)/source_size;
            light+=max(texture(tex_input,center).rgb,vec3(0.0))*(size.x*size.y);
        }
        frag_color=vec4(light/(footprint.x*footprint.y),1.0);
        return;
    }
    ivec2 first=max(ivec2(floor(lo)),ivec2(0));
    ivec2 end=min(ivec2(ceil(hi)),ivec2(source_size));
    vec3 light=vec3(0.0);
    for(int y=first.y;y<end.y;y++) for(int x=first.x;x<end.x;x++) {
        vec2 overlap=max(min(hi,vec2(x+1,y+1))-max(lo,vec2(x,y)),vec2(0.0));
        vec3 v=max(texelFetch(tex_input,ivec2(x,y),0).rgb,vec3(0.0));
        if(input_gamma>0.0) v=pow(v,vec3(input_gamma));
        light+=v*(overlap.x*overlap.y);
    }
    frag_color=vec4(light/(footprint.x*footprint.y),1.0);
}
