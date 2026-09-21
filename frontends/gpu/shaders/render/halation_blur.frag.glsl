/* Unit-energy glass scatter. Fixed quadrature weights, variable UV spacing:
 * the optical width follows the tube face, not the host drawable resolution. */
#version 450
layout(location=0) in vec2 uv;
layout(location=0) out vec4 frag_color;
layout(set=2,binding=0) uniform sampler2D tex_input;
layout(set=3,binding=0) uniform BlurParams {
    vec2 direction;
    float input_gamma;
};
const float weights[17]=float[17](0.0629553835,0.0621915567,0.0599552321,0.0564052927,0.0517856859,0.0463977282,0.0405677393,0.0346148142,0.0288230749,0.0234215549,0.0185732645,0.0143733457,0.0108548695,0.0079999716,0.0057537289,0.0040383825,0.0027660665);
vec3 light(vec2 p) {
    vec3 v=max(texture(tex_input,p).rgb,vec3(0));
    return input_gamma>0.0 ? pow(v,vec3(input_gamma)) : v;
}
void main() {
    vec3 accum=light(uv)*weights[0];
    for(int i=1;i<=16;i++) {
        vec2 offset=direction*float(i);
        accum+=(light(uv+offset)+light(uv-offset))*weights[i];
    }
    frag_color=vec4(accum,1);
}
