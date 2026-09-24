/* Keyed top-sync AGC: apply. One workgroup per line multiplies the line by
 * the gain agc_loop.comp wrote for it. All the measuring and the loop
 * live there; this pass only scales, so neighbouring lanes touch
 * neighbouring samples. In-place on the composite buffer. */
#version 450
layout(local_size_x = 256) in;
layout(set = 1, binding = 0) buffer DataBuf { float data[]; };
layout(set = 1, binding = 1) buffer Gains { vec4 gains[]; };
layout(set = 2, binding = 0) uniform Params {
    uint  total_count;
    uint  samples_per_line;
    uint  num_lines;
    float target_level;
    float attack_coeff;
    float release_coeff;
    float min_gain;
    float max_gain;
    float attack_slew_db;
    float noise_peak;
    float pad0, pad1;
};
void main() {
    uint line = gl_WorkGroupID.x;
    if (line >= num_lines) return;
    uint start = line * samples_per_line;
    uint end = min(start + samples_per_line, total_count);
    float gain = gains[line].x;
    if (gain <= 0.0) gain = 1.0;   /* a line the loop never reached passes as is */
    for (uint i = start + gl_LocalInvocationID.x; i < end; i += 256u)
        data[i] *= gain;
}
