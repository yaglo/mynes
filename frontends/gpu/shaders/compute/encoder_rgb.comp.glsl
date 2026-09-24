/*
 * RGB console video encoder — GPU Compute Shader
 * ================================================
 *
 * Source stage for consoles whose video chip outputs RGB through a DAC and
 * whose composite comes from an encoder IC (Sony CXA1145/CXA1645 on the
 * Mega Drive, S-ENC/S-RGB on the Super Famicom). It replaces the 2C02 DAC
 * stage: instead of a per-code voltage waveform, each pixel is three gun
 * voltages read from a measured DAC ramp, and the encoder forms
 *
 *   Y  = 0.299 R + 0.587 G + 0.114 B
 *   U  = 0.492 (B - Y),  V = 0.877 (R - Y)        (NTSC B-Y / R-Y modulators)
 *   composite = setup + (1 - setup) * (Y + U cos(wt - 30°) + V cos(wt + 60°))
 *
 * The phase convention is the raster's 12-slot carrier: sample phase p
 * (0..11 per subcarrier cycle) gives wt = 2π p / 12, and the raster's burst
 * (raster_encode.comp.glsl, hue 8 square wave or the sine option) has its
 * fundamental at 210° in this grid, so B-Y sits at 30° and R-Y at -60°,
 * ninety degrees ahead in NTSC's rotation sense (hue index increases with
 * vector-scope angle on the 2C02, whose square wave for hue h peaks at
 * (3 - h) slots).
 *
 * Pixels do not have to be a whole number of samples wide: a Mega Drive
 * H40 pixel is 8 master clocks = 8/15 subcarrier cycle = 6.4 samples, so the
 * pixel under a sample is floor(s * spp_den / spp_num). This is the DAC's
 * sample-and-hold output seen on the encoder's sampling grid.
 *
 * The encoder's chroma band-pass (external LC on the CXA1145, built in on
 * the CXA1645) is applied as the equivalent low-pass on U and V before
 * modulation: a Hamming-windowed sinc of `taps` taps at `chroma_cut`
 * cycles/sample. Luma is not filtered here; the console output pole of the
 * signal chain models the encoder's Y bandwidth.
 *
 * Input:  width × lines packed codes (bits 0-5 red, 6-11 green, 12-17 blue)
 *         + 64-entry ramp of gun voltages (0 = black, 1 = white); or, with
 *         code_bits 10, linear 10-bit gun voltages (bits 0-9 red, 10-19
 *         green, 20-29 blue) from a picture or video, the ramp unused
 * Output: samples_per_line × 240 composite floats (blank 0, white 1)
 *         + the same in luma only (Y/C sources), or interleaved RGB gun
 *         voltages over the decode window (decode_window.h) when
 *         source_mode == 2 (an RGB SCART connection): the picture at
 *         picture_x, picture_row + top_line, black around it.
 *
 * Dispatch: one thread per output sample, 256 per workgroup.
 */

#version 450

layout(local_size_x = 256) in;

layout(set = 0, binding = 0) readonly buffer PixelBuf { uint pixels[]; };
layout(set = 0, binding = 1) readonly buffer RampBuf { float ramp[]; };

layout(set = 1, binding = 0) writeonly buffer WaveformBuf { float waveform[]; };
layout(set = 1, binding = 1) writeonly buffer LumaBuf { float source_y[]; };

layout(set = 2, binding = 0) uniform Params {
    uint  width;             /* console pixels per line (256, 320, 512) */
    uint  lines;             /* console picture lines (224, 239, 240) */
    uint  samples_per_line;  /* active samples per raster line (2048) */
    uint  spp_num;           /* samples per pixel = spp_num / spp_den */
    uint  spp_den;
    uint  top_line;          /* raster picture line of the first console line */
    uint  source_mode;       /* 0 composite, 1 Y/C, 2 RGB */
    uint  taps;              /* chroma low-pass taps (odd); 0 = unfiltered */
    float phase_base;        /* carrier phase at the first active sample, slots */
    float phase_line_adv;    /* carrier phase advance per raster line, slots */
    float chroma_cut;        /* chroma low-pass cutoff, cycles per sample */
    float setup;             /* black pedestal as a fraction of white (0 or 0.075) */
    float luma_cut;          /* luma low-pass cutoff, cycles per sample; 0 = none */
    float trap_cut;          /* luma trap half-width, cycles per sample */
    float trap_depth;        /* luma trap depth at the subcarrier, 0 = no trap */
    uint  code_bits;         /* 0: codes index the ramp; 10: linear 10-bit guns */
    vec4  rgb_row_r, rgb_row_g, rgb_row_b; /* monitor gains and bias for RGB input */
    uint  window_width, window_lines;      /* RGB: the decode window */
    int   picture_x, picture_row;          /* and the picture's place in it */
};

float sinc_w(float cut, float x) {
    return x == 0.0 ? 2.0 * cut : sin(6.28318530718 * cut * x) / (3.14159265 * x);
}

#define TWO_PI 6.28318530718

vec3 gun_at(uint pic_line, int px) {
    px = clamp(px, 0, int(width) - 1);
    uint code = pixels[pic_line * width + uint(px)];
    if (code_bits == 10u)
        return vec3(float(code & 1023u), float((code >> 10u) & 1023u), float((code >> 20u) & 1023u)) / 1023.0;
    return vec3(ramp[code & 63u], ramp[(code >> 6u) & 63u], ramp[(code >> 12u) & 63u]);
}

vec3 yuv_of(vec3 gun) {
    float y = dot(gun, vec3(0.299, 0.587, 0.114));
    return vec3(y, 0.492111 * (gun.b - y), 0.877283 * (gun.r - y));
}

void main() {
    uint i = gl_GlobalInvocationID.x;
    if (source_mode == 2u) {
        if (i >= window_width * window_lines) return;
        int row = int(i / window_width) - picture_row, column = int(i % window_width) - picture_x;
        bool lit = row >= int(top_line) && row < int(top_line + lines) && column >= 0 && column < int(samples_per_line);
        vec3 gun = lit ? gun_at(uint(row) - top_line, int(uint(column) * spp_den / spp_num)) : vec3(0.0);
        vec3 rgb = vec3(rgb_row_r.x * gun.r + rgb_row_r.w,
                        rgb_row_g.x * gun.g + rgb_row_g.w,
                        rgb_row_b.x * gun.b + rgb_row_b.w);
        waveform[i * 3u] = rgb.r;
        waveform[i * 3u + 1u] = rgb.g;
        waveform[i * 3u + 2u] = rgb.b;
        return;
    }

    uint line = i / samples_per_line;
    uint s = i % samples_per_line;
    if (line >= 240u) return;

    bool in_picture = line >= top_line && line < top_line + lines;
    uint pic_line = in_picture ? line - top_line : 0u;

    if (!in_picture) {
        waveform[i] = setup;
        source_y[i] = setup;
        return;
    }

    /* The encoder's filters as baseband windowed sincs on the sample grid,
     * one Hamming window of `taps` over all of them: the luma low-pass (the
     * Y path's bandwidth, 5 MHz at -3 dB on a CXA1645), an optional trap at
     * the subcarrier (a band-pass of trap_cut half-width, formed by
     * modulating a low-pass onto the carrier, subtracted from Y at
     * trap_depth; the YTRAP pin's purpose), and the chroma low-pass on U
     * and V. taps is capped at 63 by the caller, so a trap narrower than
     * about 1 MHz is widened. */
    vec3 yuv = yuv_of(gun_at(pic_line, int(s * spp_den / spp_num)));
    vec2 uv = yuv.yz;
    if (taps > 1u) {
        int half_n = int(taps) / 2;
        vec2 c_acc = vec2(0.0);
        float c_norm = 0.0, y_acc = 0.0, y_norm = 0.0, t_acc = 0.0, t_norm = 0.0;
        for (int k = -half_n; k <= half_n; k++) {
            float x = float(k);
            float win = 0.54 + 0.46 * cos(3.14159265 * x / float(half_n + 1));
            int ss = int(s) + k;
            vec3 n = yuv_of(gun_at(pic_line, int(ss < 0 ? 0 : ss) * int(spp_den) / int(spp_num)));
            if (chroma_cut > 0.0) { float w = sinc_w(chroma_cut, x) * win; c_acc += w * n.yz; c_norm += w; }
            if (luma_cut > 0.0) {
                /* The Y path is an analogue lumped delay line and amplifier,
                 * a maximally-flat-delay (Bessel-like) roll-off rather than a
                 * brick wall: a Gaussian whose -3 dB point is luma_cut,
                 * sigma = 0.8326 / (2 pi f) in samples. */
                float sigma = 0.8326 / (6.28318530718 * luma_cut);
                float w = exp(-x * x / (2.0 * sigma * sigma)) * win;
                y_acc += w * n.x; y_norm += w;
            }
            if (trap_depth > 0.0) {
                float w = sinc_w(trap_cut, x) * win;
                t_norm += w;
                t_acc += w * 2.0 * cos(TWO_PI * x / 12.0) * n.x;
            }
        }
        if (chroma_cut > 0.0) uv = c_acc / c_norm;
        if (luma_cut > 0.0) yuv.x = y_acc / y_norm;
        if (trap_depth > 0.0) yuv.x -= trap_depth * t_acc / t_norm;
    }

    /* Axes measured against the receiver with full-code colour bars
     * (mynes_retro --calibrate, reference composite preset, the 2C02
     * output-impedance model off): B-Y decodes on the raster's 138-degree
     * slot phase and R-Y ninety degrees behind it, to within a degree. */
    float p = phase_base + float(line) * phase_line_adv + float(s);
    float a = TWO_PI * p / 12.0;
    float chroma = uv.x * cos(a - radians(138.0)) + uv.y * cos(a - radians(48.0));
    float y = setup + (1.0 - setup) * yuv.x;
    waveform[i] = y + (1.0 - setup) * chroma;
    source_y[i] = y;
}
