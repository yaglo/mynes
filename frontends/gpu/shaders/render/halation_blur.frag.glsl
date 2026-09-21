/* Unit-energy glass scatter. Fixed quadrature weights, variable UV spacing:
 * the optical width follows the tube face, not the host drawable resolution. */
#version 450
layout(location=0) in vec2 uv;
layout(location=0) out vec4 frag_color;
layout(set=2,binding=0) uniform sampler2D tex_input;
layout(set=3,binding=0) uniform BlurParams {
    vec2 direction;
    float input_gamma;
    float extended_kernel;
};
const float weights[17]=float[17](0.0629553835,0.0621915567,0.0599552321,0.0564052927,0.0517856859,0.0463977282,0.0405677393,0.0346148142,0.0288230749,0.0234215549,0.0185732645,0.0143733457,0.0108548695,0.0079999716,0.0057537289,0.0040383825,0.0027660665);
// Four-sigma support for explicitly calibrated widths, normalized to unit energy.
const float extended_weights[17]=float[17](0.0997390995,0.0966704500,0.0880194464,0.0752870222,0.0604948218,0.0456638872,0.0323805448,0.0215700930,0.0134982193,0.0079351938,0.0043822302,0.0022734711,0.0011080013,0.0005072800,0.0002181784,0.0000881520,0.0000334587);
float weight(int i) { return extended_kernel>0.0 ? extended_weights[i] : weights[i]; }
vec3 light(vec2 p) {
    vec3 v=max(texture(tex_input,p).rgb,vec3(0));
    return input_gamma>0.0 ? pow(v,vec3(input_gamma)) : v;
}
void main() {
    vec3 accum=light(uv)*weight(0);
    for(int i=1;i<=16;i++) {
        vec2 offset=direction*float(i);
        accum+=(light(uv+offset)+light(uv-offset))*weight(i);
    }
    frag_color=vec4(accum,1);
}
