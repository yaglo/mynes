/*
 * mynes_video -- a picture or a video through the MyNES signal chain
 * ====================================================================
 *
 * Takes a still picture or a video, encodes each frame as an NTSC
 * composite signal the way a console's RGB encoder IC does (the RGB
 * encoder source, encoder_rgb.comp.glsl, with linear 10-bit gun voltages),
 * and runs it through a preset's whole chain: cable or RF, the optional VHS
 * deck, the television's receiver, Y/C separation and decoder, and the
 * tube. The result is recorded with the same recorder as mynes_gpu
 * --record, SDR or HDR, and the source's sound is muxed back in.
 *
 * The chain runs at the NES's timing: 262 progressive lines per field at
 * 60.0988 Hz, 341 dots of 8 samples per line (227 1/3 subcarrier cycles),
 * with the 2C02's alternating frame phase. So dot crawl and comb artefacts
 * have a console's pattern, not broadcast NTSC's (227.5 cycles per line,
 * 525 interlaced lines), and a video is shown at 240 lines per field. The
 * picture fills the 256-dot, 240-line area a console picture uses, so the
 * tube shows the same narrow side borders, black at the encoder's pedestal.
 *
 * Frames come from ffmpeg: resampled to 60.0988 frames per second, fitted
 * to or cropped to the picture area's shape, and scaled to 1024 x 240
 * gamma-encoded R'G'B', which the encoder matrixes with Rec. 601 luma as an
 * NTSC encoder does. A still is repeated for --seconds.
 *
 * usage: mynes_video [options] INPUT OUTPUT.mov|OUTPUT.mp4
 */

#include <SDL3/SDL.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <signal.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/wait.h>
#include <unistd.h>

#include "signal_format.h"
#include "signal_precompute.h"
#include "video_chain.h"
#include "video_gpu.h"
#include "audio_chain.h"
#include "gpu_render.h"
#include "gpu_display.h"
#include "gpu_output.h"
#include "preset_apply.h"
#include "recorder.h"
#include "config.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

extern char **environ;

/* The picture area: 1024 pixels of 2 samples across the 2048-sample (256
 * dot) active picture, 240 lines. */
enum { SRC_W = 1024, SRC_H = 240, DOTS_PER_LINE = 341, LINE_PHASE = 4 };
/* Carrier phase advance per frame, alternating as on the 2C02 with
 * rendering on (one dot short on odd frames). */
static const int FRAME_PHASE_ADV[2] = {4, 8};

/* Shape of the picture area on the tube: the receiver's active line is
 * 282.73 dots and 241 lines (BT.470 blanking) shown at 4:3, and the
 * picture is 256 dots by 240 lines of it (tools/showcase/pipeline/recipes.py
 * derives the same numbers). */
static double picture_aspect(void) {
    const double dot_us = 1e6 * 8 / (12 * 3579545.0);
    const double active_dots = (1e6 / 15734.264 - 10.9) / dot_us;
    return 4.0 / 3.0 * (256.0 / active_dots) / (240.0 / 241.0);
}

/* ============================================================================
 * Child processes
 * ============================================================================ */

/* Start argv with its stdout on a pipe returned as *out; stderr is kept. */
static bool spawn_reader(char *const argv[], pid_t *pid, FILE **out) {
    int fds[2];
    if (pipe(fds)) return false;
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_adddup2(&actions, fds[1], 1);
    posix_spawn_file_actions_addclose(&actions, fds[0]);
    posix_spawn_file_actions_addclose(&actions, fds[1]);
    posix_spawn_file_actions_addopen(&actions, 0, "/dev/null", O_RDONLY, 0);
    int rc = posix_spawnp(pid, argv[0], &actions, NULL, argv, environ);
    posix_spawn_file_actions_destroy(&actions);
    close(fds[1]);
    if (rc) { close(fds[0]); errno = rc; return false; }
    *out = fdopen(fds[0], "rb");
    return *out != NULL;
}

static int wait_child(pid_t pid) {
    int status = 0;
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

/* Run argv and return its stdout (trimmed) in out. */
static bool run_capture(char *const argv[], char *out, size_t n) {
    pid_t pid; FILE *f;
    if (!spawn_reader(argv, &pid, &f)) return false;
    size_t used = fread(out, 1, n - 1, f);
    out[used] = 0;
    fclose(f);
    while (used && (out[used - 1] == '\n' || out[used - 1] == '\r' || out[used - 1] == ' ')) out[--used] = 0;
    return wait_child(pid) == 0;
}

static bool run_quiet(char *const argv[]) {
    pid_t pid;
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_addopen(&actions, 0, "/dev/null", O_RDONLY, 0);
    int rc = posix_spawnp(&pid, argv[0], &actions, NULL, argv, environ);
    posix_spawn_file_actions_destroy(&actions);
    return rc == 0 && wait_child(pid) == 0;
}

/* ============================================================================
 * Input
 * ============================================================================ */

typedef struct {
    const char *path;
    bool still;          /* one picture, repeated */
    bool has_audio;
    double duration;     /* seconds of the input from start; 0 when unknown */
} Input;

static bool has_image_extension(const char *path) {
    static const char *const ext[] = {".png", ".jpg", ".jpeg", ".webp", ".bmp", ".tif", ".tiff",
                                      ".heic", ".avif", ".ppm", ".pgm", ".tga", ".exr", NULL};
    const char *dot = strrchr(path, '.');
    if (!dot) return false;
    for (int i = 0; ext[i]; i++) if (!strcasecmp(dot, ext[i])) return true;
    return false;
}

static bool probe(Input *in, const char *ffprobe) {
    char out[256];
    char *dur[] = {(char *)ffprobe, "-v", "error", "-show_entries", "format=duration",
                   "-of", "default=nw=1:nk=1", (char *)in->path, NULL};
    if (!run_capture(dur, out, sizeof out)) {
        fprintf(stderr, "%s: ffprobe cannot read it\n", in->path);
        return false;
    }
    in->duration = atof(out);
    char *aud[] = {(char *)ffprobe, "-v", "error", "-select_streams", "a", "-show_entries",
                   "stream=index", "-of", "csv=p=0", (char *)in->path, NULL};
    in->has_audio = run_capture(aud, out, sizeof out) && out[0];
    in->still = has_image_extension(in->path) || !(in->duration > 0);
    return true;
}

/* ffmpeg decoding INPUT to SRC_W x SRC_H rgb48le at the chain's frame rate:
 * pixel aspect made square, then cropped (fill) or padded (fit) to the
 * picture area's shape, then scaled with Lanczos. A still is decoded once;
 * the caller repeats the last picture it read. */
static bool start_decoder(const Input *in, const char *ffmpeg, double start, double seconds, bool fit,
                          pid_t *pid, FILE **out) {
    char filter[1024], aspect[32], sec[32], ss[32], size[32];
    snprintf(aspect, sizeof aspect, "%.6f", picture_aspect());
    snprintf(sec, sizeof sec, "%.6f", seconds);
    snprintf(ss, sizeof ss, "%.6f", start);
    snprintf(size, sizeof size, "%d:%d", SRC_W, SRC_H);
    char rate[48] = "";
    if (!in->still) snprintf(rate, sizeof rate, "fps=%s,", recorder_rate_string(SIGNAL_REGION_NTSC));
    if (fit)
        snprintf(filter, sizeof filter,
                 "%sscale=iw*sar:ih:flags=lanczos,setsar=1,"
                 "pad=w='max(iw,ceil(ih*%s/2)*2)':h='max(ih,ceil(iw/%s/2)*2)':x=(ow-iw)/2:y=(oh-ih)/2:color=black,"
                 "scale=%s:flags=lanczos+accurate_rnd+full_chroma_int,setsar=1",
                 rate, aspect, aspect, size);
    else
        snprintf(filter, sizeof filter,
                 "%sscale=iw*sar:ih:flags=lanczos,setsar=1,"
                 "crop=w='min(iw,ih*%s)':h='min(ih,iw/%s)',"
                 "scale=%s:flags=lanczos+accurate_rnd+full_chroma_int,setsar=1",
                 rate, aspect, aspect, size);
    char *argv[40]; int n = 0;
    argv[n++] = (char *)ffmpeg; argv[n++] = "-nostdin"; argv[n++] = "-v"; argv[n++] = "error";
    if (!in->still && start > 0) { argv[n++] = "-ss"; argv[n++] = ss; }
    argv[n++] = "-i"; argv[n++] = (char *)in->path;
    if (in->still) { argv[n++] = "-frames:v"; argv[n++] = "1"; }
    else { argv[n++] = "-t"; argv[n++] = sec; }
    argv[n++] = "-an"; argv[n++] = "-vf"; argv[n++] = filter;
    argv[n++] = "-f"; argv[n++] = "rawvideo"; argv[n++] = "-pix_fmt"; argv[n++] = "rgb48le"; argv[n++] = "-";
    argv[n] = NULL;
    return spawn_reader(argv, pid, out);
}

/* One decoded frame into 10-bit codes; false at the end of the input. */
static bool read_frame(FILE *f, uint16_t *rgb48, uint32_t *codes, float *brightness) {
    size_t want = (size_t)SRC_W * SRC_H * 3;
    if (fread(rgb48, sizeof(uint16_t), want, f) != want) return false;
    double luma = 0;
    for (size_t i = 0; i < (size_t)SRC_W * SRC_H; i++) {
        uint32_t r = rgb48[3 * i] >> 6, g = rgb48[3 * i + 1] >> 6, b = rgb48[3 * i + 2] >> 6;
        codes[i] = r | (g << 10) | (b << 20);
        luma += 0.299 * r + 0.587 * g + 0.114 * b;
    }
    *brightness = (float)(luma / (1023.0 * SRC_W * SRC_H));
    return true;
}

/* ============================================================================
 * Chain
 * ============================================================================ */

static SignalPrecompute sig_state;
static VideoChain video_chain;
static AudioChain audio_chain;
static VideoGPUChain video_gpu_chain;
static GPUDisplay gpu_disp;
static GPURenderCtx render_ctx;
static PresetCtx preset_ctx;
static MynesConfig mynes_config;
static PPU display_ppu;
static APUAnalog analog_controls;
static bool gpu_video_enabled, gpu_audio_enabled;
static int use_gpu_audio;
static int current_preset = -1;

typedef struct {
    float luma_mhz, chroma_mhz, trap, setup;
} Encoder;

static VideoRGBSource source_for(const uint32_t *codes, int phase, const Encoder *enc) {
    return (VideoRGBSource){
        .pixels = codes, .width = SRC_W, .lines = SRC_H, .top_line = 0,
        .spp_num = SIGNAL_NTSC_SAMPLES_PER_LINE / SRC_W, .spp_den = 1,
        .phase_base = phase, .phase_line_adv = LINE_PHASE,
        .chroma_bw_hz = enc->chroma_mhz * 1e6f, .luma_bw_hz = enc->luma_mhz * 1e6f,
        .luma_trap = enc->trap, .setup = enc->setup, .code_bits = 10};
}

/* Fit the tube aspect into the output, two device rows per scanline at
 * least, as mynes_gpu does. */
static void beam_target_size(int out_w, int out_h, int *w, int *h) {
    int aw = video_chain.tv.monitor_model == 1 ? 16 : 4, ah = video_chain.tv.monitor_model == 1 ? 10 : 3;
    int bw = out_w, bh = out_h;
    if (bw * ah > bh * aw) bw = bh * aw / ah; else bh = bw * ah / aw;
    int fit_h = bh;
    if (bh < 480) bh = 480;
    *w = bh == fit_h ? bw : bh * aw / ah;
    *h = bh;
}

static bool setup_beam(SDL_GPUDevice *gpu, int out_w, int out_h) {
    int beam_w, beam_h;
    beam_target_size(out_w, out_h, &beam_w, &beam_h);
    const TVDisplayParams *tv = &video_chain.tv;
    render_ctx.display_tex = NULL;
    if (!video_gpu_set_beam_params(&video_gpu_chain, gpu, beam_w, beam_h, beam_h / 240 > 0 ? beam_h / 240 : 1,
                                   video_beam_sigma(tv, false), video_beam_sigma(tv, true)))
        return false;
    video_gpu_chain.beam_sigma_narrow = video_beam_sigma(tv, false);
    video_gpu_chain.beam_sigma_wide = video_beam_sigma(tv, true);
    video_gpu_chain.beam_h_blur_sigma = tv->beam_spot_size > 0 ? tv->beam_spot_size : 6.0f;
    return true;
}

/* Auto gain fits the preset's own white: a full-code white field through
 * the encoder and the chain until its loops settle, measured through the
 * display pass, then the temporal state is cleared. As mynes_retro. */
static bool calibrate_white(SDL_GPUDevice *gpu, const Encoder *enc) {
    static uint32_t white[SRC_W * SRC_H];
    for (int i = 0; i < SRC_W * SRC_H; i++) white[i] = 1023u | (1023u << 10) | (1023u << 20);
    VideoRGBSource src = source_for(white, 0, enc);
    for (int i = 0; i < 24; i++)
        if (!video_gpu_process_rgb(&video_gpu_chain, gpu, &src)) return false;
    bool measured = false;
    int beam_w, beam_h;
    SDL_GPUTexture *beam = video_gpu_get_beam_texture(&video_gpu_chain);
    if (beam && video_gpu_get_beam_size(&video_gpu_chain, &beam_w, &beam_h)) {
        render_ctx.display_tex = beam; render_ctx.display_tex_w = beam_w; render_ctx.display_tex_h = beam_h;
        render_ctx.owns_display_tex = false;
        measured = gpu_render_measure_white(&render_ctx, &video_chain);
    }
    video_gpu_reset_temporal_state(&video_gpu_chain, gpu);
    return measured;
}

/* One field through the chain and the tube; with a sink, the final image
 * goes to the recorder. */
static bool render_field(SDL_GPUDevice *gpu, unsigned field, int *phase, const uint32_t *codes,
                         float brightness, const Encoder *enc, Recorder *recorder) {
    while (!gpu_render_prepare(&render_ctx)) SDL_Delay(1);
    VideoRGBSource src = source_for(codes, *phase, enc);
    render_ctx.frame_brightness = brightness;
    gpu_render_update_dynamic_state(&render_ctx);
    if (!video_gpu_set_osd(&video_gpu_chain, gpu, NULL)) return false;
    video_gpu_chain.elapsed_frames = 1;
    video_gpu_chain.signal_frame_counter = field;
    video_gpu_chain.beam_frame_counter = field;
    render_ctx.frame_counter = field;
    render_ctx.source_phase = *phase;
    /* The demodulator reference follows the field's carrier phase, as
     * mynes_gpu does with the PPU clock. */
    video_gpu_set_demod(&video_gpu_chain, (float)*phase * 2.0f * (float)M_PI / 12.0f, video_gpu_chain.demod_dp);
    if (render_ctx.white_dirty && render_ctx.hdr_enabled && render_ctx.hdr_gain_mode == 0) {
        static unsigned failures;
        if (calibrate_white(gpu, enc)) { render_ctx.white_dirty = false; failures = 0; }
        else if (++failures >= 3) { render_ctx.white_dirty = false; fprintf(stderr, "White: measurement failed\n"); }
    }
    video_gpu_set_dynamic_state(&video_gpu_chain, render_ctx.hv_sag_state, render_ctx.apl_slow_state, 0.0f);
    if (!video_gpu_process_rgb(&video_gpu_chain, gpu, &src)) {
        fprintf(stderr, "GPU chain failed on field %u: %s\n", field, SDL_GetError());
        return false;
    }
    *phase = (*phase + FRAME_PHASE_ADV[field & 1]) % 12;
    int beam_w, beam_h;
    if (video_gpu_get_beam_size(&video_gpu_chain, &beam_w, &beam_h)) {
        render_ctx.display_tex = video_gpu_get_beam_texture(&video_gpu_chain);
        render_ctx.display_tex_w = beam_w;
        render_ctx.display_tex_h = beam_h;
    }
    unsigned before = recorder ? recorder_frames_written(recorder) : 0;
    render_ctx.capture_sink = recorder ? recorder_push_frame : NULL;
    render_ctx.capture_sink_user = recorder;
    render_ctx.capture_path = NULL;
    render_ctx.capture_async = false;
    gpu_render_frame(&render_ctx, &video_chain);
    render_ctx.capture_sink = NULL;
    if (recorder && recorder_frames_written(recorder) != before + 1) {
        fprintf(stderr, "Recording: field %u was not captured\n", field);
        return false;
    }
    return true;
}

/* ============================================================================
 * Main
 * ============================================================================ */

static void usage(void) {
    fprintf(stderr,
        "usage: mynes_video [options] INPUT OUTPUT.mov|OUTPUT.mp4\n"
        "  --preset NAME|FILE   television preset (repeatable: one output per preset,\n"
        "                       named OUTPUT-<preset>); default vhs_sp_consumer\n"
        "  --list-presets       print the bundled presets and exit\n"
        "  --size WxH           output size (default 1920x1440)\n"
        "  --hdr                BT.2020 PQ HDR (ProRes 4444 .mov)\n"
        "  --seconds N          length: a still is shown N seconds (default 5); a video\n"
        "                       is cut to N seconds (default: to its end)\n"
        "  --start S            start S seconds into a video\n"
        "  --fit                pad to the picture area instead of cropping to it\n"
        "  --mask-alignment pixels|physical   (default physical)\n"
        "  --settle N           fields run on the first frame before recording (default 60)\n"
        "  --luma-mhz F --chroma-mhz F --trap F --setup F   encoder: luma band (5),\n"
        "                       chroma band (1.3), subcarrier trap depth (0), black\n"
        "                       pedestal as a fraction of white (0; 0.075 is US setup)\n"
        "  --no-audio           do not carry the input's sound over\n"
        "OUTPUT.mov is ProRes 4444 (4:4:4, plays in QuickTime); OUTPUT.mp4 is H.264\n"
        "4:2:0 for playback anywhere, which halves the colour resolution of the mask.\n");
}

static void output_for(const char *output, const char *preset, int count, char *out, size_t n) {
    if (count <= 1) { snprintf(out, n, "%s", output); return; }
    const char *dot = strrchr(output, '.');
    const char *base = strrchr(preset, '/');
    base = base ? base + 1 : preset;
    char stem[256];
    snprintf(stem, sizeof stem, "%s", base);
    char *ext = strstr(stem, ".json");
    if (ext) *ext = 0;
    snprintf(out, n, "%.*s-%s%s", (int)(dot - output), output, stem, dot);
}

int main(int argc, char **argv) {
    const char *input_path = NULL, *output = NULL;
    const char *presets[32]; int preset_count = 0;
    int out_w = 1920, out_h = 1440, mask_alignment = 1, settle = 60;
    bool hdr = false, fit = false, no_audio = false, list = false;
    double seconds = 0, start = 0;
    Encoder enc = {5.0f, 1.3f, 0.0f, 0.0f};
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        bool more = i + 1 < argc;
        if (!strcmp(a, "--preset") && more) { if (preset_count < 32) presets[preset_count++] = argv[++i]; }
        else if (!strcmp(a, "--list-presets")) list = true;
        else if (!strcmp(a, "--size") && more) {
            if (sscanf(argv[++i], "%dx%d", &out_w, &out_h) != 2 || out_w < 64 || out_h < 48) { usage(); return 2; }
        }
        else if (!strcmp(a, "--hdr")) hdr = true;
        else if (!strcmp(a, "--seconds") && more) seconds = atof(argv[++i]);
        else if (!strcmp(a, "--start") && more) start = atof(argv[++i]);
        else if (!strcmp(a, "--fit")) fit = true;
        else if (!strcmp(a, "--fill")) fit = false;
        else if (!strcmp(a, "--no-audio")) no_audio = true;
        else if (!strcmp(a, "--settle") && more) settle = atoi(argv[++i]);
        else if (!strcmp(a, "--mask-alignment") && more) {
            const char *m = argv[++i];
            if (!strcmp(m, "pixels")) mask_alignment = 0;
            else if (!strcmp(m, "physical")) mask_alignment = 1;
            else { usage(); return 2; }
        }
        else if (!strcmp(a, "--luma-mhz") && more) enc.luma_mhz = (float)atof(argv[++i]);
        else if (!strcmp(a, "--chroma-mhz") && more) enc.chroma_mhz = (float)atof(argv[++i]);
        else if (!strcmp(a, "--trap") && more) enc.trap = (float)atof(argv[++i]);
        else if (!strcmp(a, "--setup") && more) enc.setup = (float)atof(argv[++i]);
        else if (!strcmp(a, "-h") || !strcmp(a, "--help")) { usage(); return 0; }
        else if (a[0] == '-') { fprintf(stderr, "unknown option %s\n", a); usage(); return 2; }
        else if (!input_path) input_path = a;
        else if (!output) output = a;
        else { usage(); return 2; }
    }
    if (!preset_count) presets[preset_count++] = "vhs_sp_consumer";
    if (!list) {
        if (!input_path || !output) { usage(); return 2; }
        const char *dot = strrchr(output, '.');
        bool mov = dot && !strcasecmp(dot, ".mov"), mp4 = dot && !strcasecmp(dot, ".mp4");
        if (!mov && !mp4) { fprintf(stderr, "OUTPUT must end in .mov or .mp4\n"); return 2; }
        if (hdr && !mov) { fprintf(stderr, "--hdr writes ProRes: OUTPUT must be .mov\n"); return 2; }
        if (settle < 0 || seconds < 0 || start < 0) { usage(); return 2; }
    }

    const char *ffmpeg = getenv("MYNES_FFMPEG") ? getenv("MYNES_FFMPEG") : "ffmpeg";
    const char *ffprobe = getenv("MYNES_FFPROBE") ? getenv("MYNES_FFPROBE") : "ffprobe";
    Input in = {.path = input_path};
    char still_png[1024] = "";
    if (!list) {
        if (!probe(&in, ffprobe)) return 1;
        /* A still in any format ffmpeg reads, tiled HEIF included (which
         * ffmpeg assembles in a filter graph of its own), becomes one
         * 16-bit PNG first, and the decoder reads that. */
        if (in.still) {
            const char *tmp = getenv("TMPDIR") ? getenv("TMPDIR") : "/tmp";
            snprintf(still_png, sizeof still_png, "%s/mynes_video-%d.png", tmp, (int)getpid());
            char *conv[] = {(char *)ffmpeg, "-nostdin", "-v", "error", "-y", "-i", (char *)input_path,
                            "-frames:v", "1", "-pix_fmt", "rgb48be", still_png, NULL};
            if (!run_quiet(conv)) { fprintf(stderr, "%s: ffmpeg cannot decode it\n", input_path); return 1; }
            in.path = still_png;
        }
        double available = in.still ? 0 : fmax(0, in.duration - start);
        if (in.still) seconds = seconds > 0 ? seconds : 5;
        else if (!(available > 0)) { fprintf(stderr, "%s: nothing after %.3f s\n", input_path, start); return 1; }
        else seconds = seconds > 0 ? fmin(seconds, available) : available;
    }

    /* A child that exits early must fail the write, not end this process. */
    signal(SIGPIPE, SIG_IGN);
    SDL_SetHint(SDL_HINT_MAC_BACKGROUND_APP, "1");
    if (!SDL_Init(SDL_INIT_VIDEO)) { fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError()); return 1; }
    SDL_GPUDevice *gpu = SDL_CreateGPUDevice(
        SDL_GPU_SHADERFORMAT_SPIRV | SDL_GPU_SHADERFORMAT_MSL | SDL_GPU_SHADERFORMAT_METALLIB,
        SDL_getenv("MYNES_GPU_VALIDATION") != NULL, NULL);
    if (!gpu) { fprintf(stderr, "SDL_CreateGPUDevice failed: %s\n", SDL_GetError()); return 1; }
    SDL_Window *window = SDL_CreateWindow("mynes_video", 640, 480, SDL_WINDOW_HIDDEN | SDL_WINDOW_HIGH_PIXEL_DENSITY);
    if (!window || !SDL_ClaimWindowForGPUDevice(gpu, window)) {
        fprintf(stderr, "Window failed: %s\n", SDL_GetError());
        return 1;
    }
    SDL_SetGPUSwapchainParameters(gpu, window, SDL_GPU_SWAPCHAINCOMPOSITION_SDR, SDL_GPU_PRESENTMODE_VSYNC);

    /* --- Presets: CPU side first, as mynes_gpu does --- */
    int region = SIGNAL_REGION_NTSC;
    signal_precompute_init(&sig_state, region);
    preset_ctx.sig_state = &sig_state;
    preset_ctx.video_chain = &video_chain;
    preset_ctx.audio_chain = &audio_chain;
    preset_ctx.gpu_audio_enabled = &gpu_audio_enabled;
    preset_ctx.use_gpu_audio = &use_gpu_audio;
    preset_ctx.video_gpu_chain = &video_gpu_chain;
    preset_ctx.gpu = gpu;
    preset_ctx.gpu_video_enabled = &gpu_video_enabled;
    preset_ctx.current_preset = &current_preset;
    preset_ctx.config = &mynes_config;
    preset_ctx.region = region;
    preset_ctx.display_ppu = &display_ppu;
    preset_ctx.analog_controls = &analog_controls;
    preset_ctx.render_ctx = &render_ctx;
    preset_ctx.source_dots_per_line = DOTS_PER_LINE;
    preset_ctx.encoder_source = true;
    preset_ctx_init(&preset_ctx);
    if (list) {
        for (int i = 0; i < preset_total_count(); i++) printf("%s\n", preset_display_name(i));
        return 0;
    }
    int preset_index[32];
    for (int p = 0; p < preset_count; p++) {
        int idx = -1;
        if (strchr(presets[p], '/') || strstr(presets[p], ".json")) idx = preset_register_file(presets[p]);
        if (idx < 0) idx = preset_find_by_slug(presets[p]);
        if (idx < 0) { fprintf(stderr, "No preset %s (--list-presets shows them)\n", presets[p]); return 1; }
        preset_index[p] = idx;
    }
    preset_load_index_exact(preset_index[0]);

    char shader_dir[1024], render_shader_dir[1024];
    const char *base = SDL_GetBasePath();
    snprintf(shader_dir, sizeof shader_dir, "%s../shaders/compute", base ? base : "./");
    snprintf(render_shader_dir, sizeof render_shader_dir, "%s../shaders/render", base ? base : "./");
    if (!video_gpu_init(&video_gpu_chain, gpu, &video_chain, shader_dir,
                        sig_state.fir_y, sig_state.fir_y_n, sig_state.fir_c, sig_state.fir_c_n,
                        sig_state.fir_q, sig_state.fir_q_n) || !video_gpu_chain.pipe_encoder.pipeline) {
        fprintf(stderr, "GPU video chain failed (shaders in %s)\n", shader_dir);
        return 1;
    }
    gpu_video_enabled = true;
    video_gpu_set_color_matrix(&video_gpu_chain, sig_state.color_matrix, sig_state.color_bias);
    video_gpu_upload_signal_table(&video_gpu_chain, gpu, (const float *)sig_state.table, NULL,
                                  SIG_TABLE_ENTRIES, SIG_TABLE_STRIDE);
    video_gpu_chain.signal_line_phase = LINE_PHASE;
    video_gpu_chain.demod_line_phase = LINE_PHASE * (2.0f * (float)M_PI / 12.0f);
    video_gpu_set_demod(&video_gpu_chain, 0.0f, 2.0f * (float)M_PI / 12.0f);

    SDL_GPUTextureFormat target = hdr ? SDL_GPU_TEXTUREFORMAT_R16G16B16A16_FLOAT
                                      : SDL_GetGPUSwapchainTextureFormat(gpu, window);
    if (!gpu_display_init_target(&gpu_disp, gpu, target, out_w, out_h, render_shader_dir)) {
        fprintf(stderr, "Display pipeline failed\n");
        return 1;
    }
    render_ctx.gpu = gpu;
    render_ctx.window = window;
    render_ctx.gpu_disp = &gpu_disp;
    render_ctx.gpu_display_enabled = true;
    render_ctx.crt_shader_enabled = true;
    render_ctx.hdr_enabled = hdr;
    render_ctx.hdr_gain_mode = 0;
    render_ctx.hdr_boost = 1.0f;
    render_ctx.mask_alignment = mask_alignment;
    render_ctx.panel_subpixels = 0;
    render_ctx.offscreen_w = out_w;
    render_ctx.offscreen_h = out_h;
    render_ctx.drawable_w = out_w;
    render_ctx.drawable_h = out_h;
    render_ctx.safe_area = (SDL_Rect){0, 0, out_w, out_h};
    render_ctx.offscreen_format = hdr ? target : SDL_GPU_TEXTUREFORMAT_INVALID;
    render_ctx.offscreen_headroom = recorder_offscreen_headroom(0, hdr, getenv("MYNES_OFFSCREEN_HEADROOM"));
    render_ctx.owns_display_tex = false;

    unsigned frames = recorder_frame_count(seconds, region);
    if (!frames) { fprintf(stderr, "Nothing to record\n"); return 1; }
    uint16_t *rgb48 = malloc((size_t)SRC_W * SRC_H * 3 * sizeof(uint16_t));
    uint32_t *codes = malloc((size_t)SRC_W * SRC_H * sizeof(uint32_t));
    float *silence = calloc(1024, sizeof(float));
    if (!rgb48 || !codes || !silence) return 1;
    int status = 0;

    for (int p = 0; p < preset_count && !status; p++) {
        if (p > 0) preset_load_index(preset_index[p]);
        if (video_chain.signal_fmt.region != SIGNAL_REGION_NTSC) {
            fprintf(stderr, "%s is a PAL preset; the tool runs the NTSC chain only\n", presets[p]);
            status = 1; break;
        }
        preset_apply_gpu_push(&preset_ctx);
        if (!setup_beam(gpu, out_w, out_h)) { fprintf(stderr, "Beam target failed\n"); status = 1; break; }
        video_gpu_reset_temporal_state(&video_gpu_chain, gpu);
        render_ctx.white_dirty = true;

        char out_path[2048];
        output_for(output, presets[p], preset_count, out_path, sizeof out_path);
        const char *dot = strrchr(out_path, '.');
        /* The display pass writes sRGB-encoded BT.709 primaries. ProRes
         * takes its colour fields from the frames, so the tags are set on
         * the frames as well as on the stream. */
        const char *codec = hdr ? NULL : !strcasecmp(dot, ".mov")
            ? "-vf scale=out_color_matrix=bt709:out_range=tv,"
              "setparams=colorspace=bt709:color_primaries=bt709:color_trc=iec61966-2-1:range=tv "
              "-c:v prores_ks -profile:v 4 -pix_fmt yuv444p10le -vendor apl0 "
              "-colorspace bt709 -color_primaries bt709 -color_trc iec61966-2-1 -color_range tv"
            : "-vf scale=out_color_matrix=bt709:out_range=tv,format=yuv420p,"
              "setparams=colorspace=bt709:color_primaries=bt709:color_trc=iec61966-2-1:range=tv "
              "-c:v libx264 -preset slow -crf 16 "
              "-colorspace bt709 -color_primaries bt709 -color_trc iec61966-2-1 -color_range tv";
        RecorderOptions options = {.output = out_path, .seconds = frames / recorder_rate(region), .after = 0,
                                   .region = region, .width = out_w, .height = out_h, .ffmpeg = ffmpeg,
                                   .codec_args = codec, .headroom = gpu_render_headroom(&render_ctx), .hdr = hdr};
        char error[2048];
        Recorder *recorder = recorder_create(&options, error, sizeof error);
        if (!recorder) { fprintf(stderr, "Recording: %s\n", error); status = 1; break; }
        pid_t decoder; FILE *decoded;
        if (!start_decoder(&in, ffmpeg, start, seconds, fit, &decoder, &decoded)) {
            fprintf(stderr, "Cannot start %s: %s\n", ffmpeg, strerror(errno));
            recorder_destroy(recorder); status = 1; break;
        }
        fprintf(stderr, "%s through %s: %u fields at %s Hz, %dx%d %s -> %s\n", input_path,
                preset_display_name(preset_active_index()), frames, recorder_rate_string(region),
                out_w, out_h, hdr ? "HDR" : "SDR", out_path);
        float brightness = 0;
        int phase = 0;
        unsigned field = 0, written = 0;
        bool have = read_frame(decoded, rgb48, codes, &brightness);
        if (!have) { fprintf(stderr, "%s: no picture decoded\n", input_path); status = 1; }
        /* The receiver's AGC, line and colour loops and the tube's
         * persistence settle on the first picture before recording. */
        for (int s = 0; !status && s < settle; s++)
            if (!render_field(gpu, field++, &phase, codes, brightness, &enc, NULL)) status = 1;
        double audio_due = 0;
        while (!status && written < frames) {
            if (!render_field(gpu, field++, &phase, codes, brightness, &enc, recorder)) { status = 1; break; }
            written++;
            /* The recorder muxes a sound track of the clip's length; the
             * input's own sound replaces it afterwards. */
            audio_due += 44100.0 / recorder_rate(region);
            size_t samples = (size_t)audio_due;
            audio_due -= (double)samples;
            while (samples) {
                size_t chunk = samples < 1024 ? samples : 1024;
                fwrite(silence, sizeof(float), chunk, recorder_audio_file(recorder));
                samples -= chunk;
            }
            if (written % 60 == 0 && written < frames) fprintf(stderr, "\r%u / %u fields", written, frames);
            /* A video that ends early keeps its last picture. */
            if (written < frames && !read_frame(decoded, rgb48, codes, &brightness)) {}
        }
        fprintf(stderr, "\r%u / %u fields\n", written, frames);
        fclose(decoded);
        kill(decoder, SIGTERM);
        wait_child(decoder);
        if (!status && !gpu_render_release_pending(&render_ctx)) status = 1;
        if (!status && !recorder_finish(recorder, error, sizeof error)) {
            fprintf(stderr, "Recording failed: %s\n", error);
            status = 1;
        }
        recorder_destroy(recorder);
        /* The input's sound, from the same start, in place of the silent track. */
        if (!status && in.has_audio && !no_audio && !in.still) {
            char tmp[2100], ss[32], dur[32];
            snprintf(tmp, sizeof tmp, "%.*s.audio%s", (int)(dot - out_path), out_path, dot);
            snprintf(ss, sizeof ss, "%.6f", start);
            snprintf(dur, sizeof dur, "%.6f", frames / recorder_rate(region));
            char *mux[] = {(char *)ffmpeg, "-y", "-nostdin", "-v", "error", "-i", out_path,
                           "-ss", ss, "-i", (char *)input_path, "-map", "0:v:0", "-map", "1:a:0",
                           "-c:v", "copy", "-c:a", "aac", "-b:a", "256k", "-t", dur,
                           "-movflags", "+faststart", tmp, NULL};
            if (run_quiet(mux) && rename(tmp, out_path) == 0) fprintf(stderr, "Sound: from %s\n", input_path);
            else { remove(tmp); fprintf(stderr, "Sound: could not carry it over; the clip is silent\n"); }
        }
        if (!status) fprintf(stderr, "Wrote %s\n", out_path);
    }

    free(rgb48); free(codes); free(silence);
    if (still_png[0]) remove(still_png);
    gpu_display_destroy(&gpu_disp, gpu);
    video_gpu_destroy(&video_gpu_chain, gpu);
    SDL_ReleaseWindowFromGPUDevice(gpu, window);
    SDL_DestroyWindow(window);
    SDL_DestroyGPUDevice(gpu);
    SDL_Quit();
    return status;
}
