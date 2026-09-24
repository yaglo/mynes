/* GPU regressions for the RGB console encoder source and, through it, the
 * receiver against a standard-level NTSC signal.
 *
 * The 2C02's burst is nonstandard, so the NES path can never show whether
 * the receiver decodes a standard source to identity. The encoder stage
 * carries a 40 IRE sine burst, a -40 IRE sync and Rec. 601 luma, so
 * full-code colour bars through it must come back as the sent RGB before the
 * tube, on the composite, S-Video and RGB connections and on both console
 * line lengths. The second test measures cross-colour on one-pixel white
 * strokes and holds the encoder's subcarrier trap to what it removes. */
#include <SDL3/SDL.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "video_gpu.h"
#include "signal_precompute.h"

static int failures;
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "FAIL %d: %s\n", __LINE__, #x); failures++; } } while (0)

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* Mega Drive VDP colour DAC ramp, 15 levels (plutiedev, VDP colour ramp). */
static const float md_ramp[15] = {0, 29, 52, 70, 87, 101, 116, 130, 144, 158, 172, 187, 206, 228, 255};

typedef struct {
    const char *name;
    int dots_per_line, line_phase, width;
    float ramp[64];
    int ramp_n;
} Console;

static Console console_md(void) {
    Console c = {"Mega Drive H40", 342, 0, 320, {0}, 15};
    for (int i = 0; i < 15; i++) c.ramp[i] = md_ramp[i] / 255.0f;
    return c;
}
static Console console_snes(void) {
    Console c = {"Super Famicom", 341, 4, 256, {0}, 32};
    for (int i = 0; i < 32; i++) c.ramp[i] = (float)i / 31.0f;
    return c;
}

static int gcd(int a, int b) { while (b) { int t = a % b; a = b; b = t; } return a; }

/* The encoder's pedestal (setup) for decode(); 0 unless a test sets it. */
static float decode_setup;

/* Run `frames` pictures through a fresh chain and return the decoded RGB
 * of the last one (rgb_size floats, caller frees), or NULL. *window_out
 * says where the picture's samples are; *spl_out is its width in samples. */
static float *decode(SDL_GPUDevice *gpu, const Console *con, VideoConnectionType conn,
                     const uint32_t *codes, int lines, float luma_bw_hz, float luma_trap,
                     int frames, int *spl_out, DecodeWindow *window_out, int code_bits) {
    SignalPrecompute sp;
    signal_precompute_init(&sp, SIGNAL_REGION_NTSC);
    VideoChain chain;
    video_chain_init_preset(&chain, conn, VIDEO_COMB_NONE, SIGNAL_REGION_NTSC);
    chain.signal_fmt.dots_per_line = con->dots_per_line;
    chain.console_phase_distortion_ns = 0; /* a 2C02 pin estimate, not an encoder IC */
    VideoGPUChain v;
    if (!video_gpu_init(&v, gpu, &chain, "shaders/compute", sp.fir_y, sp.fir_y_n,
                        sp.fir_c, sp.fir_c_n, sp.fir_q, sp.fir_q_n)) return NULL;
    video_gpu_set_color_matrix(&v, sp.color_matrix, sp.color_bias);
    video_gpu_set_demod(&v, 0.0f, 2.0f * (float)M_PI / 12.0f);
    int g = gcd(SIGNAL_NTSC_SAMPLES_PER_LINE, con->width);
    VideoRGBSource src = {
        .pixels = codes, .width = con->width, .lines = lines, .top_line = 8,
        .spp_num = SIGNAL_NTSC_SAMPLES_PER_LINE / g, .spp_den = con->width / g,
        .ramp = con->ramp, .ramp_n = con->ramp_n,
        .phase_base = 0, .phase_line_adv = con->line_phase,
        .chroma_bw_hz = 1.3e6f, .luma_bw_hz = luma_bw_hz, .luma_trap = luma_trap, .setup = decode_setup,
        .code_bits = code_bits,
    };
    float *rgb = calloc(v.rgb_size / sizeof(float), sizeof(float));
    bool ok = rgb != NULL;
    for (int f = 0; ok && f < frames; f++) {
        v.elapsed_frames = 1;
        v.signal_frame_counter = (uint32_t)f;
        v.beam_frame_counter = (uint32_t)f;
        video_gpu_set_demod(&v, (float)src.phase_base * 2.0f * (float)M_PI / 12.0f, v.demod_dp);
        ok = video_gpu_process_rgb(&v, gpu, &src) && (f + 1 < frames || video_gpu_download_window_rgb(&v, gpu, rgb));
        if (con->line_phase) src.phase_base = (src.phase_base + ((f & 1) ? 8 : 4)) % 12;
    }
    *spl_out = chain.signal_fmt.samples_per_line;
    *window_out = v.window;
    video_gpu_destroy(&v, gpu);
    if (!ok) { free(rgb); return NULL; }
    return rgb;
}

/* Eight full-code bars: white, yellow, cyan, green, magenta, red, blue, black. */
static const int bar_rgb[8][3] = {{1,1,1},{1,1,0},{0,1,1},{0,1,0},{1,0,1},{1,0,0},{0,0,1},{0,0,0}};

static void encoder_identity(SDL_GPUDevice *gpu, const Console *con, VideoConnectionType conn,
                             const char *conn_name) {
    const int lines = 224;
    uint32_t *codes = malloc((size_t)con->width * lines * sizeof(uint32_t));
    uint32_t full = (uint32_t)(con->ramp_n - 1);
    for (int y = 0; y < lines; y++)
        for (int x = 0; x < con->width; x++) {
            const int *c = bar_rgb[x * 8 / con->width];
            codes[y * con->width + x] = (c[0] ? full : 0) | ((c[1] ? full : 0) << 6) | ((c[2] ? full : 0) << 12);
        }
    int spl = 0;
    DecodeWindow w;
    float *rgb = decode(gpu, con, conn, codes, lines, 5e6f, 0.0f, 12, &spl, &w, 0);
    CHECK(rgb != NULL);
    float worst = 0;
    if (rgb) {
        for (int b = 0; b < 8; b++) {
            double acc[3] = {0}; int n = 0;
            for (int y = 8 + 40; y < 8 + lines - 40; y++)
                for (int x = b * spl / 8 + spl / 32; x < (b + 1) * spl / 8 - spl / 32; x++) {
                    const float *px = rgb + decode_window_rgb_index(&w, y, x);
                    acc[0] += px[0]; acc[1] += px[1]; acc[2] += px[2]; n++;
                }
            for (int c = 0; c < 3; c++) {
                float err = fabsf((float)(acc[c] / n) - (float)bar_rgb[b][c]);
                if (err > worst) worst = err;
            }
        }
        printf("encoder identity, %s, %s: worst bar error %.4f\n", con->name, conn_name, worst);
        CHECK(worst < 0.01f);
    }
    free(rgb);
    free(codes);
}

/* Cross-colour: mean decoded chroma magnitude (R-Y, B-Y) over white
 * one-pixel strokes every fourth pixel (left half) and every fourth line
 * (right half). Horizontal strokes carry no subcarrier-band energy; the
 * vertical ones do, and the trap must remove most of it. */
static double stroke_chroma(SDL_GPUDevice *gpu, const Console *con, float luma_bw_hz, float trap,
                            int half, double *luma_out) {
    const int lines = 224;
    uint32_t *codes = malloc((size_t)con->width * lines * sizeof(uint32_t));
    uint32_t full = (uint32_t)(con->ramp_n - 1), white = full | (full << 6) | (full << 12);
    for (int y = 0; y < lines; y++)
        for (int x = 0; x < con->width; x++)
            codes[y * con->width + x] = x < con->width / 2 ? ((x & 3) == 0 ? white : 0)
                                                           : ((y & 3) == 0 ? white : 0);
    int spl = 0;
    DecodeWindow w;
    float *rgb = decode(gpu, con, VIDEO_CONN_COMPOSITE, codes, lines, luma_bw_hz, trap, 12, &spl, &w, 0);
    free(codes);
    if (!rgb) { failures++; return -1; }
    double chroma = 0, luma = 0; int n = 0;
    for (int y = 8 + 20; y < 8 + lines - 20; y++)
        for (int x = half * spl / 2 + spl / 16; x < (half + 1) * spl / 2 - spl / 16; x++) {
            const float *px = rgb + decode_window_rgb_index(&w, y, x);
            double yy = 0.299 * px[0] + 0.587 * px[1] + 0.114 * px[2];
            chroma += sqrt((px[0] - yy) * (px[0] - yy) + (px[2] - yy) * (px[2] - yy));
            luma += yy; n++;
        }
    free(rgb);
    if (luma_out) *luma_out = luma / n;
    return chroma / n;
}

/* A picture or video enters as linear 10-bit gun voltages instead of a
 * console's DAC codes: the same bars at code 1023, and a grey at code 512
 * in the lower half, decode to the sent voltages. */
static void encoder_linear(SDL_GPUDevice *gpu, VideoConnectionType conn, const char *conn_name) {
    Console con = console_snes();
    con.width = 1024;
    const int lines = 224;
    uint32_t *codes = malloc((size_t)con.width * lines * sizeof(uint32_t));
    for (int y = 0; y < lines; y++)
        for (int x = 0; x < con.width; x++) {
            const int *c = bar_rgb[x * 8 / con.width];
            uint32_t v = y < lines / 2 ? 1023u : 512u;
            codes[y * con.width + x] = y < lines / 2
                ? (c[0] ? v : 0) | ((c[1] ? v : 0) << 10) | ((c[2] ? v : 0) << 20)
                : v | (v << 10) | (v << 20);
        }
    int spl = 0;
    DecodeWindow w;
    float *rgb = decode(gpu, &con, conn, codes, lines, 5e6f, 0.0f, 12, &spl, &w, 10);
    CHECK(rgb != NULL);
    if (rgb) {
        float worst = 0;
        for (int b = 0; b < 8; b++) {
            double acc[3] = {0}; int n = 0;
            for (int y = 8 + 30; y < 8 + lines / 2 - 20; y++)
                for (int x = b * spl / 8 + spl / 32; x < (b + 1) * spl / 8 - spl / 32; x++) {
                    const float *px = rgb + decode_window_rgb_index(&w, y, x);
                    acc[0] += px[0]; acc[1] += px[1]; acc[2] += px[2]; n++;
                }
            for (int c = 0; c < 3; c++) worst = fmaxf(worst, fabsf((float)(acc[c] / n) - (float)bar_rgb[b][c]));
        }
        double grey[3] = {0}; int n = 0;
        for (int y = 8 + lines / 2 + 20; y < 8 + lines - 20; y++)
            for (int x = spl / 8; x < spl * 7 / 8; x++) {
                const float *px = rgb + decode_window_rgb_index(&w, y, x);
                grey[0] += px[0]; grey[1] += px[1]; grey[2] += px[2]; n++;
            }
        float grey_err = 0;
        for (int c = 0; c < 3; c++) grey_err = fmaxf(grey_err, fabsf((float)(grey[c] / n) - 512.0f / 1023.0f));
        printf("encoder, 10-bit linear input, %s: worst bar error %.4f, grey error %.4f\n", conn_name, worst, grey_err);
        CHECK(worst < 0.01f);
        CHECK(grey_err < 0.005f);
    }
    free(rgb);
    free(codes);
}

/* With a 7.5 IRE setup the encoder's pedestal spans the whole active line:
 * the console's side border decodes to the black of a black bar, not 7.5
 * IRE below it. */
static void encoder_pedestal(SDL_GPUDevice *gpu) {
    Console con = console_snes();
    const int lines = 224;
    uint32_t *codes = malloc((size_t)con.width * lines * sizeof(uint32_t));
    uint32_t full = (uint32_t)(con.ramp_n - 1);
    for (int y = 0; y < lines; y++)
        for (int x = 0; x < con.width; x++) {
            const int *c = bar_rgb[x * 8 / con.width];
            codes[y * con.width + x] = (c[0] ? full : 0) | ((c[1] ? full : 0) << 6) | ((c[2] ? full : 0) << 12);
        }
    int spl = 0;
    DecodeWindow w;
    decode_setup = 0.075f;
    float *rgb = decode(gpu, &con, VIDEO_CONN_COMPOSITE, codes, lines, 5e6f, 0.0f, 12, &spl, &w, 0);
    decode_setup = 0;
    CHECK(rgb != NULL);
    if (rgb) {
        double bar[3] = {0}, border[3] = {0};
        int nb = 0, nd = 0;
        for (int y = 60; y <= 180; y++) {
            for (int x = 7 * spl / 8 + spl / 32; x < spl - spl / 32; x++, nb++)
                for (int c = 0; c < 3; c++) bar[c] += rgb[decode_window_rgb_index(&w, y, x) + c];
            /* Raster dots 55 to 60, clear of the picture and the dot-49 pulse. */
            for (int x = (55 - 65) * w.spp; x < (61 - 65) * w.spp; x++, nd++)
                for (int c = 0; c < 3; c++) border[c] += rgb[decode_window_rgb_index(&w, y, x) + c];
        }
        float worst = 0;
        for (int c = 0; c < 3; c++) worst = fmaxf(worst, fabsf((float)(bar[c] / nb - border[c] / nd)));
        printf("encoder, 7.5 IRE setup: border against the black bar %.4f\n", worst);
        CHECK(worst < 0.01f);
    }
    free(rgb);
    free(codes);
}

int test_encoder(SDL_GPUDevice *gpu) {
    encoder_pedestal(gpu);
    Console md = console_md(), snes = console_snes();
    const struct { VideoConnectionType conn; const char *name; } conns[] = {
        {VIDEO_CONN_COMPOSITE, "composite"}, {VIDEO_CONN_SVIDEO, "S-Video"}, {VIDEO_CONN_RGB, "RGB"}};
    for (unsigned i = 0; i < sizeof(conns) / sizeof(conns[0]); i++) {
        encoder_identity(gpu, &md, conns[i].conn, conns[i].name);
        encoder_identity(gpu, &snes, conns[i].conn, conns[i].name);
        encoder_linear(gpu, conns[i].conn, conns[i].name);
    }
    double luma_v = 0, luma_h = 0;
    double open_v = stroke_chroma(gpu, &snes, 5e6f, 0.0f, 0, &luma_v);
    double open_h = stroke_chroma(gpu, &snes, 5e6f, 0.0f, 1, &luma_h);
    double trap_v = stroke_chroma(gpu, &snes, 5e6f, 1.0f, 0, NULL);
    printf("cross-colour, Super Famicom composite: vertical strokes %.4f (luma %.3f), "
           "horizontal %.4f (luma %.3f), vertical with trap %.4f\n",
           open_v, luma_v, open_h, luma_h, trap_v);
    CHECK(open_v > 0.2);                 /* one-pixel strokes do rainbow on composite */
    CHECK(open_h < 0.01);                /* nothing along the line direction */
    CHECK(trap_v < open_v * 0.5);        /* the trap removes at least half of it (0.36 on the default chain, 0.24 on the reference preset, 0.03 with an adaptive comb) */
    CHECK(fabs(luma_v - 0.25) < 0.02 && fabs(luma_h - 0.25) < 0.02); /* a quarter of white on average */
    return failures;
}
