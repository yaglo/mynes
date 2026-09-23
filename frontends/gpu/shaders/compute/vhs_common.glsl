/* Shared by vhs_tape and vhs_playback: parameters, hashing and the
 * two-level scans that run first-order IIR sections across a threadgroup.
 * Each thread owns K consecutive samples in subgroup order: the thread
 * gl_SubgroupID * gl_SubgroupSize + gl_SubgroupInvocationID owns tile
 * position K times that index. The scans are exact when the 224 threads
 * fill whole subgroups in that order, which Metal guarantees (7 SIMD
 * groups of 32) and Vulkan drivers give a one-dimensional workgroup with
 * full subgroups; a partial last subgroup would misplace its carries.
 * The including shader declares `coeff[]` (vec4) and the Params block
 * (GpuVHSParams) before this file; compile_shaders.sh reads the bindings
 * from the shader itself. */

const float TWO_PI = 6.28318530718;
const int SPL = 2728;          // 12 fsc samples per NES line
const int SPL3 = 682;
const int LINES = 262;
const int MASK_WORDS = 171;
const int SECTIONS = 16;       // first section vec4 after the filter headers
const int CLICK = SECTIONS + 48;

const int F_YREC = 0, F_PRE = 1, F_CREC = 2, F_MOD = 3, F_RF_REC = 4, F_RF_PB = 5,
          F_ENV = 6, F_YPB = 7, F_CPB = 8, F_DE = 9, F_CANC = 10, F_NOISE = 11;

shared vec2 scan_share[64];

uint thread_index() { return gl_SubgroupID * gl_SubgroupSize + gl_SubgroupInvocationID; }
bool last_lane() { return gl_SubgroupInvocationID == gl_SubgroupSize - 1u; }

vec2 cmul(vec2 a, vec2 b) { return vec2(a.x*b.x - a.y*b.y, a.x*b.y + a.y*b.x); }
vec2 cpow(vec2 p, float n) {
    float m = pow(length(p), n), a = atan(p.y, p.x) * n;
    return m * vec2(cos(a), sin(a));
}

uint pcg(uint v) {
    uint s = v * 747796405u + 2891336453u;
    uint w = ((s >> ((s >> 28u) + 4u)) ^ s) * 277803737u;
    return (w >> 22u) ^ w;
}
uint noise_key(uint stream, int n) {
    return pcg(frame * 0x9e3779b9u ^ stream) + uint(n) * 0x85ebca6bu;
}
float uniform01(uint key) { return (float(pcg(key) >> 8) + 0.5) * (1.0 / 16777216.0); }
vec2 gauss2(uint key) {
    uint a = pcg(key), b = pcg(a ^ 0x9e3779b9u);
    float u1 = (float(a >> 8) + 0.5) * (1.0 / 16777216.0);
    float u2 = float(b >> 8) * (1.0 / 16777216.0);
    float r = sqrt(-2.0 * log(u1));
    return r * vec2(cos(TWO_PI * u2), sin(TWO_PI * u2));
}

/* State entering this thread's block for y[n] = p y[n-1] + u[n], given the
 * block's own final state v from zero and the block decay pK = p^K. */
vec2 carry_complex(vec2 v, vec2 p, float k) {
    vec2 pk = cpow(p, k), m = pk, agg = v;
    for (uint d = 1u; d < gl_SubgroupSize; d <<= 1u) {
        vec2 o = subgroupShuffleUp(agg, d);
        if (gl_SubgroupInvocationID >= d) agg += cmul(m, o);
        m = cmul(m, m);
    }
    if (last_lane()) scan_share[gl_SubgroupID] = agg;
    barrier();
    vec2 carry = vec2(0);
    for (uint g = 0u; g < gl_SubgroupID; g++) carry = cmul(m, carry) + scan_share[g];
    vec2 prev = subgroupShuffleUp(agg, 1u);
    if (gl_SubgroupInvocationID > 0u)
        carry = prev + cmul(cpow(p, k * float(gl_SubgroupInvocationID)), carry);
    barrier();
    return carry;
}
float carry_real(float v, float p, float k) {
    float pk = pow(p, k), m = pk, agg = v;
    for (uint d = 1u; d < gl_SubgroupSize; d <<= 1u) {
        float o = subgroupShuffleUp(agg, d);
        if (gl_SubgroupInvocationID >= d) agg += m * o;
        m *= m;
    }
    if (last_lane()) scan_share[gl_SubgroupID].x = agg;
    barrier();
    float carry = 0.0;
    for (uint g = 0u; g < gl_SubgroupID; g++) carry = m * carry + scan_share[g].x;
    float prev = subgroupShuffleUp(agg, 1u);
    if (gl_SubgroupInvocationID > 0u)
        carry = prev + pow(p, k * float(gl_SubgroupInvocationID)) * carry;
    barrier();
    return carry;
}
/* Running sum modulo 1 (carrier phase in cycles): exclusive prefix. */
float carry_fract(float v) {
    float agg = v;
    for (uint d = 1u; d < gl_SubgroupSize; d <<= 1u) {
        float o = subgroupShuffleUp(agg, d);
        if (gl_SubgroupInvocationID >= d) agg = fract(agg + o);
    }
    if (last_lane()) scan_share[gl_SubgroupID].x = agg;
    barrier();
    float carry = 0.0;
    for (uint g = 0u; g < gl_SubgroupID; g++) carry = fract(carry + scan_share[g].x);
    float prev = subgroupShuffleUp(agg, 1u);
    if (gl_SubgroupInvocationID > 0u) carry = fract(prev + carry);
    barrier();
    return carry;
}
/* Largest value in the preceding threads, -1 when there is none. */
int carry_max(int v) {
    int agg = v;
    for (uint d = 1u; d < gl_SubgroupSize; d <<= 1u) {
        int o = subgroupShuffleUp(agg, d);
        if (gl_SubgroupInvocationID >= d) agg = max(agg, o);
    }
    if (last_lane()) scan_share[gl_SubgroupID].x = intBitsToFloat(agg);
    barrier();
    int carry = -1;
    for (uint g = 0u; g < gl_SubgroupID; g++) carry = max(carry, floatBitsToInt(scan_share[g].x));
    int prev = subgroupShuffleUp(agg, 1u);
    if (gl_SubgroupInvocationID > 0u) carry = max(prev, carry);
    barrier();
    return carry;
}
/* The previous thread's value, or own for the first thread of the tile. */
vec4 previous_thread(vec4 v) {
    vec4 prev = subgroupShuffleUp(v, 1u);
    if (last_lane()) scan_share[gl_SubgroupID] = v.xy;
    barrier();
    vec2 lo = gl_SubgroupID > 0u ? scan_share[gl_SubgroupID - 1u] : v.xy;
    barrier();
    if (last_lane()) scan_share[gl_SubgroupID] = v.zw;
    barrier();
    vec2 hi = gl_SubgroupID > 0u ? scan_share[gl_SubgroupID - 1u] : v.zw;
    barrier();
    if (gl_SubgroupInvocationID == 0u) prev = vec4(lo, hi);
    return prev;
}

/* Filter slot f on a real 16-sample block: direct term, real poles and
 * conjugate pairs (2 Re r y). */
void iir_real16(int f, inout float x[16]) {
    vec4 h = coeff[f];
    float o[16];
    for (int k = 0; k < 16; k++) o[k] = h.x * x[k];
    int first = int(h.z), count = int(h.w);
    for (int j = 0; j < count; j++) {
        vec4 s = coeff[SECTIONS + first + j];
        if (s.y == 0.0) {
            float p = s.x, st = 0.0, loc[16];
            for (int k = 0; k < 16; k++) { st = p * st + x[k]; loc[k] = st; }
            float c = carry_real(st, p, 16.0), w = p;
            for (int k = 0; k < 16; k++) { o[k] += s.z * (loc[k] + w * c); w *= p; }
        } else {
            vec2 p = s.xy, st = vec2(0), loc[16];
            for (int k = 0; k < 16; k++) { st = cmul(p, st) + vec2(x[k], 0.0); loc[k] = st; }
            vec2 c = carry_complex(st, p, 16.0), w = p;
            for (int k = 0; k < 16; k++) {
                vec2 y = loc[k] + cmul(w, c);
                o[k] += 2.0 * (s.z * y.x - s.w * y.y);
                w = cmul(w, p);
            }
        }
    }
    x = o;
}
/* Real filter slot f (every pole stored) on a complex 16-sample block. */
void iir_complex16(int f, inout vec2 x[16]) {
    vec4 h = coeff[f];
    vec2 o[16];
    for (int k = 0; k < 16; k++) o[k] = cmul(h.xy, x[k]);
    int first = int(h.z), count = int(h.w);
    for (int j = 0; j < count; j++) {
        vec4 s = coeff[SECTIONS + first + j];
        vec2 p = s.xy, st = vec2(0), loc[16];
        for (int k = 0; k < 16; k++) { st = cmul(p, st) + x[k]; loc[k] = st; }
        vec2 c = carry_complex(st, p, 16.0), w = p;
        for (int k = 0; k < 16; k++) { o[k] += cmul(s.zw, loc[k] + cmul(w, c)); w = cmul(w, p); }
    }
    x = o;
}
void iir_complex4(int f, inout vec2 x[4]) {
    vec4 h = coeff[f];
    vec2 o[4];
    for (int k = 0; k < 4; k++) o[k] = cmul(h.xy, x[k]);
    int first = int(h.z), count = int(h.w);
    for (int j = 0; j < count; j++) {
        vec4 s = coeff[SECTIONS + first + j];
        vec2 p = s.xy, st = vec2(0), loc[4];
        for (int k = 0; k < 4; k++) { st = cmul(p, st) + x[k]; loc[k] = st; }
        vec2 c = carry_complex(st, p, 4.0), w = p;
        for (int k = 0; k < 4; k++) { o[k] += cmul(s.zw, loc[k] + cmul(w, c)); w = cmul(w, p); }
    }
    x = o;
}

int wrap_sample(int n) { return n < 0 ? n + int(total) : (n >= int(total) ? n - int(total) : n); }
