/* The console's border on the tube: the decode window's arithmetic, the
 * backdrop decoded through the same chain as the picture, the receiver's
 * flyback blanking around it, the picture's place on the face, and the
 * border of an RGB PPU and of an RGB encoder source. */
#include "video_gpu.h"
#include "signal_precompute.h"
#include "gpu_half.h"
#include "preset_json.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern bool dispatch_beam_profile_public(VideoGPUChain *, SDL_GPUCommandBuffer *);

static int failures;
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "Border FAIL %d: %s\n", __LINE__, #x); failures++; } } while (0)
#define TAU 6.28318530718f

/* NTSC and PAL windows against the values worked out from BT.470. */
static void window_arithmetic(void) {
    const struct {
        int region, spp, start_dot, dots, width, first_line, lines, picture_x, picture_row, row0, row1, raster_line0;
        float x0, x1; size_t samples;
    } expect[] = {
        {SIGNAL_REGION_NTSC, 8, 46, 292, 2336, 0, 242, 152, 0, 1, 242, 0, 35.77f, 2297.57f, 565312},
        {SIGNAL_REGION_PAL, 10, 51, 286, 2860, -20, 288, 140, 20, 0, 288, 292, 48.64f, 2815.21f, 823680},
    };
    for (int k = 0; k < 2; k++) {
        DecodeWindow w;
        decode_window_raster(&w, expect[k].region, expect[k].spp, 341, 65);
        CHECK(w.start_dot == expect[k].start_dot && w.dots == expect[k].dots && w.width == expect[k].width);
        CHECK(w.first_line == expect[k].first_line && w.lines == expect[k].lines);
        CHECK(w.picture_x == expect[k].picture_x && w.picture_row == expect[k].picture_row);
        CHECK(w.trace_row0 == expect[k].row0 && w.trace_row1 == expect[k].row1);
        CHECK(fabsf(w.trace_x0 - expect[k].x0) < .01f && fabsf(w.trace_x1 - expect[k].x1) < .01f);
        CHECK(decode_window_samples(&w) == expect[k].samples);
        CHECK(decode_window_raster_line(&w, 0) == expect[k].raster_line0);
        /* The picture lies inside the window, from the same raster samples. */
        CHECK(w.picture_x >= 0 && w.picture_x + w.picture_w <= w.width);
        CHECK(w.picture_row >= 0 && w.picture_row + w.picture_h <= w.lines);
        CHECK(w.start_dot * w.spp + w.picture_x == 65 * w.spp);
        CHECK(decode_window_rgb_index(&w, 0, 0) == (size_t)(w.picture_row * w.width + w.picture_x) * 3);
        /* The widest spot halo reads 32 samples past an output: both margins
         * leave it inside the window, where blanking makes them zero. */
        CHECK(w.trace_x0 >= DECODE_WINDOW_HALO_SAMPLES && w.width - w.trace_x1 >= DECODE_WINDOW_HALO_SAMPLES);
        CHECK(fabsf((w.trace_x1 - w.trace_x0) - w.active_dots * w.spp) < 1e-3f);
        CHECK(w.start_dot + w.dots <= w.dots_per_line);
        printf("Decode window %s: dots %d..%d, lines %d..%d, trace %.2f..%.2f, %zu samples\n",
               k ? "PAL" : "NTSC", w.start_dot, w.start_dot + w.dots, w.first_line, w.first_line + w.lines,
               w.trace_x0, w.trace_x1, decode_window_samples(&w));
    }
}

static bool chain(SDL_GPUDevice *gpu, VideoGPUChain *v, VideoChain *c, SignalPrecompute *sp,
                  int region, VideoConnectionType conn, VideoCombType comb) {
    signal_precompute_init(sp, region);
    video_chain_init_preset(c, conn, comb, region);
    memset(&c->tv, 0, sizeof(c->tv));
    c->tv.gamma = 1; c->tv.h_size = c->tv.v_size = 1;
    c->tv.r_bandwidth = c->tv.g_bandwidth = c->tv.b_bandwidth = 10e6f;
    c->tv.chroma_bandwidth = 1.3e6f;   /* the comb's chroma band */
    c->console_psu_hum = 0; c->cable.shield_effectiveness = 1;
    if (!video_gpu_init(v, gpu, c, "shaders/compute", sp->fir_y, sp->fir_y_n, sp->fir_c, sp->fir_c_n, sp->fir_q, sp->fir_q_n))
        return false;
    if (!video_gpu_upload_signal_table(v, gpu, (float *)sp->table,
            region == SIGNAL_REGION_PAL ? (float *)sp->table_alt : NULL, SIG_TABLE_ENTRIES, SIG_TABLE_STRIDE))
        return false;
    video_gpu_set_color_matrix(v, sp->color_matrix, sp->color_bias);
    return true;
}

static void set_backdrop(VideoGPUChain *v, const SignalPrecompute *sp, unsigned entry) {
    memcpy(v->backdrop, sp->table[entry], 12 * sizeof(float));
    memcpy(v->gray_backdrop, sp->table[entry & 0x1f0], 12 * sizeof(float));
    v->backdrop_entry = entry;
}

/* Frames first..first+count-1 of one picture; the last one's RGB into rgb. */
static bool frames(SDL_GPUDevice *gpu, VideoGPUChain *v, const SignalPrecompute *sp, const uint16_t *codes,
                   unsigned first, unsigned count, float *rgb) {
    bool ok = true;
    for (unsigned f = first; ok && f < first + count; f++) {
        int phase = signal_frame_phase(sp, f);
        video_gpu_set_demod(v, (phase + sp->demod_rotate) * TAU / 12, TAU / 12);
        v->elapsed_frames = 1;
        ok = video_gpu_process_full(v, gpu, codes, phase, sp->phase_line_adv, 0, f + 1 == first + count ? rgb : NULL);
    }
    return ok;
}

/* Mean RGB over a raster line's samples [x0, x1) (window samples). */
static void mean_rgb(const VideoGPUChain *v, const float *rgb, int row, int x0, int x1, double out[3]) {
    out[0] = out[1] = out[2] = 0;
    for (int x = x0; x < x1; x++)
        for (int ch = 0; ch < 3; ch++) out[ch] += rgb[((size_t)row * v->window.width + x) * 3 + ch];
    for (int ch = 0; ch < 3; ch++) out[ch] /= (x1 - x0);
}

/* Window samples of raster dots [dot0, dot1). */
static int dot_sample(const VideoGPUChain *v, int dot) { return (dot - v->window.start_dot) * v->window.spp; }

/* A $22 backdrop decodes in the border to the colour a $22 picture decodes
 * to: the border goes through the receiver, the decoder and the RGB
 * amplifiers with the picture. The probes stay 5 dots clear of the picture
 * and of the 2C02's grey pulse at dot 49, beyond the chroma filters' reach,
 * and span whole carrier cycles. A line comb mixes the bottom border with
 * the picture's last line, so it checks the sides only. */
static void backdrop_decodes(SDL_GPUDevice *gpu, VideoConnectionType conn, VideoCombType comb, const char *name) {
    SignalPrecompute sp; VideoChain c; VideoGPUChain v;
    CHECK(chain(gpu, &v, &c, &sp, SIGNAL_REGION_NTSC, conn, comb));
    static uint16_t codes[256 * 240];
    float *a = malloc(v.rgb_size), *b = malloc(v.rgb_size);
    set_backdrop(&v, &sp, 0x22);
    for (int i = 0; i < 256 * 240; i++) codes[i] = 0x22;
    CHECK(frames(gpu, &v, &sp, codes, 0, 8, a));
    for (int i = 0; i < 256 * 240; i++) codes[i] = 0x0f;
    CHECK(frames(gpu, &v, &sp, codes, 8, 8, b));
    double reference[3] = {0}, probe[3];
    int n = 0;
    for (int line = 60; line <= 180; line++) {
        mean_rgb(&v, a, line + v.window.picture_row, v.window.picture_x + 400, v.window.picture_x + 1600, probe);
        for (int ch = 0; ch < 3; ch++) reference[ch] += probe[ch];
        n++;
    }
    for (int ch = 0; ch < 3; ch++) reference[ch] /= n;
    double worst = 0;
    const int sides[2][2] = {{55, 61}, {324, 330}};
    for (int line = 60; line <= 180; line++)
        for (int s = 0; s < 2; s++) {
            mean_rgb(&v, b, line + v.window.picture_row, dot_sample(&v, sides[s][0]), dot_sample(&v, sides[s][1]), probe);
            for (int ch = 0; ch < 3; ch++) worst = fmax(worst, fabs(probe[ch] - reference[ch]));
        }
    double bottom = 0;
    if (comb == VIDEO_COMB_NONE)
        for (int line = 240; line <= 241; line++) {
            mean_rgb(&v, b, line + v.window.picture_row, dot_sample(&v, 100), dot_sample(&v, 220), probe);
            for (int ch = 0; ch < 3; ch++) bottom = fmax(bottom, fabs(probe[ch] - reference[ch]));
        }
    printf("Border %s: $22 picture %.3f %.3f %.3f; $22 border off it by %.4f at the sides, %.4f below\n",
           name, reference[0], reference[1], reference[2], worst, bottom);
    CHECK(worst < .01 && bottom < .01);
    CHECK(reference[2] > reference[0] + .2); /* sky blue, not black */
    free(a); free(b); video_gpu_destroy(&v, gpu);
}

/* Around the unblanked raster the tube is dark: blanked samples and the
 * vertical-blanking line emit nothing over a raised black floor, a drifting
 * APL bias and receiver noise; an underscanned raster ends where the spot
 * does; and with the raster filling the face its edge shows the border. */
static void dark_outside_raster(SDL_GPUDevice *gpu) {
    enum { W = 256, H = 480 };
    SignalPrecompute sp; VideoChain c; VideoGPUChain v;
    CHECK(chain(gpu, &v, &c, &sp, SIGNAL_REGION_NTSC, VIDEO_CONN_COMPOSITE, VIDEO_COMB_NONE));
    c.tv.black_floor = .05f; c.tv.apl_black_lift = 2; c.tv.noise_level = .05f;
    CHECK(video_gpu_set_beam_params(&v, gpu, W, H, 2, .3f, .5f));
    video_gpu_set_dynamic_state(&v, 0, 1, 0);
    set_backdrop(&v, &sp, 0x22);
    static uint16_t codes[256 * 240];
    for (int i = 0; i < 256 * 240; i++) codes[i] = 0x22;
    CHECK(frames(gpu, &v, &sp, codes, 0, 4, NULL));
    const DecodeWindow *w = &v.window;
    float *current = malloc(v.rgb_size);
    CHECK(gpu_buffer_download(gpu, v.buf_gun_current, current, v.rgb_size));
    int blanked = 0, lit = 0;
    for (int row = 0; row < w->lines; row++)
        for (int x = 0; x < w->width; x++) {
            size_t i = ((size_t)row * w->width + x) * 3;
            bool off = row < w->trace_row0 || row >= w->trace_row1 || x + .5f <= w->trace_x0 || x - .5f >= w->trace_x1;
            if (off) { CHECK(current[i] == 0 && current[i + 1] == 0 && current[i + 2] == 0); blanked++; }
            else if (current[i + 2] > 0) lit++;
        }
    printf("Flyback: %d blanked samples dark, %d unblanked lit, row 0 blanked\n", blanked, lit);
    CHECK(w->trace_row0 == 1 && blanked > 19000 && lit > 500000);
    /* An underscanned raster: every face pixel beyond the spot's reach of the
     * unblanked raster is exactly dark (the horizontal halo reaches 33
     * samples, the vertical spot 4 lines). */
    c.tv.h_size = c.tv.v_size = .9f;
    CHECK(frames(gpu, &v, &sp, codes, 4, 2, NULL));
    float *dx = malloc(W * H * 16), *dy = malloc(W * H * 16);
    uint16_t *out = malloc(W * H * 8);
    CHECK(gpu_buffer_download(gpu, v.buf_deflection_x, dx, W * H * 16));
    CHECK(gpu_buffer_download(gpu, v.buf_deflection_y, dy, W * H * 16));
    CHECK(gpu_buffer_download(gpu, v.buf_beam_rgba, out, W * H * 8));
    int dark = 0;
    for (int p = 0; p < W * H; p++) {
        float x = dx[p * 4 + 1], line = dy[p * 4 + 1];
        if (x < w->trace_x0 - 34 || x > w->trace_x1 + 34 || line < w->trace_row0 - 5 || line > w->trace_row1 + 5) {
            CHECK(out[p * 4] == 0 && out[p * 4 + 1] == 0 && out[p * 4 + 2] == 0);
            dark++;
        }
    }
    printf("Underscan 0.9: %d face pixels beyond the spot's reach, all dark\n", dark);
    CHECK(dark > W * H / 20);
    /* The raster fills the face: its left column is the border's. */
    c.tv.h_size = c.tv.v_size = 1;
    CHECK(frames(gpu, &v, &sp, codes, 6, 2, NULL));
    CHECK(gpu_buffer_download(gpu, v.buf_beam_rgba, out, W * H * 8));
    float edge = 0, inside = 0;
    for (int y = H / 4; y < 3 * H / 4; y++) {
        edge += gpu_half_to_float(out[(y * W) * 4 + 2]);
        inside += gpu_half_to_float(out[(y * W + 8) * 4 + 2]);
    }
    printf("Face edge column: blue %.4f against %.4f eight columns in\n", edge / (H / 2), inside / (H / 2));
    CHECK(edge > .5f * inside && inside > 0);
    free(current); free(dx); free(dy); free(out); video_gpu_destroy(&v, gpu);
}

/* The picture lands where it did before the border was decoded: the
 * deflection map in window coordinates minus the picture's offset is the
 * old picture coordinate, and end to end a one-dot column and a one-line
 * row land their light at the analytic place on the face. An RGB source
 * keeps upstream filters out of the centroid. */
static void picture_position(SDL_GPUDevice *gpu) {
    enum { W = 1024, H = 960 };
    SignalPrecompute sp; VideoChain c; VideoGPUChain v;
    CHECK(chain(gpu, &v, &c, &sp, SIGNAL_REGION_NTSC, VIDEO_CONN_RGB, VIDEO_COMB_NONE));
    float matrix[3][3] = {{1, 0, 0}, {1, 0, 0}, {1, 0, 0}}, bias[3] = {0};
    video_gpu_set_color_matrix(&v, matrix, bias);
    CHECK(video_gpu_set_beam_params(&v, gpu, W, H, 4, .3f, .5f));
    const DecodeWindow *w = &v.window;
    static uint16_t codes[256 * 240];
    for (int i = 0; i < 256 * 240; i++) codes[i] = (i % 256 == 128) ? 0x30 : 0x0f;
    set_backdrop(&v, &sp, 0x0f);
    CHECK(frames(gpu, &v, &sp, codes, 0, 2, NULL));
    float *dx = malloc(W * H * 16), *dy = malloc(W * H * 16);
    uint16_t *out = malloc(W * H * 8);
    CHECK(gpu_buffer_download(gpu, v.buf_deflection_x, dx, W * H * 16));
    CHECK(gpu_buffer_download(gpu, v.buf_deflection_y, dy, W * H * 16));
    float worst_x = 0, worst_y = 0;
    for (int oy = 0; oy < H; oy += 7)
        for (int ox = 0; ox < W; ox += 5) {
            int p = oy * W + ox;
            float old_x = ((ox + .5f) / W * w->active_dots - w->picture_left) * w->spp;
            float old_y = (oy + .5f) / H * w->active_lines - w->picture_top;
            worst_x = fmaxf(worst_x, fabsf(dx[p * 4 + 1] - w->picture_x - old_x));
            worst_y = fmaxf(worst_y, fabsf(dy[p * 4 + 1] - w->picture_row - old_y));
        }
    printf("Landing against the picture coordinates: %.2g samples, %.2g lines\n", worst_x, worst_y);
    CHECK(worst_x < 1e-3f && worst_y < 1e-4f);
    /* Column at picture dot 128: samples 1024 to 1031, centred on 1027.5. */
    CHECK(gpu_buffer_download(gpu, v.buf_beam_rgba, out, W * H * 8));
    double expect_x = ((1027.5 / w->spp + w->picture_left) / w->active_dots) * W - .5;
    double worst = 0;
    for (int oy = H / 8; oy < 7 * H / 8; oy += 16) {
        double sum = 0, moment = 0;
        for (int ox = (int)expect_x - 24; ox <= (int)expect_x + 24; ox++) {
            double light = gpu_half_to_float(out[(oy * W + ox) * 4 + 1]);
            sum += light; moment += light * ox;
        }
        if (sum > 0) worst = fmax(worst, fabs(moment / sum - expect_x));
        CHECK(sum > 0);
    }
    printf("Column at dot 128: centroid within %.4f px of %.2f\n", worst, expect_x);
    CHECK(worst < .05);
    /* Row at picture line 120, centred on 120.5. */
    for (int i = 0; i < 256 * 240; i++) codes[i] = (i / 256 == 120) ? 0x30 : 0x0f;
    CHECK(frames(gpu, &v, &sp, codes, 2, 2, NULL));
    CHECK(gpu_buffer_download(gpu, v.buf_beam_rgba, out, W * H * 8));
    double expect_y = (120.5 + w->picture_top) / w->active_lines * H - .5;
    worst = 0;
    for (int ox = W / 4; ox < 3 * W / 4; ox += 16) {
        double sum = 0, moment = 0;
        for (int oy = (int)expect_y - 24; oy <= (int)expect_y + 24; oy++) {
            double light = gpu_half_to_float(out[(oy * W + ox) * 4 + 1]);
            sum += light; moment += light * oy;
        }
        if (sum > 0) worst = fmax(worst, fabs(moment / sum - expect_y));
        CHECK(sum > 0);
    }
    printf("Row at line 120: centroid within %.4f px of %.2f\n", worst, expect_y);
    CHECK(worst < .05);
    free(dx); free(dy); free(out); video_gpu_destroy(&v, gpu);
}

/* The picture decodes as it did when the window was the picture alone:
 * a chain whose window is switched to that layout gives the same samples
 * away from the picture's edges, where only the amplifier's taps could see
 * the border. */
static void picture_interior(SDL_GPUDevice *gpu) {
    SignalPrecompute sp; VideoChain c, d; VideoGPUChain v, p;
    CHECK(chain(gpu, &v, &c, &sp, SIGNAL_REGION_NTSC, VIDEO_CONN_COMPOSITE, VIDEO_COMB_NONE));
    CHECK(chain(gpu, &p, &d, &sp, SIGNAL_REGION_NTSC, VIDEO_CONN_COMPOSITE, VIDEO_COMB_NONE));
    decode_window_picture(&p.window, SIGNAL_REGION_NTSC, sp.samples_per_pixel);
    set_backdrop(&v, &sp, 0x22); set_backdrop(&p, &sp, 0x22);
    static uint16_t codes[256 * 240];
    for (int i = 0; i < 256 * 240; i++) codes[i] = (uint16_t)(((i % 256) / 7 + (i / 256) / 5) % 64);
    float *a = malloc(v.rgb_size), *b = malloc(p.rgb_size);
    CHECK(frames(gpu, &v, &sp, codes, 0, 4, a));
    CHECK(frames(gpu, &p, &sp, codes, 0, 4, b));
    float worst = 0;
    for (int line = 1; line < 240; line++)
        for (int x = 48; x < 2048 - 48; x++)
            for (int ch = 0; ch < 3; ch++)
                worst = fmaxf(worst, fabsf(a[decode_window_rgb_index(&v.window, line, x) + ch]
                                         - b[decode_window_rgb_index(&p.window, line, x) + ch]));
    printf("Picture interior against the picture-only window: %g\n", worst);
    CHECK(worst < 1e-6f);
    free(a); free(b); video_gpu_destroy(&v, gpu); video_gpu_destroy(&p, gpu);
}

/* The 2C07 blanks its border: a PAL set shows black around the picture
 * whatever the backdrop, and above and below it, and emits nothing outside
 * the unblanked raster. */
static void pal_border(SDL_GPUDevice *gpu) {
    SignalPrecompute sp; VideoChain c; VideoGPUChain v;
    CHECK(chain(gpu, &v, &c, &sp, SIGNAL_REGION_PAL, VIDEO_CONN_COMPOSITE, VIDEO_COMB_NONE));
    c.tv.black_floor = .05f;
    set_backdrop(&v, &sp, 0x22);
    static uint16_t codes[256 * 240];
    for (int i = 0; i < 256 * 240; i++) codes[i] = 0x0f;
    float *rgb = malloc(v.rgb_size), *current = malloc(v.rgb_size);
    CHECK(frames(gpu, &v, &sp, codes, 0, 6, rgb));
    const DecodeWindow *w = &v.window;
    double black[3] = {0}, probe[3];
    for (int line = 60; line <= 180; line++) {
        mean_rgb(&v, rgb, line + w->picture_row, w->picture_x + 600, w->picture_x + 1800, probe);
        for (int ch = 0; ch < 3; ch++) black[ch] += probe[ch] / 121;
    }
    double worst = 0;
    int x0 = (int)ceilf(w->trace_x0) + 16, x1 = (int)floorf(w->trace_x1) - 16;
    for (int row = 0; row < w->lines; row++) {
        bool above_below = row < 20 || row >= 260;
        for (int x = x0; x < x1; x++) {
            bool side = x < w->picture_x - 24 || x >= w->picture_x + w->picture_w + 24;
            if (!above_below && !side) continue;
            for (int ch = 0; ch < 3; ch++)
                worst = fmax(worst, fabs(rgb[((size_t)row * w->width + x) * 3 + ch] - black[ch]));
        }
    }
    printf("PAL border and the lines above and below the picture: within %.4f of the picture's black\n", worst);
    CHECK(worst < .01);
    CHECK(gpu_buffer_download(gpu, v.buf_gun_current, current, v.rgb_size));
    int dark = 0;
    for (int row = 0; row < w->lines; row++)
        for (int x = 0; x < w->width; x++)
            if (x + .5f <= w->trace_x0 || x - .5f >= w->trace_x1) {
                size_t i = ((size_t)row * w->width + x) * 3;
                CHECK(current[i] == 0 && current[i + 1] == 0 && current[i + 2] == 0);
                dark++;
            }
    CHECK(dark > 20000);
    free(rgb); free(current); video_gpu_destroy(&v, gpu);
}

/* An RGB PPU puts out the backdrop where the 2C02 does (dots 50 to 331 of
 * lines 0 to 241) through its own palette, and blanking elsewhere. */
static const unsigned short rgb_2c03_22 = 0x447;
static void rgb_ppu_border(SDL_GPUDevice *gpu) {
    SignalPrecompute sp; VideoChain c; VideoGPUChain v;
    CHECK(chain(gpu, &v, &c, &sp, SIGNAL_REGION_NTSC, VIDEO_CONN_RGB, VIDEO_COMB_NONE));
    float matrix[3][3] = {{1, 0, 0}, {1, 0, 0}, {1, 0, 0}}, bias[3] = {0};
    video_gpu_set_color_matrix(&v, matrix, bias);
    set_backdrop(&v, &sp, 0x22);
    static uint16_t codes[256 * 240];
    for (int i = 0; i < 256 * 240; i++) codes[i] = 0x0f;
    float *amp = malloc(v.rgb_size), *dac = malloc(v.rgb_size);
    CHECK(frames(gpu, &v, &sp, codes, 0, 1, amp));
    /* Without a beam the amplifier's input, the PPU's output, is left in
     * the second buffer. */
    CHECK(gpu_buffer_download(gpu, v.buf_rgb2, dac, v.rgb_size));
    const DecodeWindow *w = &v.window;
    float border[3];
    for (int ch = 0; ch < 3; ch++) border[ch] = ((rgb_2c03_22 >> (8 - 4 * ch)) & 15) / 7.0f;
    int wrong = 0, lit = 0;
    for (int row = 0; row < w->lines; row++)
        for (int d = 0; d < w->dots; d++) {
            int line = row - w->picture_row, dot = w->start_dot + d;
            bool picture = line >= 0 && line < 240 && dot >= 65 && dot < 321;
            bool backdrop = !picture && line >= 0 && line < 242 && dot >= 50 && dot < 332;
            for (int ch = 0; ch < 3; ch++) {
                float expected = backdrop ? border[ch] : 0;
                if (fabsf(dac[((size_t)row * w->width + d * w->spp) * 3 + ch] - expected) > 1e-6f) wrong++;
            }
            lit += backdrop;
        }
    CHECK(wrong == 0 && lit > 5000);
    /* Through the amplifier, mid-border. */
    double probe[3];
    mean_rgb(&v, amp, 120, dot_sample(&v, 56), dot_sample(&v, 60), probe);
    printf("RGB PPU border: %d samples off the 2C03's $22 or blanking; amplified %.3f %.3f %.3f\n",
           wrong, probe[0], probe[1], probe[2]);
    for (int ch = 0; ch < 3; ch++) CHECK(fabs(probe[ch] - border[ch]) < .01);
    free(amp); free(dac); video_gpu_destroy(&v, gpu);
}

/* An RGB encoder source writes the decode window: its picture where the
 * picture goes, black around it. */
static void encoder_rgb_window(SDL_GPUDevice *gpu) {
    SignalPrecompute sp; VideoChain c; VideoGPUChain v;
    CHECK(chain(gpu, &v, &c, &sp, SIGNAL_REGION_NTSC, VIDEO_CONN_RGB, VIDEO_COMB_NONE));
    float matrix[3][3] = {{1, 0, 0}, {1, 0, 0}, {1, 0, 0}}, bias[3] = {0};
    video_gpu_set_color_matrix(&v, matrix, bias);
    enum { WIDTH = 256, LINES = 224, TOP = 8 };
    static uint32_t pixels[WIDTH * LINES];
    for (int i = 0; i < WIDTH * LINES; i++) pixels[i] = 1023u | (1023u << 10) | (1023u << 20);
    VideoRGBSource src = {.pixels = pixels, .width = WIDTH, .lines = LINES, .top_line = TOP,
                          .spp_num = sp.samples_per_pixel, .spp_den = 1, .code_bits = 10};
    float *amp = malloc(v.rgb_size), *out = malloc(v.rgb_size);
    CHECK(video_gpu_process_rgb(&v, gpu, &src));
    CHECK(video_gpu_download_window_rgb(&v, gpu, amp));
    CHECK(gpu_buffer_download(gpu, v.buf_rgb2, out, v.rgb_size));
    const DecodeWindow *w = &v.window;
    int wrong = 0;
    for (int row = 0; row < w->lines; row++)
        for (int x = 0; x < w->width; x++) {
            int line = row - w->picture_row, s = x - w->picture_x;
            float expected = line >= TOP && line < TOP + LINES && s >= 0 && s < w->picture_w ? 1 : 0;
            if (fabsf(out[((size_t)row * w->width + x) * 3 + 1] - expected) > 1e-6f) wrong++;
        }
    printf("RGB encoder source: %d window samples off the picture or black\n", wrong);
    CHECK(wrong == 0);
    free(amp); free(out); video_gpu_destroy(&v, gpu);
}

/* A PC monitor behind a scaler is fed the console's output as the scaler
 * digitises it, not a TV's blanked raster: the picture's first line, which
 * a TV blanks with the vertical interval, reaches the scaler's input like
 * any other, and a TV still blanks it. */
static void scaler_sees_first_line(SDL_GPUDevice *gpu) {
    for (int monitor = 0; monitor < 2; monitor++) {
        SignalPrecompute sp; VideoChain c; VideoGPUChain v;
        CHECK(chain(gpu, &v, &c, &sp, SIGNAL_REGION_NTSC, VIDEO_CONN_COMPOSITE, VIDEO_COMB_NONE));
        c.tv.monitor_model = monitor;
        set_backdrop(&v, &sp, 0x0f);
        static uint16_t codes[256 * 240];
        for (int i = 0; i < 256 * 240; i++) codes[i] = 0x30;
        float *rgb = malloc(v.rgb_size);
        CHECK(frames(gpu, &v, &sp, codes, 0, 4, rgb));
        double first[3], middle[3];
        const DecodeWindow *w = &v.window;
        mean_rgb(&v, rgb, w->picture_row, w->picture_x + 64, w->picture_x + w->picture_w - 64, first);
        mean_rgb(&v, rgb, w->picture_row + 120, w->picture_x + 64, w->picture_x + w->picture_w - 64, middle);
        printf("Picture line 0 %s: green %.4f against %.4f at line 120\n",
               monitor ? "into the FW900's scaler" : "on a TV", first[1], middle[1]);
        if (monitor) CHECK(middle[1] > .5 && fabs(first[1] - middle[1]) < .01);
        else CHECK(middle[1] > .5 && first[1] == 0);
        free(rgb); video_gpu_destroy(&v, gpu);
    }
}

/* The colour oscillator runs through the whole time between fields: on the
 * frame after the one the 2C02 shortens by a dot, and on the one after
 * that, the top of the field decodes with the hue of its middle, where the
 * burst loop has long settled. */
static void field_top_hue(SDL_GPUDevice *gpu) {
    SignalPrecompute sp; VideoChain c; VideoGPUChain v;
    CHECK(chain(gpu, &v, &c, &sp, SIGNAL_REGION_NTSC, VIDEO_CONN_COMPOSITE, VIDEO_COMB_NONE));
    set_backdrop(&v, &sp, 0x22);
    static uint16_t codes[256 * 240];
    for (int i = 0; i < 256 * 240; i++) codes[i] = 0x22;
    float *rgb = malloc(v.rgb_size);
    const DecodeWindow *w = &v.window;
    double worst = 0;
    for (unsigned f = 0; f < 10; f++) {
        CHECK(frames(gpu, &v, &sp, codes, f, 1, rgb));
        if (f < 6) continue;
        double top[3] = {0}, middle[3] = {0}, probe[3];
        for (int line = 2; line < 20; line++) {
            mean_rgb(&v, rgb, w->picture_row + line, w->picture_x + 400, w->picture_x + 1600, probe);
            for (int ch = 0; ch < 3; ch++) top[ch] += probe[ch] / 18;
        }
        for (int line = 110; line < 128; line++) {
            mean_rgb(&v, rgb, w->picture_row + line, w->picture_x + 400, w->picture_x + 1600, probe);
            for (int ch = 0; ch < 3; ch++) middle[ch] += probe[ch] / 18;
        }
        for (int ch = 0; ch < 3; ch++) worst = fmax(worst, fabs(top[ch] - middle[ch]));
    }
    printf("Field top against middle over four frames: within %.4f\n", worst);
    CHECK(worst < .01);
    free(rgb); video_gpu_destroy(&v, gpu);
}

/* An isolated line carries the light of a line of a uniform field whatever
 * the output row's pitch in raster lines: each row integrates the spot over
 * the lines it covers on the face, 287 active PAL lines, or an NTSC field
 * enlarged by 4% overscan, on 240 output rows. The spot is narrower than a
 * row, so a row that integrated a whole line would be off by the ratio of
 * the pitches. */
static void line_energy(SDL_GPUDevice *gpu) {
    enum { W = 32, H = 240 };
    const struct { int region; float overscan; } cases[] = {{SIGNAL_REGION_PAL, 0}, {SIGNAL_REGION_NTSC, .04f}};
    for (int k = 0; k < 2; k++) {
        SignalPrecompute sp; VideoChain c; VideoGPUChain v;
        CHECK(chain(gpu, &v, &c, &sp, cases[k].region, VIDEO_CONN_RGB, VIDEO_COMB_NONE));
        float matrix[3][3] = {{1, 0, 0}, {1, 0, 0}, {1, 0, 0}}, bias[3] = {0};
        video_gpu_set_color_matrix(&v, matrix, bias);
        c.tv.overscan = cases[k].overscan;
        CHECK(video_gpu_set_beam_params(&v, gpu, W, H, 1, .1f, .1f));
        set_backdrop(&v, &sp, 0x0f);
        static uint16_t codes[256 * 240];
        uint16_t *out = malloc(W * H * 8);
        double field = 0, line = 0;
        for (int pass = 0; pass < 2; pass++) {
            for (int i = 0; i < 256 * 240; i++) codes[i] = pass == 0 || i / 256 == 120 ? 0x30 : 0x0f;
            CHECK(frames(gpu, &v, &sp, codes, 2 * pass, 2, NULL));
            CHECK(gpu_buffer_download(gpu, v.buf_beam_rgba, out, W * H * 8));
            /* The field's mean over whole rows: a spot this narrow puts one
             * line in some rows and two in others. */
            if (pass == 0) for (int y = H / 4; y < 3 * H / 4; y++) field += gpu_half_to_float(out[(y * W + W / 2) * 4 + 1]) / (H / 2);
            else for (int y = 0; y < H; y++) line += gpu_half_to_float(out[(y * W + W / 2) * 4 + 1]);
        }
        double pitch = v.window.active_lines * (1 - 2 * cases[k].overscan) / H;
        printf("Line energy %s: %.4f of a uniform field's line at %.3f lines per row\n",
               k ? "NTSC, 4% overscan" : "PAL", line * pitch / field, pitch);
        CHECK(field > .5 && fabs(line * pitch / field - 1) < .02);
        free(out); video_gpu_destroy(&v, gpu);
    }
}

/* Monitors that show the whole active raster, border included: studio and
 * reference monitors in underscan, arcade and computer monitors, and the
 * PC monitor behind a scaler. Every other shipped preset is a household
 * set, whose service alignment hides the border. */
static const char *const border_monitors[] = {
    "sony_pvm_14l2", "sony_pvm_20m4u", "studio_pvm", "reference_composite", "measured_glare_experiment",
    "nec_xm29_arcade", "arcade_cabinet", "sony_gdm_fw900", "commodore_1702", "warm_desktop_monitor",
};

/* The landing at the edge of the visible glass along n output pixels (a
 * row, or a column with stride the width): the display pass clips the
 * light where the tube-face warp of the output coordinate, with curvature
 * k along the scan and the other coordinate at other_uv, leaves [0, 1]
 * (crt_display.frag.glsl), so the landing is interpolated to that point. */
static float glass_inside(int j, int n, float k, float other_uv, bool far_edge) {
    float u = (j + .5f) / n - .5f, o = other_uv - .5f;
    float face = u * (1 + k * (u * u + o * o)) + .5f;
    return far_edge ? 1 - face : face;
}
static float glass_edge(const float *map, int n, int stride, float k, float other_uv, bool far_edge) {
    for (int i = 1; i < n; i++) {
        int j = far_edge ? n - 1 - i : i;
        if (glass_inside(j, n, k, other_uv, far_edge) < 0) continue;
        /* The glass edge lies between this pixel and the one before it, or
         * half a pixel outside the first one. */
        int a = far_edge ? j + 1 : j - 1;
        float ia = glass_inside(a, n, k, other_uv, far_edge), ib = glass_inside(j, n, k, other_uv, far_edge);
        float la = map[(size_t)a * stride * 4 + 1], lb = map[(size_t)j * stride * 4 + 1];   /* green */
        return la + (lb - la) * -ia / (ib - ia);
    }
    return NAN;
}

/* A household set's service alignment hides the 2C02's border: through the
 * deflection map, the glass edges land within 2 dots of the picture at the
 * sides and within a line of it at the bottom (the border is 14.5 dots of
 * the 282.7-dot active line on the left, 11 dots and 1.2 of blanking on the
 * right, two lines below). A monitor shows most of the side border. */
static void household_overscan(SDL_GPUDevice *gpu) {
    enum { W = 1128, H = 846 };
    static char names[64][128], paths[64][512];
    int count = preset_json_scan_dir(MYNES_TEST_PRESET_DIR, names, paths, 64);
    CHECK(count >= 20);
    SignalPrecompute sp; VideoChain c; VideoGPUChain v;
    CHECK(chain(gpu, &v, &c, &sp, SIGNAL_REGION_NTSC, VIDEO_CONN_COMPOSITE, VIDEO_COMB_NONE));
    CHECK(video_gpu_set_beam_params(&v, gpu, W, H, 4, .3f, .5f));
    static uint16_t codes[256 * 240];
    for (int i = 0; i < 256 * 240; i++) codes[i] = 0x0f;
    set_backdrop(&v, &sp, 0x0f);
    float *dx = malloc(W * H * 16), *dy = malloc(W * H * 16);
    const DecodeWindow *w = &v.window;
    int households = 0;
    for (int k = 0; k < count; k++) {
        PhysicalPreset p;
        CHECK(preset_json_load(&p, paths[k]));
        bool monitor = false;
        for (size_t m = 0; m < sizeof border_monitors / sizeof *border_monitors; m++)
            monitor |= strcmp(names[k], border_monitors[m]) == 0;
        c.tv.overscan = p.tv.overscan; c.tv.h_size = p.tv.h_size; c.tv.v_size = p.tv.v_size;
        c.tv.h_pos = p.tv.h_pos; c.tv.v_pos = p.tv.v_pos;
        c.tv.barrel = p.tv.barrel; c.tv.barrel_v = p.tv.barrel_v;
        c.tv.keystone = p.tv.keystone; c.tv.rotation = p.tv.rotation;
        c.tv.skew_x = p.tv.skew_x; c.tv.skew_y = p.tv.skew_y;
        CHECK(frames(gpu, &v, &sp, codes, 2 * k, 1, NULL));
        CHECK(gpu_buffer_download(gpu, v.buf_deflection_x, dx, W * H * 16));
        CHECK(gpu_buffer_download(gpu, v.buf_deflection_y, dy, W * H * 16));
        float kh = p.tv.barrel, kv = p.tv.barrel_v != 0 ? p.tv.barrel_v : p.tv.barrel;
        float mid_v = (H / 2 + .5f) / H, mid_u = (W / 2 + .5f) / W;
        const float *row = dx + (size_t)(H / 2) * W * 4, *col = dy + (size_t)(W / 2) * 4;
        float x0 = glass_edge(row, W, 1, kh, mid_v, false), x1 = glass_edge(row, W, 1, kh, mid_v, true);
        float y1 = glass_edge(col, H, W, kv, mid_u, true);
        float left = SIGNAL_PICTURE_DOT - (w->start_dot + x0 / w->spp);
        float right = (w->start_dot + x1 / w->spp) - (SIGNAL_PICTURE_DOT + SIGNAL_NES_WIDTH);
        float bottom = (y1 - w->picture_row) - SIGNAL_NES_HEIGHT;
        printf("  %-26s %s: border %5.2f dots left, %5.2f right, %5.2f lines below\n", names[k],
               monitor ? "monitor  " : "household", left, right, bottom);
        if (monitor) {
            CHECK(left > 8 && right > 8);
        } else {
            CHECK(left <= 2 && right <= 2 && bottom <= 1);
            households++;
        }
    }
    printf("Household sets: %d of %d presets hide the border to 2 dots\n", households, count);
    CHECK(households >= 12);
    free(dx); free(dy); video_gpu_destroy(&v, gpu);
}

int test_border(SDL_GPUDevice *gpu) {
    window_arithmetic();
    household_overscan(gpu);
    backdrop_decodes(gpu, VIDEO_CONN_COMPOSITE, VIDEO_COMB_NONE, "composite");
    backdrop_decodes(gpu, VIDEO_CONN_SVIDEO, VIDEO_COMB_NONE, "S-Video");
    backdrop_decodes(gpu, VIDEO_CONN_COMPOSITE, VIDEO_COMB_3LINE, "composite, 3-line comb");
    dark_outside_raster(gpu);
    picture_position(gpu);
    picture_interior(gpu);
    pal_border(gpu);
    rgb_ppu_border(gpu);
    encoder_rgb_window(gpu);
    line_energy(gpu);
    scaler_sees_first_line(gpu);
    field_top_hue(gpu);
    return failures;
}
