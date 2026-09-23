/*
 * mynes_retro -- an RGB console through the MyNES signal, receiver and CRT chain
 * ==============================================================================
 *
 * Research prototype. Loads a libretro core (Genesis Plus GX for the Mega
 * Drive, Snes9x for the Super Famicom), recovers the video chip's colour
 * codes from the core's RGB565 frame, and feeds them to the GPU chain's
 * encoder source (encoder_rgb.comp.glsl) instead of the 2C02 DAC. From the
 * raster on, the chain is the NES one: console output pole, cable, RF,
 * receiver PLL, Y/C separation, decoder, amplifiers, tube.
 *
 * The recovery is exact, not a colour-space guess: both cores expand the
 * chip's codes with injective shifts, so the codes come back by shifting
 * down, and the console profile then maps codes to measured DAC voltages
 * (the Mega Drive's 15-level ramp) or to the chip's linear ramp (S-PPU2,
 * 5 bits per gun).
 *
 * Geometry per console (12 samples per subcarrier cycle):
 *   Mega Drive  342 dots of 8 samples per line (3420 master clocks, 228
 *               cycles, no line-to-line phase change), 320 pixels of 6.4
 *               samples in H40 or 256 of 8 in H32, 224 lines.
 *   Super Famicom  341 dots per line (1364 master clocks, 227.33 cycles,
 *               4 slots per line like the 2C02), 256 pixels of 8 samples
 *               or 512 of 4, 224 or 239 lines from line 1.
 *
 * Usage:
 *   mynes_retro --core core.dylib --console md|snes [--preset name]
 *               [--offscreen WxH --screenshot-after N --screenshot out.ppm]
 *               [--frames N] [--sdr] rom
 */

#include <SDL3/SDL.h>
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdarg.h>

#include "libretro.h"
#include "signal_format.h"
#include "signal_precompute.h"
#include "video_chain.h"
#include "video_gpu.h"
#include "audio_chain.h"
#include "gpu_render.h"
#include "gpu_display.h"
#include "gpu_output.h"
#include "preset_apply.h"
#include "gpu_osd.h"
#include "config.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* ============================================================================
 * Console profiles
 * ============================================================================ */

typedef struct {
    const char *name;
    int dots_per_line;      /* complete line in dots of samples_per_pixel */
    int code_bits;          /* bits per gun the core's RGB565 carries */
    int line_phase;         /* carrier slots per line */
    int frame_phase_adv[2]; /* carrier slots per frame, by frame parity */
    int top_line;           /* raster line of the console's first picture line */
    float ramp[64];
    int ramp_n;
    float setup;            /* encoder black pedestal */
    float chroma_bw_hz;     /* encoder chroma band, baseband */
    float luma_bw_hz;       /* encoder luma band */
    float luma_trap;        /* luma trap depth at the subcarrier, 0 = not fitted */
} ConsoleProfile;

/* The encoder's live controls, in the units the setup menu shows. Seeded
 * from the console profile; the M menu's Encoder submenu edits them. */
static float enc_luma_mhz, enc_chroma_mhz, enc_trap, enc_setup;
static const OSDMenuItem encoder_menu[] = {
    {"Luma band (MHz)", OSD_MI_FLOAT, &enc_luma_mhz, 0.25f, 1.0f, 12.0f, NULL, NULL, 0, NULL, "%.2f"},
    {"Chroma band (MHz)", OSD_MI_FLOAT, &enc_chroma_mhz, 0.1f, 0.3f, 3.0f, NULL, NULL, 0, NULL, "%.2f"},
    {"Luma trap depth", OSD_MI_FLOAT, &enc_trap, 0.05f, 0.0f, 1.0f, NULL, NULL, 0, NULL, "%.2f"},
    {"Setup (IRE/100)", OSD_MI_FLOAT, &enc_setup, 0.005f, 0.0f, 0.1f, NULL, NULL, 0, NULL, "%.3f"},
};

static ConsoleProfile profile_md, profile_snes;

static void profiles_init(void) {
    /* Mega Drive VDP colour DAC, measured 15-level ramp (plutiedev.com,
     * "VDP color ramp"): normal colours use every other step, shadow the
     * first eight, highlight the last eight. Genesis Plus GX writes that
     * step index into each channel, so the code is the ramp position. */
    static const float md_ramp[15] = {0, 29, 52, 70, 87, 101, 116, 130, 144,
                                      158, 172, 187, 206, 228, 255};
    profile_md.name = "Mega Drive";
    profile_md.dots_per_line = 342;
    profile_md.code_bits = 4;
    profile_md.line_phase = 0;          /* 2736 samples per line = 228 cycles */
    profile_md.frame_phase_adv[0] = 0;  /* 262 lines of 228 cycles: integer */
    profile_md.frame_phase_adv[1] = 0;
    profile_md.top_line = 11;           /* NTSC V28: 11 top border lines */
    for (int i = 0; i < 15; i++) profile_md.ramp[i] = md_ramp[i] / 255.0f;
    profile_md.ramp_n = 15;
    profile_md.setup = 0.0f;
    profile_md.chroma_bw_hz = 1.3e6f;   /* generic encoder chroma band, not a measured CXA1145 filter */
    profile_md.luma_bw_hz = 5.0e6f;     /* CXA1645 Y output, -3 dB at 5 MHz (datasheet); CXA1145 unmeasured */
    profile_md.luma_trap = 0.0f;        /* YTRAP fitted per board revision: unverified */

    /* Super Famicom S-PPU2: five bits per gun. A linear ramp; the measured
     * DAC curve and the 2-chip consoles' slow falling edges are not modelled. */
    profile_snes.name = "Super Famicom";
    profile_snes.dots_per_line = 341;
    profile_snes.code_bits = 5;
    profile_snes.line_phase = 4;        /* 2728 samples per line, 227.33 cycles */
    profile_snes.frame_phase_adv[0] = 4;   /* 262 lines of 1364 clocks */
    profile_snes.frame_phase_adv[1] = 8;   /* one line of 1360 clocks every other frame */
    profile_snes.top_line = 1;
    for (int i = 0; i < 32; i++) profile_snes.ramp[i] = (float)i / 31.0f;
    profile_snes.ramp_n = 32;
    profile_snes.setup = 0.0f;
    profile_snes.chroma_bw_hz = 1.3e6f;
    profile_snes.luma_bw_hz = 5.0e6f;   /* encoder figure; the S-PPU2 DAC's slow edges are not modelled */
    profile_snes.luma_trap = 0.0f;
}

/* ============================================================================
 * libretro core
 * ============================================================================ */

typedef struct {
    void *handle;
    void (*set_environment)(retro_environment_t);
    void (*set_video_refresh)(retro_video_refresh_t);
    void (*set_audio_sample)(retro_audio_sample_t);
    void (*set_audio_sample_batch)(retro_audio_sample_batch_t);
    void (*set_input_poll)(retro_input_poll_t);
    void (*set_input_state)(retro_input_state_t);
    void (*init)(void);
    void (*deinit)(void);
    unsigned (*api_version)(void);
    void (*get_system_info)(struct retro_system_info *);
    void (*get_system_av_info)(struct retro_system_av_info *);
    bool (*load_game)(const struct retro_game_info *);
    void (*unload_game)(void);
    void (*run)(void);
} Core;

static Core core;
static enum retro_pixel_format core_pixel_format = RETRO_PIXEL_FORMAT_0RGB1555;
static char system_dir[1024];
static SDL_AudioStream *audio_stream;

/* Latest frame from the core, converted to gun codes. */
static uint32_t codes[1024 * 240];
static int frame_width, frame_height;
static bool frame_ready;
static const ConsoleProfile *profile;

static void core_log(enum retro_log_level level, const char *fmt, ...) {
    if (level < RETRO_LOG_INFO) return;
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
}

static bool env_cb(unsigned cmd, void *data) {
    switch (cmd) {
    case RETRO_ENVIRONMENT_SET_PIXEL_FORMAT: {
        enum retro_pixel_format f = *(const enum retro_pixel_format *)data;
        if (f != RETRO_PIXEL_FORMAT_RGB565 && f != RETRO_PIXEL_FORMAT_XRGB8888) return false;
        core_pixel_format = f;
        return true;
    }
    case RETRO_ENVIRONMENT_GET_LOG_INTERFACE:
        ((struct retro_log_callback *)data)->log = core_log;
        return true;
    case RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY:
    case RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY:
    case RETRO_ENVIRONMENT_GET_CORE_ASSETS_DIRECTORY:
        *(const char **)data = system_dir;
        return true;
    case RETRO_ENVIRONMENT_GET_CAN_DUPE:
        *(bool *)data = true;
        return true;
    case RETRO_ENVIRONMENT_GET_CORE_OPTIONS_VERSION:
        *(unsigned *)data = 2;
        return true;
    case RETRO_ENVIRONMENT_SET_VARIABLES:
    case RETRO_ENVIRONMENT_SET_CORE_OPTIONS:
    case RETRO_ENVIRONMENT_SET_CORE_OPTIONS_INTL:
    case RETRO_ENVIRONMENT_SET_CORE_OPTIONS_V2:
    case RETRO_ENVIRONMENT_SET_CORE_OPTIONS_V2_INTL:
    case RETRO_ENVIRONMENT_SET_CORE_OPTIONS_DISPLAY:
    case RETRO_ENVIRONMENT_SET_INPUT_DESCRIPTORS:
    case RETRO_ENVIRONMENT_SET_CONTROLLER_INFO:
    case RETRO_ENVIRONMENT_SET_MEMORY_MAPS:
    case RETRO_ENVIRONMENT_SET_GEOMETRY:
    case RETRO_ENVIRONMENT_SET_SYSTEM_AV_INFO:
    case RETRO_ENVIRONMENT_SET_SUPPORT_NO_GAME:
    case RETRO_ENVIRONMENT_SET_SUBSYSTEM_INFO:
    case RETRO_ENVIRONMENT_SET_SUPPORT_ACHIEVEMENTS:
    case RETRO_ENVIRONMENT_SET_SERIALIZATION_QUIRKS:
    case RETRO_ENVIRONMENT_SET_MINIMUM_AUDIO_LATENCY:
        return true;
    case RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE:
        *(bool *)data = false;
        return true;
    default:
        return false; /* core options fall back to their defaults */
    }
}

/* Recover the video chip's codes from the core's pixels. RGB565 from both
 * cores is an injective expansion of the codes, so shifting down is exact. */
static void video_cb(const void *data, unsigned width, unsigned height, size_t pitch) {
    if (!data || width == 0 || width > 1024) return;
    frame_width = (int)width;
    /* Interlaced output (448/478 lines) is taken as its first field. */
    int step = height > 240 ? 2 : 1;
    frame_height = (int)height / step;
    if (frame_height > 240) frame_height = 240;
    int bits = profile->code_bits;
    for (int y = 0; y < frame_height; y++) {
        const uint8_t *row = (const uint8_t *)data + (size_t)y * step * pitch;
        uint32_t *out = codes + (size_t)y * width;
        if (core_pixel_format == RETRO_PIXEL_FORMAT_RGB565) {
            const uint16_t *px = (const uint16_t *)row;
            for (unsigned x = 0; x < width; x++) {
                uint16_t p = px[x];
                uint32_t r = (p >> 11) >> (5 - bits);
                uint32_t g = ((p >> 5) & 63) >> (6 - bits);
                uint32_t b = (p & 31) >> (5 - bits);
                out[x] = r | (g << 6) | (b << 12);
            }
        } else {
            const uint32_t *px = (const uint32_t *)row;
            for (unsigned x = 0; x < width; x++) {
                uint32_t p = px[x];
                uint32_t r = ((p >> 16) & 255) >> (8 - bits);
                uint32_t g = ((p >> 8) & 255) >> (8 - bits);
                uint32_t b = (p & 255) >> (8 - bits);
                out[x] = r | (g << 6) | (b << 12);
            }
        }
    }
    frame_ready = true;
}

static void audio_sample_cb(int16_t left, int16_t right) {
    int16_t pair[2] = {left, right};
    if (audio_stream) SDL_PutAudioStreamData(audio_stream, pair, sizeof(pair));
}

static size_t audio_batch_cb(const int16_t *data, size_t frames) {
    if (audio_stream) SDL_PutAudioStreamData(audio_stream, data, (int)(frames * 2 * sizeof(int16_t)));
    return frames;
}

static void input_poll_cb(void) {}

static int16_t input_state_cb(unsigned port, unsigned device, unsigned index, unsigned id) {
    (void)index;
    if (port != 0 || device != RETRO_DEVICE_JOYPAD || osd_menu_is_open) return 0;
    const bool *k = SDL_GetKeyboardState(NULL);
    switch (id) {
    case RETRO_DEVICE_ID_JOYPAD_UP:     return k[SDL_SCANCODE_UP];
    case RETRO_DEVICE_ID_JOYPAD_DOWN:   return k[SDL_SCANCODE_DOWN];
    case RETRO_DEVICE_ID_JOYPAD_LEFT:   return k[SDL_SCANCODE_LEFT];
    case RETRO_DEVICE_ID_JOYPAD_RIGHT:  return k[SDL_SCANCODE_RIGHT];
    case RETRO_DEVICE_ID_JOYPAD_START:  return k[SDL_SCANCODE_RETURN];
    case RETRO_DEVICE_ID_JOYPAD_SELECT: return k[SDL_SCANCODE_RSHIFT];
    case RETRO_DEVICE_ID_JOYPAD_Y:      return k[SDL_SCANCODE_Z]; /* MD A, SFC Y */
    case RETRO_DEVICE_ID_JOYPAD_B:      return k[SDL_SCANCODE_X]; /* MD B, SFC B */
    case RETRO_DEVICE_ID_JOYPAD_A:      return k[SDL_SCANCODE_C]; /* MD C, SFC A */
    case RETRO_DEVICE_ID_JOYPAD_X:      return k[SDL_SCANCODE_S]; /* MD Y, SFC X */
    case RETRO_DEVICE_ID_JOYPAD_L:      return k[SDL_SCANCODE_A]; /* MD X, SFC L */
    case RETRO_DEVICE_ID_JOYPAD_R:      return k[SDL_SCANCODE_D]; /* MD Z, SFC R */
    default: return 0;
    }
}

#define LOAD_SYM(field, name) do { \
    *(void **)&core.field = dlsym(core.handle, name); \
    if (!core.field) { fprintf(stderr, "%s: missing %s\n", path, name); return false; } } while (0)

static bool core_load(const char *path) {
    core.handle = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    if (!core.handle) { fprintf(stderr, "dlopen %s: %s\n", path, dlerror()); return false; }
    LOAD_SYM(set_environment, "retro_set_environment");
    LOAD_SYM(set_video_refresh, "retro_set_video_refresh");
    LOAD_SYM(set_audio_sample, "retro_set_audio_sample");
    LOAD_SYM(set_audio_sample_batch, "retro_set_audio_sample_batch");
    LOAD_SYM(set_input_poll, "retro_set_input_poll");
    LOAD_SYM(set_input_state, "retro_set_input_state");
    LOAD_SYM(init, "retro_init");
    LOAD_SYM(deinit, "retro_deinit");
    LOAD_SYM(api_version, "retro_api_version");
    LOAD_SYM(get_system_info, "retro_get_system_info");
    LOAD_SYM(get_system_av_info, "retro_get_system_av_info");
    LOAD_SYM(load_game, "retro_load_game");
    LOAD_SYM(unload_game, "retro_unload_game");
    LOAD_SYM(run, "retro_run");
    if (core.api_version() != RETRO_API_VERSION) {
        fprintf(stderr, "%s: libretro API %u, expected %u\n", path, core.api_version(), RETRO_API_VERSION);
        return false;
    }
    return true;
}

static bool core_start(const char *rom_path) {
    core.set_environment(env_cb);
    core.set_video_refresh(video_cb);
    core.set_audio_sample(audio_sample_cb);
    core.set_audio_sample_batch(audio_batch_cb);
    core.set_input_poll(input_poll_cb);
    core.set_input_state(input_state_cb);
    core.init();
    struct retro_system_info info = {0};
    core.get_system_info(&info);
    fprintf(stderr, "Core: %s %s\n", info.library_name, info.library_version);
    struct retro_game_info game = {.path = rom_path};
    void *rom = NULL;
    FILE *f = fopen(rom_path, "rb");
    if (!f) { perror(rom_path); return false; }
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    rom = malloc((size_t)size);
    if (!rom || fread(rom, 1, (size_t)size, f) != (size_t)size) { fclose(f); free(rom); return false; }
    fclose(f);
    game.data = rom;
    game.size = (size_t)size;
    bool ok = core.load_game(&game);
    if (!ok) fprintf(stderr, "Core refused %s\n", rom_path);
    /* Cores that need the full path have read it themselves. */
    free(rom);
    return ok;
}

/* ============================================================================
 * Frontend
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
static bool osd_parameter_editing;
static uint32_t osd_pixels[GPU_OSD_PIXELS];

/* Fit the tube aspect into the drawable, as mynes_gpu does: two device rows
 * per scanline is the floor, the drawable fit the ceiling. */
static void beam_target_size(int win_w, int win_h, int *out_w, int *out_h) {
    int aw = video_chain.tv.monitor_model == 1 ? 16 : 4, ah = video_chain.tv.monitor_model == 1 ? 10 : 3;
    int w = win_w, h = win_h;
    if (w * ah > h * aw) w = h * aw / ah; else h = w * ah / aw;
    int fit_h = h;
    if (h < 480) h = 480;
    *out_w = h == fit_h ? w : h * aw / ah; *out_h = h;
}

/* Drawable pixels the tube face is fitted into, clear of a camera housing. */
static void picture_area_size(SDL_Window *win, int *w, int *h) {
    SDL_GetWindowSizeInPixels(win, w, h);
    SDL_Rect safe = gpu_output_safe_area(win, *w, *h);
    *w = safe.w;
    *h = safe.h;
}

static void resource_dir(char *out, size_t size, const char *base, const char *leaf) {
    /* The build tree keeps shaders beside bin/; the same layout as mynes_gpu. */
    snprintf(out, size, "%s../%s", base, leaf);
}

static int gcd(int a, int b) { while (b) { int t = a % b; a = b; b = t; } return a; }

/* Auto gain fits the preset's own white, as mynes_gpu does: a full-code
 * white field through the encoder and the chain until its loops settle,
 * measured through the display pass, then the chain's temporal state is
 * cleared so the white does not linger. */
static bool calibrate_white(SDL_GPUDevice *gpu, const VideoRGBSource *picture) {
    static uint32_t white[320 * 224];
    uint32_t full = (uint32_t)(profile->ramp_n - 1);
    for (int i = 0; i < 320 * 224; i++) white[i] = full | (full << 6) | (full << 12);
    VideoRGBSource src = *picture;
    src.pixels = white; src.width = 320; src.lines = 224;
    src.spp_num = SIGNAL_NTSC_SAMPLES_PER_LINE / gcd(SIGNAL_NTSC_SAMPLES_PER_LINE, 320);
    src.spp_den = 320 / gcd(SIGNAL_NTSC_SAMPLES_PER_LINE, 320);
    for (int i = 0; i < 24; i++)
        if (!video_gpu_process_rgb(&video_gpu_chain, gpu, &src, NULL)) return false;
    bool measured = false;
    int beam_w, beam_h;
    SDL_GPUTexture *beam = video_gpu_get_beam_texture(&video_gpu_chain);
    if (beam && video_gpu_get_beam_size(&video_gpu_chain, &beam_w, &beam_h)) {
        render_ctx.display_tex = beam; render_ctx.display_tex_w = beam_w; render_ctx.display_tex_h = beam_h;
        render_ctx.owns_display_tex = false;
        measured = gpu_render_measure_white(&render_ctx, &video_chain);
        if (measured)
            fprintf(stderr, "White: peak %.2f, average %.2f at gain 1; Auto gain %.2f\n", render_ctx.white_peak_measured,
                    render_ctx.white_mean_measured, 0.95f * gpu_render_headroom(&render_ctx) / render_ctx.white_peak_measured);
    }
    video_gpu_reset_temporal_state(&video_gpu_chain, gpu);
    return measured;
}

static void usage(void) {
    fprintf(stderr, "usage: mynes_retro --core core.dylib --console md|snes [--preset name] "
                    "[--offscreen WxH --screenshot-after N --screenshot out.ppm] [--frames N] [--sdr] rom\n");
}

int main(int argc, char **argv) {
    const char *core_path = NULL, *console = NULL, *preset_path = NULL, *rom_path = NULL;
    const char *screenshot_path = "/tmp/mynes_retro.ppm";
    int offscreen_w = 0, offscreen_h = 0, screenshot_after = 0;
    unsigned frame_limit = 0;
    bool force_sdr = false, test_bars = false, calibrate = false, test_strokes = false;
    float opt_luma_mhz = -1, opt_chroma_mhz = -1, opt_trap = -1;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--core") && i + 1 < argc) core_path = argv[++i];
        else if (!strcmp(argv[i], "--console") && i + 1 < argc) console = argv[++i];
        else if (!strcmp(argv[i], "--preset") && i + 1 < argc) preset_path = argv[++i];
        else if (!strcmp(argv[i], "--offscreen") && i + 1 < argc) {
            if (sscanf(argv[++i], "%dx%d", &offscreen_w, &offscreen_h) != 2) { usage(); return 1; }
        } else if (!strcmp(argv[i], "--screenshot-after") && i + 1 < argc) screenshot_after = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--screenshot") && i + 1 < argc) screenshot_path = argv[++i];
        else if (!strcmp(argv[i], "--frames") && i + 1 < argc) frame_limit = (unsigned)atoi(argv[++i]);
        else if (!strcmp(argv[i], "--sdr")) force_sdr = true;
        else if (!strcmp(argv[i], "--test-bars")) test_bars = true;
        else if (!strcmp(argv[i], "--calibrate")) { test_bars = true; calibrate = true; }
        else if (!strcmp(argv[i], "--cross-colour")) { test_strokes = true; calibrate = true; }
        else if (!strcmp(argv[i], "--luma-bw") && i + 1 < argc) opt_luma_mhz = (float)atof(argv[++i]);
        else if (!strcmp(argv[i], "--chroma-bw") && i + 1 < argc) opt_chroma_mhz = (float)atof(argv[++i]);
        else if (!strcmp(argv[i], "--trap") && i + 1 < argc) opt_trap = (float)atof(argv[++i]);
        else if (argv[i][0] == '-') { usage(); return 1; }
        else rom_path = argv[i];
    }
    if (!core_path || !console || !rom_path) { usage(); return 1; }
    profiles_init();
    if (!strcmp(console, "md")) profile = &profile_md;
    else if (!strcmp(console, "snes")) profile = &profile_snes;
    else { usage(); return 1; }

    snprintf(system_dir, sizeof(system_dir), "%s", getenv("TMPDIR") ? getenv("TMPDIR") : "/tmp");
    if (!core_load(core_path)) return 1;

    if (offscreen_w) SDL_SetHint(SDL_HINT_MAC_BACKGROUND_APP, "1");
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO)) {
        fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
        return 1;
    }
    SDL_GPUDevice *gpu = SDL_CreateGPUDevice(
        SDL_GPU_SHADERFORMAT_SPIRV | SDL_GPU_SHADERFORMAT_MSL | SDL_GPU_SHADERFORMAT_METALLIB,
        SDL_getenv("MYNES_GPU_VALIDATION") != NULL, NULL);
    if (!gpu) { fprintf(stderr, "SDL_CreateGPUDevice failed: %s\n", SDL_GetError()); return 1; }
    SDL_WindowFlags flags = SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY;
    if (offscreen_w) flags |= SDL_WINDOW_HIDDEN;
    SDL_Window *window = SDL_CreateWindow("MyNES retro", 1024, 768, flags);
    /* Nothing is clicked: the pointer stays hidden over the picture, as in mynes_gpu. */
    SDL_HideCursor();
    if (!window || !SDL_ClaimWindowForGPUDevice(gpu, window)) {
        fprintf(stderr, "Window failed: %s\n", SDL_GetError());
        return 1;
    }
    bool hdr_available = !force_sdr && SDL_WindowSupportsGPUSwapchainComposition(gpu, window,
        SDL_GPU_SWAPCHAINCOMPOSITION_HDR_EXTENDED_LINEAR);
    if (hdr_available)
        hdr_available = SDL_SetGPUSwapchainParameters(gpu, window,
            SDL_GPU_SWAPCHAINCOMPOSITION_HDR_EXTENDED_LINEAR, SDL_GPU_PRESENTMODE_VSYNC);
    if (!hdr_available)
        SDL_SetGPUSwapchainParameters(gpu, window, SDL_GPU_SWAPCHAINCOMPOSITION_SDR, SDL_GPU_PRESENTMODE_VSYNC);

    /* --- Core --- */
    if (!core_start(rom_path)) return 1;
    struct retro_system_av_info av = {0};
    core.get_system_av_info(&av);
    fprintf(stderr, "Core video: %ux%u (max %ux%u) %.3f Hz, audio %.0f Hz\n",
            av.geometry.base_width, av.geometry.base_height, av.geometry.max_width,
            av.geometry.max_height, av.timing.fps, av.timing.sample_rate);
    if (!offscreen_w) {
        SDL_AudioSpec spec = {.format = SDL_AUDIO_S16, .channels = 2, .freq = (int)av.timing.sample_rate};
        audio_stream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, NULL, NULL);
        if (audio_stream) SDL_ResumeAudioStreamDevice(audio_stream);
    }

    /* --- Preset (CPU side first, like mynes_gpu) --- */
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
    preset_ctx.source_dots_per_line = profile->dots_per_line;
    preset_ctx.encoder_source = true;
    preset_ctx_init(&preset_ctx);
    enc_luma_mhz = profile->luma_bw_hz / 1e6f;
    enc_chroma_mhz = profile->chroma_bw_hz / 1e6f;
    enc_trap = profile->luma_trap;
    enc_setup = profile->setup;
    if (opt_luma_mhz >= 0) enc_luma_mhz = opt_luma_mhz;
    if (opt_chroma_mhz >= 0) enc_chroma_mhz = opt_chroma_mhz;
    if (opt_trap >= 0) enc_trap = opt_trap;
    {
        OSDMenuItem sub = {"Encoder", OSD_MI_SUBMENU, NULL, 0, 0, 0, NULL, encoder_menu,
                           (int)(sizeof(encoder_menu) / sizeof(encoder_menu[0])), NULL, NULL};
        if (!preset_menu_root_append(sub)) fprintf(stderr, "OSD root menu is full; no Encoder submenu\n");
    }
    int preset_idx = -1;
    if (preset_path) {
        preset_idx = preset_register_file(preset_path);
        if (preset_idx < 0 && !strchr(preset_path, '/') && !strstr(preset_path, ".json"))
            preset_idx = preset_find_by_slug(preset_path);
        if (preset_idx < 0) { fprintf(stderr, "Could not load preset: %s\n", preset_path); return 1; }
    }
    if (preset_idx < 0) preset_idx = preset_find_by_slug("reference_composite");
    if (preset_idx < 0 && preset_total_count() > 0) preset_idx = 0;
    if (preset_idx >= 0) preset_load_index_exact(preset_idx);
    if (video_chain.signal_fmt.region != SIGNAL_REGION_NTSC) {
        fprintf(stderr, "PAL presets are not supported by this prototype\n");
        return 1;
    }
    /* preset_apply keeps the console's line length and drops the 2C02
     * output-impedance model on every load; the first load happened above. */

    char shader_dir[1024], render_shader_dir[1024];
    const char *base = SDL_GetBasePath();
    resource_dir(shader_dir, sizeof(shader_dir), base ? base : "./", "shaders/compute");
    resource_dir(render_shader_dir, sizeof(render_shader_dir), base ? base : "./", "shaders/render");

    if (!video_gpu_init(&video_gpu_chain, gpu, &video_chain, shader_dir,
                        sig_state.fir_y, sig_state.fir_y_n, sig_state.fir_c, sig_state.fir_c_n,
                        sig_state.fir_q, sig_state.fir_q_n)) {
        fprintf(stderr, "GPU video chain failed\n");
        return 1;
    }
    if (!video_gpu_chain.pipe_encoder.pipeline) {
        fprintf(stderr, "encoder_rgb.comp.spv missing from %s\n", shader_dir);
        return 1;
    }
    gpu_video_enabled = true;
    video_gpu_set_color_matrix(&video_gpu_chain, sig_state.color_matrix, sig_state.color_bias);
    video_gpu_upload_signal_table(&video_gpu_chain, gpu, (const float *)sig_state.table, NULL,
                                  SIG_TABLE_ENTRIES, SIG_TABLE_STRIDE);
    {
        float fsc = signal_region_subcarrier_hz(region), fs = signal_region_sample_rate_hz(region);
        video_gpu_chain.signal_line_phase = profile->line_phase;
        video_gpu_chain.demod_line_phase = profile->line_phase * (2.0f * (float)M_PI / 12.0f);
        video_gpu_set_demod(&video_gpu_chain, 0.0f, 2.0f * (float)M_PI * fsc / fs);
    }
    {
        int win_w, win_h;
        picture_area_size(window, &win_w, &win_h);
        if (offscreen_w) { win_w = offscreen_w; win_h = offscreen_h; }
        int beam_w, beam_h;
        beam_target_size(win_w, win_h, &beam_w, &beam_h);
        int rps = beam_h / 240; if (rps < 1) rps = 1;
        if (!video_gpu_set_beam_params(&video_gpu_chain, gpu, beam_w, beam_h, rps, 0.20f, 0.70f)) {
            fprintf(stderr, "Beam target failed\n");
            return 1;
        }
        const TVDisplayParams *tv = &video_chain.tv;
        video_gpu_chain.beam_sigma_narrow = video_beam_sigma(tv, false);
        video_gpu_chain.beam_sigma_wide = video_beam_sigma(tv, true);
        video_gpu_chain.beam_h_blur_sigma = tv->beam_spot_size > 0 ? tv->beam_spot_size : 6.0f;
    }
    bool display_ready = offscreen_w
        ? gpu_display_init_target(&gpu_disp, gpu, SDL_GetGPUSwapchainTextureFormat(gpu, window),
                                  offscreen_w, offscreen_h, render_shader_dir)
        : gpu_display_init(&gpu_disp, gpu, window, render_shader_dir);
    if (!display_ready) { fprintf(stderr, "Display pipeline failed\n"); return 1; }
    render_ctx.gpu = gpu;
    render_ctx.window = window;
    render_ctx.gpu_disp = &gpu_disp;
    render_ctx.gpu_display_enabled = true;
    render_ctx.crt_shader_enabled = true;
    render_ctx.hdr_enabled = hdr_available;
    render_ctx.hdr_gain_mode = 0; /* Auto: fit the preset's white, measured through the encoder */
    render_ctx.hdr_boost = 1.0f;
    render_ctx.offscreen_w = offscreen_w;
    render_ctx.offscreen_h = offscreen_h;
    render_ctx.offscreen_format = SDL_GPU_TEXTUREFORMAT_INVALID;
    render_ctx.offscreen_headroom = 1.6f;
    render_ctx.owns_display_tex = false;
    preset_apply_gpu_push(&preset_ctx);
    fprintf(stderr, "%s through preset %s, %s\n", profile->name,
            preset_display_name(preset_active_index()), hdr_available ? "HDR" : "SDR");

    /* --- Frames --- */
    bool running = true;
    unsigned frame = 0;
    int phase = 0;
    Uint64 deadline = 0;
    Uint64 period_ns = (Uint64)(1e9 / (av.timing.fps > 1 ? av.timing.fps : 60.0));
    while (running) {
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            if (ev.type == SDL_EVENT_QUIT) running = false;
            if (ev.type == SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED && !offscreen_w)
                gpu_display_resize(&gpu_disp, gpu, ev.window.data1, ev.window.data2);
            if (ev.type != SDL_EVENT_KEY_DOWN) continue;
            /* The setup menu (M) takes every key while it is open. */
            if (gpu_osd_handle_key(ev.key.scancode, &osd_parameter_editing)) continue;
            if (ev.key.repeat) continue;
            bool alt = (ev.key.mod & SDL_KMOD_ALT) != 0;
            if (ev.key.scancode == SDL_SCANCODE_M) {
                osd_parameter_editing = false;
                osd_menu_open_root(preset_menu_root, preset_menu_root_count, "SETUP");
            }
            if (ev.key.scancode == SDL_SCANCODE_ESCAPE) running = false;
            /* F, F11 or Alt+Return: fullscreen, native when the mask is fitted to panel pixels. */
            if (ev.key.scancode == SDL_SCANCODE_F || ev.key.scancode == SDL_SCANCODE_F11 ||
                (alt && ev.key.scancode == SDL_SCANCODE_RETURN)) {
                if (!gpu_output_toggle_fullscreen(window, render_ctx.mask_alignment == 0))
                    fprintf(stderr, "Fullscreen: %s\n", SDL_GetError());
            }
            /* P: next preset. preset_apply re-pushes the chain in place. */
            if (ev.key.scancode == SDL_SCANCODE_P && preset_total_count() > 0) {
                int next = (preset_active_index() + 1) % preset_total_count();
                if (preset_load_index(next) >= 0)
                    fprintf(stderr, "Preset: %s\n", preset_display_name(preset_active_index()));
            }
        }
        /* The tube viewport follows the window, fullscreen and the preset's aspect. */
        if (!offscreen_w && video_gpu_chain.beam_out_w > 0) {
            int w, h;
            picture_area_size(window, &w, &h);
            beam_target_size(w, h, &w, &h);
            if (w > 0 && h > 0 && (w != video_gpu_chain.beam_out_w || h != video_gpu_chain.beam_out_h)) {
                render_ctx.display_tex = NULL;
                if (!video_gpu_set_beam_params(&video_gpu_chain, gpu, w, h, h / 240 > 0 ? h / 240 : 1,
                                               video_gpu_chain.beam_sigma_narrow, video_gpu_chain.beam_sigma_wide))
                    fprintf(stderr, "Could not resize CRT beam: %s\n", SDL_GetError());
            }
        }
        if (!offscreen_w) {
            Uint64 now = SDL_GetTicksNS();
            if (!deadline || now > deadline + period_ns * 3) deadline = now;
            while (now < deadline) {
                Uint64 remaining = deadline - now;
                SDL_DelayPrecise(remaining < 1000000 ? remaining : 1000000);
                now = SDL_GetTicksNS();
            }
            deadline += period_ns;
        }
        if (!gpu_render_prepare(&render_ctx)) { SDL_Delay(1); continue; }

        frame_ready = false;
        core.run();
        if (!frame_ready) {
            /* A duplicated frame keeps the previous picture. */
        }
        frame++;
        if (test_strokes) {
            /* Cross-colour test: white single-pixel vertical strokes every
             * fourth pixel on the left half, single-line horizontal strokes
             * every fourth line on the right half, on black. */
            uint32_t full = (uint32_t)(profile->ramp_n - 1);
            uint32_t white = full | (full << 6) | (full << 12);
            frame_width = 320; frame_height = 224;
            for (int y = 0; y < 224; y++)
                for (int x = 0; x < 320; x++)
                    codes[y * 320 + x] = x < 160 ? ((x & 3) == 0 ? white : 0) : ((y & 3) == 0 ? white : 0);
        } else if (test_bars) {
            /* Eight full-code bars: white, yellow, cyan, green, magenta, red, blue, black. */
            static const int bar_rgb[8][3] = {{1,1,1},{1,1,0},{0,1,1},{0,1,0},{1,0,1},{1,0,0},{0,0,1},{0,0,0}};
            uint32_t full = (uint32_t)(profile->ramp_n - 1);
            frame_width = 320; frame_height = 224;
            for (int y = 0; y < 224; y++)
                for (int x = 0; x < 320; x++) {
                    const int *c = bar_rgb[x * 8 / 320];
                    codes[y * 320 + x] = (c[0] ? full : 0) | ((c[1] ? full : 0) << 6) | ((c[2] ? full : 0) << 12);
                }
        }

        /* Console pixels onto the 2048-sample active line: 2048/width. */
        int width = frame_width > 0 ? frame_width : 256;
        int g = gcd(SIGNAL_NTSC_SAMPLES_PER_LINE, width);
        int lines = frame_height > 0 ? frame_height : 224;
        int top = profile->top_line;
        if (top + lines > 240) top = 240 - lines;
        VideoRGBSource src = {
            .pixels = codes, .width = width, .lines = lines,
            .top_line = top, .spp_num = SIGNAL_NTSC_SAMPLES_PER_LINE / g, .spp_den = width / g,
            .ramp = profile->ramp, .ramp_n = profile->ramp_n,
            .phase_base = phase, .phase_line_adv = profile->line_phase,
            .chroma_bw_hz = enc_chroma_mhz * 1e6f, .luma_bw_hz = enc_luma_mhz * 1e6f,
            .luma_trap = enc_trap, .setup = enc_setup,
        };

        /* Scene brightness for the receiver's supply and DC-restoration models. */
        double luma = 0;
        for (int i = 0; i < src.width * src.lines; i++) {
            uint32_t c = codes[i];
            luma += 0.299 * profile->ramp[c & 63] + 0.587 * profile->ramp[(c >> 6) & 63] + 0.114 * profile->ramp[(c >> 12) & 63];
        }
        render_ctx.frame_brightness = (float)(luma / (src.width * src.lines));
        gpu_render_update_dynamic_state(&render_ctx);

        /* Host UI on the RGB plane after the receiver, before the tube. */
        bool osd_visible = false;
        if (osd_menu_is_open) {
            memset(osd_pixels, 0, sizeof(osd_pixels));
            gpu_osd_render(osd_pixels, osd_menu_current(), osd_parameter_editing,
                           preset_display_name(preset_active_index()), preset_is_modified(),
                           false, &render_ctx, gpu_render_headroom(&render_ctx));
            osd_visible = true;
        } else {
            const char *notice = preset_cycle_notice();
            if (notice && *notice) {
                memset(osd_pixels, 0, sizeof(osd_pixels));
                gpu_osd_preset_notice(osd_pixels, notice);
                osd_visible = true;
            }
        }
        if (!video_gpu_set_osd(&video_gpu_chain, gpu, osd_visible ? osd_pixels : NULL))
            fprintf(stderr, "OSD upload failed: %s\n", SDL_GetError());

        video_gpu_chain.elapsed_frames = 1;
        video_gpu_chain.signal_frame_counter = frame - 1;
        video_gpu_chain.beam_frame_counter = frame - 1;
        render_ctx.frame_counter = frame - 1;
        render_ctx.source_phase = phase;
        /* The receiver's demodulator reference follows the frame's carrier
         * phase, as mynes_gpu does with the PPU clock; the chain subtracts
         * the source phase again, so the reference stays at the axes the
         * encoder was calibrated against. */
        video_gpu_set_demod(&video_gpu_chain, (float)phase * 2.0f * (float)M_PI / 12.0f, video_gpu_chain.demod_dp);
        if (render_ctx.white_dirty && render_ctx.hdr_enabled && render_ctx.hdr_gain_mode == 0 &&
            render_ctx.drawable_w > 0 && render_ctx.drawable_h > 0) {
            static unsigned failures;
            if (calibrate_white(gpu, &src)) { render_ctx.white_dirty = false; failures = 0; }
            else if (++failures >= 3) { render_ctx.white_dirty = false; fprintf(stderr, "White: measurement failed\n"); }
        }
        video_gpu_set_dynamic_state(&video_gpu_chain, render_ctx.hv_sag_state, render_ctx.apl_slow_state, 0.0f);
        float *rgb_out = NULL;
        if (calibrate && frame >= 30) rgb_out = calloc(video_gpu_chain.rgb_size / sizeof(float), sizeof(float));
        if (!video_gpu_process_rgb(&video_gpu_chain, gpu, &src, rgb_out)) {
            fprintf(stderr, "GPU chain failed on frame %u: %s\n", frame, SDL_GetError());
            running = false;
            break;
        }
        if (rgb_out && test_strokes) {
            /* Mean chroma magnitude and luma of the decoded picture per half. */
            int spl = video_chain.signal_fmt.samples_per_line;
            for (int half = 0; half < 2; half++) {
                double chroma = 0, luma = 0; int n = 0;
                for (int y = top + 20; y < top + lines - 20; y++)
                    for (int x = half * spl / 2 + spl / 16; x < (half + 1) * spl / 2 - spl / 16; x++) {
                        const float *px = rgb_out + ((size_t)y * spl + x) * 3;
                        double yy = 0.299 * px[0] + 0.587 * px[1] + 0.114 * px[2];
                        chroma += sqrt((px[0] - yy) * (px[0] - yy) + (px[2] - yy) * (px[2] - yy));
                        luma += yy; n++;
                    }
                printf("%s strokes: mean chroma %.4f, mean luma %.4f\n",
                       half ? "horizontal" : "vertical", chroma / n, luma / n);
            }
            free(rgb_out);
            running = false;
        } else if (rgb_out) {
            /* Decoded gun voltages before the tube, averaged over the centre of each bar. */
            static const char *names[8] = {"white","yellow","cyan","green","magenta","red","blue","black"};
            int spl = video_chain.signal_fmt.samples_per_line;
            for (int b = 0; b < 8; b++) {
                double acc[3] = {0}; int n = 0;
                for (int y = top + 40; y < top + lines - 40; y++)
                    for (int x = b * spl / 8 + spl / 32; x < (b + 1) * spl / 8 - spl / 32; x++) {
                        const float *px = rgb_out + ((size_t)y * spl + x) * 3;
                        acc[0] += px[0]; acc[1] += px[1]; acc[2] += px[2]; n++;
                    }
                printf("%-8s R=%.3f G=%.3f B=%.3f\n", names[b], acc[0] / n, acc[1] / n, acc[2] / n);
            }
            free(rgb_out);
            running = false;
        }
        phase = (phase + profile->frame_phase_adv[frame & 1]) % 12;

        int beam_w, beam_h;
        if (video_gpu_get_beam_size(&video_gpu_chain, &beam_w, &beam_h)) {
            render_ctx.display_tex = video_gpu_get_beam_texture(&video_gpu_chain);
            render_ctx.display_tex_w = beam_w;
            render_ctx.display_tex_h = beam_h;
        }
        bool capture = screenshot_after > 0 && frame >= (unsigned)screenshot_after;
        render_ctx.capture_path = capture ? screenshot_path : NULL;
        render_ctx.capture_async = false;
        gpu_render_frame(&render_ctx, &video_chain);
        if (capture) {
            fprintf(stderr, "Capture frame %u, carrier phase %d: %s\n", frame, phase, screenshot_path);
            if (!render_ctx.capture_accepted) fprintf(stderr, "Capture failed: %s\n", SDL_GetError());
            render_ctx.capture_path = NULL;
            running = false;
        }
        if (frame_limit && frame >= frame_limit) running = false;
    }

    core.unload_game();
    core.deinit();
    if (audio_stream) SDL_DestroyAudioStream(audio_stream);
    gpu_display_destroy(&gpu_disp, gpu);
    video_gpu_destroy(&video_gpu_chain, gpu);
    SDL_ReleaseWindowFromGPUDevice(gpu, window);
    SDL_DestroyWindow(window);
    SDL_DestroyGPUDevice(gpu);
    SDL_Quit();
    dlclose(core.handle);
    return 0;
}
