/*
 * GPU Frontend -- SDL3 + SDL_GPU NES Emulator
 * =============================================
 *
 * Standalone GPU signal chain: SignalPrecompute provides the 2C02 voltage
 * table and FIR taps with ZERO dependency on composite.h. The CPU generates
 * the composite waveform from the palette index framebuffer, then GPU
 * compute shaders run the full Y/C separation, chroma demod, and matrix
 * decode. The CRT display pipeline (halation + barrel + mask + gamma)
 * renders at native display resolution.
 *
 * Build: cmake -DNES_BUILD_GPU_FRONTEND=ON
 * Run:   ./build/bin/mynes_gpu <rom.nes>
 */

#define SDL_MAIN_USE_CALLBACKS 0
#include <SDL3/SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <math.h>

/* NES core. */
#include "nes/nes.h"
#include "ppu/ppu.h"
#include "nes/apu.h"
#include "nes/rom.h"
#include "nes/mapper.h"
#include "nes/osd.h"

/* GPU signal chain (NO composite.h). */
#include "signal_precompute.h"
#include "video_gpu.h"
#include "gpu_half.h"
#include "gpu_display.h"
#include "audio_chain.h"
#include "audio_gpu.h"
#include "audio_sync.h"
#include "video_chain.h"
#include "presets.h"
#include "chain_vis.h"
#include "dump_frame.h"
#include "debug_tap.h"
#include "debug_server.h"

/* Extracted modules. */
#include "gpu_render.h"
#include "preset_apply.h"
#include "waveform_gen.h"
#include "test_signals.h"
#include "gpu_log.h"

/* Shared frontend helpers. */
#include "browser.h"
#include "config.h"

/* ============================================================================
 * Global emulator state
 * ============================================================================ */

static NES              nes;
static ROM              rom;
static bool             rom_loaded = false;
static bool             running = true;
static int              region = 0;  /* 0=NTSC, 1=PAL */

/* Signal chain state (standalone -- no Composite struct). */
static SignalPrecompute sig_state;
static VideoChain       video_chain;
static VideoGPUChain    video_gpu_chain;
static AudioChain       audio_chain;
static AudioGPUChain    audio_gpu;
static GPUDisplay       gpu_disp;

/* GPU pipeline flags. */
static bool             gpu_video_enabled = false;
static bool             gpu_audio_enabled = false;
static bool             gpu_display_enabled = false;
static int              use_gpu_audio = false;       /* A: toggle between CPU and GPU audio */
static bool             composite_enabled = true;   /* C: composite vs raw RGB */
static int              current_preset = 0;   /* index into scanned presets/ */

/* Test signal mode: 0=NES, 1=color bars, 2=sine sweep */
static int              test_signal_mode = 0;

/* CPU waveform + GPU RGB output buffers. */
static float           *waveform_buf = NULL;
static float           *gpu_rgb_out = NULL;
static unsigned         frame_count = 0;

/* One block per emulated frame; both backends preserve the same state. */
static float cpu_audio_frame[AUDIO_BLOCK_CAPACITY];
static float audio_output[AUDIO_BLOCK_CAPACITY];
static int audio_frame_pos;
static AudioState audio_state;
static AudioRateCtrl audio_rate_ctrl;
static bool audio_playing;
static int audio_fade_remaining;
static FILE *audio_capture, *audio_trace;

/* Chain visualiser. */
static ChainVis        *chain_vis = NULL;

/* Debug tap manager (GPU buffer readback for SwiftUI visualiser). */
static DebugTapManager *tap_mgr = NULL;

/* Debug server (for SwiftUI visualiser IPC). */
static DebugServer     *debug_srv = NULL;

/* SDL3 state. */
static SDL_Window      *window = NULL;
static SDL_GPUDevice   *gpu = NULL;
static SDL_AudioStream *audio_stream = NULL;

/* Controller. */
static uint8_t          controller_state = 0;

/* Render and preset contexts. */
static GPURenderCtx     render_ctx;
static PresetCtx        preset_ctx;

/* Browser + persistent config. The browser writes into the PPU
 * framebuffer (just like an emulated game frame) so the composite +
 * CRT shader pipeline applies to it the same way. No flag-stashing
 * needed when it opens or closes. */
static MynesConfig      mynes_config;
static Browser          browser;
static bool             browser_active = false;

/* Translate SDL3 scancode → browser key, or -1 if not handled. */
static int sdl_to_browser_key(int scancode) {
    switch (scancode) {
    case SDL_SCANCODE_UP:        return BROWSER_KEY_UP;
    case SDL_SCANCODE_DOWN:      return BROWSER_KEY_DOWN;
    case SDL_SCANCODE_LEFT:      return BROWSER_KEY_LEFT;
    case SDL_SCANCODE_RIGHT:     return BROWSER_KEY_RIGHT;
    case SDL_SCANCODE_RETURN:    return BROWSER_KEY_ENTER;
    case SDL_SCANCODE_KP_ENTER:  return BROWSER_KEY_ENTER;
    case SDL_SCANCODE_BACKSPACE: return BROWSER_KEY_BACK;
    case SDL_SCANCODE_ESCAPE:    return BROWSER_KEY_ESCAPE;
    case SDL_SCANCODE_PAGEUP:    return BROWSER_KEY_PAGEUP;
    case SDL_SCANCODE_PAGEDOWN:  return BROWSER_KEY_PAGEDOWN;
    default:                     return -1;
    }
}

/* Custom 64×3 RGB palette used for the raw-RGB display path (C key).
 * Loaded from a .pal file at startup; falls back to the built-in 2C02
 * palette on failure. */
static uint8_t raw_palette[64][3];
static bool    raw_palette_loaded = false;

/* Load a 192-byte (64 colors × RGB) palette file into raw_palette. */
static bool load_raw_palette(const char *path) {
    FILE *fp = fopen(path, "rb");
    if (!fp) return false;
    uint8_t buf[192];
    size_t n = fread(buf, 1, sizeof(buf), fp);
    fclose(fp);
    if (n != 192) return false;
    for (int i = 0; i < 64; i++) {
        raw_palette[i][0] = buf[i*3 + 0];
        raw_palette[i][1] = buf[i*3 + 1];
        raw_palette[i][2] = buf[i*3 + 2];
    }
    return true;
}

/* Performance overlay (V key). */
static bool             perf_overlay = false;
static char             perf_text[128] = "";

/* ============================================================================
 * Audio callback
 * ============================================================================ */

/* Returns true if `frame` appears in the debug-dump frame list. */
static bool frame_wanted(unsigned frame, const int *list, int n) {
    for (int i = 0; i < n; i++) {
        if ((int)frame == list[i]) return true;
    }
    return false;
}

static void apu_sample_callback(void *user_data, float sample) {
    (void)user_data;

    if (audio_frame_pos < AUDIO_BLOCK_CAPACITY)
        cpu_audio_frame[audio_frame_pos++] = sample;
}

float g_audio_bass_rms = 0.0f;

static void audio_reset(void) {
    if (audio_stream) {
        SDL_ClearAudioStream(audio_stream);
        SDL_SetAudioStreamFrequencyRatio(audio_stream, 1.0f);
    }
    memset(&audio_state, 0, sizeof(audio_state));
    memset(&audio_rate_ctrl, 0, sizeof(audio_rate_ctrl));
    audio_frame_pos = 0;
    audio_fade_remaining = AUDIO_STREAM_RATE / 200;
    g_audio_bass_rms = 0;
    nes.apu.hp_filter1 = nes.apu.hp_filter2 = nes.apu.lp_filter = 0;
    nes.apu.hp_prev_input1 = nes.apu.hp_prev_input2 = 0;
}

static void audio_adjust_rate(double frame_seconds) {
    if (!audio_stream) return;
    int bytes = SDL_GetAudioStreamQueued(audio_stream);
    if (bytes >= 0) SDL_SetAudioStreamFrequencyRatio(audio_stream,
        audio_sync_ratio(&audio_rate_ctrl, bytes / (int)sizeof(float), frame_seconds));
}

static void audio_submit_frame(void) {
    int count = audio_frame_pos;
    audio_frame_pos = 0;
    if (!count || !audio_stream) return;
    bool processed = use_gpu_audio && gpu_audio_enabled &&
        audio_gpu_process(&audio_gpu, gpu, &audio_state, cpu_audio_frame, audio_output, count);
    if (!processed) {
        if (use_gpu_audio) {
            fprintf(stderr, "GPU audio failed; continuing with CPU chain: %s\n", SDL_GetError());
            use_gpu_audio = false;
        }
        audio_chain_process(&audio_chain, &audio_state, cpu_audio_frame, audio_output, count);
    }
    int bytes = SDL_GetAudioStreamQueued(audio_stream);
    if (bytes >= 0 && audio_sync_stale(bytes / (int)sizeof(float), count)) {
        // A long presentation/OS stall must not leave old gameplay queued.
        SDL_ClearAudioStream(audio_stream);
        memset(&audio_rate_ctrl, 0, sizeof(audio_rate_ctrl));
        SDL_SetAudioStreamFrequencyRatio(audio_stream, 1.0f);
        audio_fade_remaining = AUDIO_STREAM_RATE / 200;
    }
    for (int i = 0; i < count; ++i) {
        float sample = fmaxf(-1.0f, fminf(1.0f, audio_output[i] * 1.5f));
        // Track post-coupling acoustic energy, not the APU DAC's DC bias.
        g_audio_bass_rms = .9995f * g_audio_bass_rms + .0005f * sample * sample;
        if (audio_fade_remaining > 0) {
            sample *= 1.0f - (float)audio_fade_remaining / (AUDIO_STREAM_RATE / 200);
            --audio_fade_remaining;
        }
        audio_output[i] = sample;
    }
    if (audio_capture) fwrite(audio_output, sizeof(float), count, audio_capture);
    if (audio_trace) fprintf(audio_trace, "%u,%d,%d,%.7f,%d\n", frame_count, count,
        SDL_GetAudioStreamQueued(audio_stream) / (int)sizeof(float),
        SDL_GetAudioStreamFrequencyRatio(audio_stream), use_gpu_audio);
    if (!SDL_PutAudioStreamData(audio_stream, audio_output, count * sizeof(float)))
        fprintf(stderr, "Audio submission failed: %s\n", SDL_GetError());
}

/* ============================================================================
 * Input handling
 * ============================================================================ */

static void handle_key(SDL_Scancode sc, bool down) {
    uint8_t mask = 0;
    switch (sc) {
        case SDL_SCANCODE_X:      mask = 0x01; break;  /* A */
        case SDL_SCANCODE_Z:      mask = 0x02; break;  /* B */
        case SDL_SCANCODE_TAB:    mask = 0x04; break;  /* Select */
        case SDL_SCANCODE_RETURN: mask = 0x08; break;  /* Start */
        case SDL_SCANCODE_UP:     mask = 0x10; break;
        case SDL_SCANCODE_DOWN:   mask = 0x20; break;
        case SDL_SCANCODE_LEFT:   mask = 0x40; break;
        case SDL_SCANCODE_RIGHT:  mask = 0x80; break;
        default: return;
    }
    if (down) controller_state |= mask;
    else      controller_state &= ~mask;
    nes.controller[0] = controller_state;
}

/* ============================================================================
 * Main
 * ============================================================================ */

int main(int argc, char **argv) {
    /* Argument parsing: exactly one positional (the ROM path), plus a
     * few optional flags used for development / visualiser integration. */
    const char *rom_path = NULL;
    /* --debug-dump[=frames]: PPM dumps + chroma printouts on listed frames.
     *   --debug-dump            → frames 3 and 60 (built-in defaults)
     *   --debug-dump=3,60,120   → whatever frames you pass (comma-separated).
     * debug_dump_frames holds up to 32 frame numbers. */
    bool debug_dump = false;
    int debug_dump_frames[32];
    int debug_dump_count = 0;
    bool debug_server_enabled = false;  /* --debug-server: ChainVisualiser IPC socket */
    const char *debug_socket = getenv("MYNES_DEBUG_SOCKET");
    if (!debug_socket) debug_socket = "/tmp/mynes_gpu_debug.sock";
    const char *static_frame_path = NULL;  /* --simulate-frame <file.bin>: bypass emulation */
    const char *preset_path = NULL;        /* --preset <path.json>: apply on startup */
    int screenshot_after = 0;              /* --screenshot-after <N>: dump and exit */
    const char *screenshot_path = "/tmp/gpu_capture.ppm";
    uint8_t *static_frame_buf = NULL;      /* 256*240 palette indices when loaded */
    for (int i = 1; i < argc; i++) {
        if (strncmp(argv[i], "--debug-dump", 12) == 0
            && (argv[i][12] == '\0' || argv[i][12] == '=')) {
            debug_dump = true;
            if (argv[i][12] == '=') {
                /* Parse comma-separated frame list. */
                char *list = argv[i] + 13;
                char *tok = strtok(list, ",");
                while (tok && debug_dump_count < 32) {
                    int f = atoi(tok);
                    if (f >= 0) debug_dump_frames[debug_dump_count++] = f;
                    tok = strtok(NULL, ",");
                }
            }
            if (debug_dump_count == 0) {
                /* Default frames when no list was given. */
                debug_dump_frames[0] = 3;
                debug_dump_frames[1] = 60;
                debug_dump_count = 2;
            }
        } else if (strcmp(argv[i], "--debug-server") == 0) {
            debug_server_enabled = true;
        } else if (strcmp(argv[i], "--simulate-frame") == 0 && i + 1 < argc) {
            static_frame_path = argv[++i];
        } else if (strcmp(argv[i], "--preset") == 0 && i + 1 < argc) {
            preset_path = argv[++i];
        } else if (strcmp(argv[i], "--screenshot-after") == 0 && i + 1 < argc) {
            screenshot_after = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--screenshot-path") == 0 && i + 1 < argc) {
            screenshot_path = argv[++i];
        } else if (strcmp(argv[i], "--verbose") == 0 || strcmp(argv[i], "-v") == 0) {
            gpu_verbose = true;
        } else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            printf("Usage: %s [options] <rom.nes>\n"
                   "\n"
                   "NES emulator — experimental GPU frontend (SDL3 + SDL_GPU).\n"
                   "\n"
                   "Options:\n"
                   "  -v, --verbose         Print detailed startup info (shader loads,\n"
                   "                        FIR taps, chain stages, beam tuning, …)\n"
                   "  --debug-dump[=list]   Dump per-stage buffers to /tmp at the listed\n"
                   "                        frames. Default list: 3,60. Example:\n"
                   "                          --debug-dump=3,60,120,300\n"
                   "  --debug-server        Start Unix socket at /tmp/mynes_gpu_debug.sock\n"
                   "                        for the ChainVisualiser dev tool\n"
                   "                        (tools/visualiser)\n"
                   "  -h, --help            Show this help\n",
                   argv[0]);
            return 0;
        } else if (argv[i][0] == '-') {
            fprintf(stderr, "Unknown option: %s (try --help)\n", argv[i]);
            return 1;
        } else if (!rom_path) {
            rom_path = argv[i];
        } else {
            fprintf(stderr, "Unexpected argument: %s\n", argv[i]);
            return 1;
        }
    }
    /* rom_path may be NULL — in that case the startup ROM browser runs
     * after the SDL/GPU init below, then sets rom_path before continuing. */

    fprintf(stderr, "MyNES — SDL3 GPU signal / CRT frontend\n");

    /* Persistent config (recent ROMs, last preset). */
    mynes_config_load(&mynes_config);

    /* --- SDL3 init --- */
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO)) {
        fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
        return 1;
    }

    gpu = SDL_CreateGPUDevice(
        SDL_GPU_SHADERFORMAT_SPIRV | SDL_GPU_SHADERFORMAT_MSL | SDL_GPU_SHADERFORMAT_METALLIB,
        SDL_getenv("MYNES_GPU_VALIDATION") != NULL, NULL
    );
    if (!gpu) {
        fprintf(stderr, "SDL_CreateGPUDevice failed: %s\n", SDL_GetError());
        SDL_Quit();
        return 1;
    }
    LOGV("GPU backend: %s\n", SDL_GetGPUDeviceDriver(gpu));

    window = SDL_CreateWindow(
        "MyNES (GPU)",
        1280, 960,
        SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY
    );
    if (!window) {
        fprintf(stderr, "SDL_CreateWindow failed: %s\n", SDL_GetError());
        SDL_DestroyGPUDevice(gpu);
        SDL_Quit();
        return 1;
    }

    if (!SDL_ClaimWindowForGPUDevice(gpu, window)) {
        fprintf(stderr, "SDL_ClaimWindowForGPUDevice failed: %s\n", SDL_GetError());
        SDL_DestroyWindow(window);
        SDL_DestroyGPUDevice(gpu);
        SDL_Quit();
        return 1;
    }

    /* Enable vsync + HDR (EDR on macOS) if supported. */
    bool hdr_available = SDL_WindowSupportsGPUSwapchainComposition(gpu, window,
        SDL_GPU_SWAPCHAINCOMPOSITION_HDR_EXTENDED_LINEAR);
    /* Enable EDR (HDR extended linear) if available. Values >1.0 map to
     * brighter-than-SDR-white on the display. Screenshots may appear
     * brighter than on-screen due to sRGB capture of linear values. */
    if (hdr_available) {
        hdr_available = SDL_SetGPUSwapchainParameters(gpu, window,
            SDL_GPU_SWAPCHAINCOMPOSITION_HDR_EXTENDED_LINEAR,
            SDL_GPU_PRESENTMODE_VSYNC);
        LOGV("HDR: Extended linear (EDR) enabled\n");
    }
    if (!hdr_available) {
        SDL_SetGPUSwapchainParameters(gpu, window,
            SDL_GPU_SWAPCHAINCOMPOSITION_SDR,
            SDL_GPU_PRESENTMODE_VSYNC);
        LOGV("HDR: not available, using SDR\n");
    }

    /* --- Audio --- */
    SDL_AudioSpec spec = {0};
    spec.freq = 44100;
    spec.format = SDL_AUDIO_F32;
    spec.channels = 1;
    audio_stream = SDL_OpenAudioDeviceStream(
        SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK,
        &spec, NULL, NULL
    );
    if (audio_stream) {
        SDL_ResumeAudioStreamDevice(audio_stream);
    }

    /* --- NES init --- */
    nes_init(&nes);
    apu_set_audio_callback(&nes.apu, apu_sample_callback, NULL);

    /* Try to load a realistic RGB palette for the raw-display path.
     * Checked in priority order; first hit wins. */
    const char *pal_candidates[] = {
        "palettes/Digital Prime (FBX).pal",
        "../palettes/Digital Prime (FBX).pal",
        "../../palettes/Digital Prime (FBX).pal",
        NULL
    };
    for (int i = 0; pal_candidates[i]; i++) {
        if (load_raw_palette(pal_candidates[i])) {
            raw_palette_loaded = true;
            nes.ppu.color_palette = raw_palette;
            LOGV("Loaded raw palette: %s\n", pal_candidates[i]);
            break;
        }
    }
    if (!raw_palette_loaded)
        fprintf(stderr, "No custom palette found — using built-in 2C02\n");

    /* If no ROM was given on argv, the browser opens immediately in the
     * main loop below. The rest of init (signal chain, GPU pipeline,
     * preset, display) runs with NTSC defaults so the browser frames go
     * through exactly the same composite + CRT path as a real game. */
    if (rom_path) {
        int err = nes_rom_load(&rom, rom_path);
        if (err != ROM_OK) {
            fprintf(stderr, "Failed to load ROM: %s\n", nes_rom_error_str(err));
            SDL_DestroyWindow(window);
            SDL_DestroyGPUDevice(gpu);
            SDL_Quit();
            return 1;
        }

        nes_load_mapper(&nes, rom.mapper,
                        rom.prg_rom, rom.prg_size,
                        rom.chr_rom, rom.chr_size,
                        rom.mirroring);

        /* Persist this ROM as the most-recent. */
        mynes_config_add_recent(&mynes_config, rom_path);
        mynes_config_save(&mynes_config);
    } else {
        /* Open the browser as soon as the main loop starts. */
        browser_init(&browser, NULL, &mynes_config);
        browser_active = true;
    }

    /* If we have a ROM, honour its region and reset the bus.
     * If we don't, the GPU pipeline below initialises with NTSC defaults
     * — the browser draws into the PPU framebuffer and rides the same
     * composite + CRT pipeline a real game would. */
    if (rom_path) {
        if (rom.tv_system == NES_TV_PAL) {
            region = 1;
            ppu_set_region(&nes.ppu, PPU_REGION_PAL);
            apu_set_region(&nes.apu, 1);
        }
        nes_reset(&nes);
        rom_loaded = true;
        if (gpu_verbose) nes_rom_print_info(&rom);
    }

    /* --simulate-frame: load a 256*240 palette-index buffer. Main loop
     * paints it into the PPU every frame in place of running the CPU. */
    if (static_frame_path) {
        FILE *fp = fopen(static_frame_path, "rb");
        if (!fp) {
            fprintf(stderr, "Failed to open static frame %s\n", static_frame_path);
            return 1;
        }
        static_frame_buf = (uint8_t *)malloc(256 * 240);
        size_t n = fread(static_frame_buf, 1, 256 * 240, fp);
        fclose(fp);
        if (n != 256 * 240) {
            fprintf(stderr, "Static frame wrong size: %zu\n", n);
            return 1;
        }
        browser_active = false;
        rom_loaded = true;  /* so the main loop doesn't skip rendering */
        fprintf(stderr, "Static-frame mode: %s loaded\n", static_frame_path);
    }

    /* --- Signal precompute (standalone, no composite.h) --- */
    signal_precompute_init(&sig_state, region);
    LOGV("Signal: %d spp, %d spl (%s)\n",
         sig_state.samples_per_pixel, sig_state.samples_per_line,
         region ? "PAL" : "NTSC");

    /* --- Initialize preset context and apply default preset --- */
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
    preset_ctx.nes = &nes;
    preset_ctx.chain_vis = NULL;  /* set after chain_vis_create */
    preset_ctx.shader_dir = NULL; /* set after shader_dir_buf is resolved */
    preset_ctx.render_ctx = &render_ctx;
    preset_ctx_init(&preset_ctx);

    /* Default preset selection. Apply once pre-GPU (populates FIR taps,
     * signal table, audio-chain sizing needed by video_gpu_init /
     * audio_gpu_init) and once post-GPU (pushes beam params, temporal
     * blend weights, stage param re-push against the live chain).
     * Single-call flows leave either GPU-side state stale (post-only)
     * or init with zero-size buffers (pre-only). */
    int startup_preset_idx = -1;
    if (preset_path) {
        const char *slash = strrchr(preset_path, '/');
        const char *stem = slash ? slash + 1 : preset_path;
        startup_preset_idx = preset_find_by_slug(stem);
    }
    if (startup_preset_idx < 0 && mynes_config.last_preset[0])
        startup_preset_idx = preset_find_by_slug(mynes_config.last_preset);
    if (startup_preset_idx < 0)
        startup_preset_idx = preset_find_by_slug("reference_composite");
    if (startup_preset_idx < 0 && preset_total_count() > 0)
        startup_preset_idx = 0;
    if (startup_preset_idx >= 0) {
        preset_load_index_exact(startup_preset_idx);
    }

    /* --- Resolve shader directory --- */
    char shader_dir_buf[1024];
    const char *base = SDL_GetBasePath();
    if (base) {
        snprintf(shader_dir_buf, sizeof(shader_dir_buf),
                 "%s../shaders/compute", base);
    } else {
        snprintf(shader_dir_buf, sizeof(shader_dir_buf),
                 "frontends/gpu/shaders/compute");
    }
    const char *shader_dir = shader_dir_buf;

    /* GPU and CPU backends consume the same band-limited APU samples. */
    gpu_audio_enabled = audio_gpu_init(&audio_gpu, gpu, &audio_chain, shader_dir);
    if (!gpu_audio_enabled) fprintf(stderr, "GPU audio unavailable; CPU chain remains active\n");
    use_gpu_audio = gpu_audio_enabled && getenv("MYNES_GPU_AUDIO") != NULL;
    audio_reset();
    const char *audio_capture_path = getenv("MYNES_AUDIO_CAPTURE");
    const char *audio_trace_path = getenv("MYNES_AUDIO_TRACE");
    if (audio_capture_path) audio_capture = fopen(audio_capture_path, "wb");
    if (audio_trace_path) {
        audio_trace = fopen(audio_trace_path, "w");
        if (audio_trace) fprintf(audio_trace, "frame,samples,queued,ratio,gpu\n");
    }

    /* --- GPU video signal chain --- */
    if (video_gpu_init(&video_gpu_chain, gpu, &video_chain, shader_dir,
                       sig_state.fir_y, sig_state.fir_y_n,
                       sig_state.fir_c, sig_state.fir_c_n,
                       sig_state.fir_q, sig_state.fir_q_n)) {
        gpu_video_enabled = true;
        int spl = sig_state.samples_per_line;
        /* Size CPU-side readback buffers for the wider region (PAL)
         * so mid-session region switches never require reallocating.
         * NTSC wastes ~500 KB of host RAM; acceptable price for
         * simplifying the switch path. */
        int wf_size = SIGNAL_MAX_FRAME_FLOATS;
        waveform_buf = (float *)calloc(wf_size, sizeof(float));
        gpu_rgb_out = (float *)calloc(wf_size * 3, sizeof(float));
        LOGV("GPU video chain: initialized (%dx240, %d-sample buffers)\n",
               spl, wf_size);

        /* Upload signal table to GPU. */
        video_gpu_set_color_matrix(&video_gpu_chain,
                                   sig_state.color_matrix, sig_state.color_bias);

        /* PAL needs both tables (alt-line V-phase inversion); NTSC
         * leaves table_alt untouched, so pass NULL to release any
         * leftover buffer from a previous region switch. */
        const float *alt = (sig_state.region == SIGNAL_REGION_PAL)
                           ? (const float *)sig_state.table_alt
                           : NULL;
        if (!video_gpu_upload_signal_table(&video_gpu_chain, gpu,
                                           (const float *)sig_state.table, alt,
                                           SIG_TABLE_ENTRIES, SIG_TABLE_STRIDE)) {
            fprintf(stderr, "Signal table upload failed\n");
        }

        /* Set chroma demodulator phase increment from the live region. */
        {
            float fsc = signal_region_subcarrier_hz(sig_state.region);
            float fsample = signal_region_sample_rate_hz(sig_state.region);
            float dp = 2.0f * (float)M_PI * fsc / fsample;
            video_gpu_chain.signal_phase_base = sig_state.phase_base;
                video_gpu_chain.signal_line_phase = sig_state.phase_line_adv;
                video_gpu_chain.demod_line_phase = sig_state.phase_line_adv * (2.0f * (float)M_PI / 12.0f);
                video_gpu_set_demod(&video_gpu_chain, 0.0f, dp);
            LOGV("Demod: dp=%.6f rad/sample (Fsc=%.3f MHz, Fs=%.3f MHz)\n",
                 dp, fsc/1e6, fsample/1e6);
        }

        /* Configure beam profile (GPU scanline structure).
         * 4 output rows per NES scanline -> 240*4 = 960 output height.
         * Output width matches signal resolution for now (blit scales). */
        {
            /* Pixel-perfect: match beam to window's physical pixel size.
             * 4:3 viewport within the window — no stretching needed. */
            int win_pw, win_ph;
            SDL_GetWindowSizeInPixels(window, &win_pw, &win_ph);
            float aspect = 4.0f / 3.0f;
            int beam_w, beam_h, beam_rps;
            if ((float)win_pw / (float)win_ph > aspect) {
                /* Window wider than 4:3 — height-limited. */
                beam_h = win_ph;
                beam_w = (int)(beam_h * aspect);
            } else {
                /* Window taller than 4:3 — width-limited. */
                beam_w = win_pw;
                beam_h = (int)(beam_w / aspect);
            }
            beam_rps = beam_h / 240;
            if (beam_rps < 1) beam_rps = 1;

            LOGV("Pixel-perfect beam: %dx%d (%d rps, window %dx%d)\n",
                 beam_w, beam_h, beam_rps, win_pw, win_ph);
            if (video_gpu_set_beam_params(&video_gpu_chain, gpu,
                                          beam_w, beam_h, beam_rps,
                                          0.20f, 0.70f)) {
                LOGV("Beam profile: %dx%d (%d rows/scanline)\n",
                     beam_w, beam_h, beam_rps);
                /* Push preset beam params — the hardcoded sigma above is just
                 * for buffer allocation. The real values come from the preset. */
                {
                    TVDisplayParams *tv = &video_chain.tv;
                    float sig_n = (0.35f - tv->beam_sharpness * 0.20f) * (0.4f + tv->beam_height_min * 0.8f);
                    float sig_w = 0.30f + tv->beam_height_max * 0.33f;
                    if (sig_n < 0.10f) sig_n = 0.10f;
                    if (sig_w < 0.20f) sig_w = 0.20f;
                    video_gpu_chain.beam_sigma_narrow = sig_n;
                    video_gpu_chain.beam_sigma_wide = sig_w;
                    video_gpu_chain.beam_h_blur_sigma = tv->beam_spot_size > 0 ? tv->beam_spot_size : 6.0f;
                    video_gpu_chain.temporal_blend = 0.0f;  /* 0=dot crawl visible, 0.5=cancel */
                    LOGV("Beam from preset: sig_n=%.2f sig_w=%.2f spot=%.1f\n", sig_n, sig_w, video_gpu_chain.beam_h_blur_sigma);
                }
            }
        }
    } else {
        printf("GPU video chain: not available\n");
        gpu_video_enabled = false;
    }

    /* The active preset was applied in the init block above; its name
     * was already printed by preset_load_by_index(). */

    /* --- GPU display pipeline (CRT shaders) --- */
    {
        char render_shader_dir[1024];
        if (base) {
            snprintf(render_shader_dir, sizeof(render_shader_dir),
                     "%s../shaders/render", base);
        } else {
            snprintf(render_shader_dir, sizeof(render_shader_dir),
                     "frontends/gpu/shaders/render");
        }
        if (gpu_display_init(&gpu_disp, gpu, window, render_shader_dir)) {
            gpu_display_enabled = true;
            LOGV("GPU display pipeline: initialized (CRT shader active)\n");
        } else {
            printf("GPU display pipeline: not available (falling back to blit)\n");
            gpu_display_enabled = false;
        }
    }

    /* --- Initialize render context --- */
    render_ctx.gpu = gpu;
    render_ctx.window = window;
    render_ctx.display_tex = NULL;
    render_ctx.display_tex_w = 0;
    render_ctx.display_tex_h = 0;
    render_ctx.gpu_disp = &gpu_disp;
    render_ctx.gpu_display_enabled = gpu_display_enabled;
    render_ctx.crt_shader_enabled = true;
    render_ctx.hdr_enabled = hdr_available;

    /* --- Chain visualiser --- */
    chain_vis = chain_vis_create(&video_chain, &audio_chain);
    preset_ctx.chain_vis = chain_vis;
    preset_ctx.shader_dir = shader_dir;

    /* Second half of the preset apply — the CPU-side state was set up
     * pre-GPU-init so audio_gpu_init / video_gpu_init could size their
     * buffers from the preset's FIR tap counts and audio params. Now
     * that the chain is live, push the GPU-side uniforms / tap buffers
     * / stage params. One call, not a re-parse of the preset JSON. */
    preset_apply_gpu_push(&preset_ctx);

    /* ROM region overrides the preset region. The preset is a
     * "video chain" description (cable + TV + colour matrix), not a
     * region decision — an iNES header that flags PAL is an
     * authoritative property of the ROM and the whole pipeline
     * must follow. If the preset just loaded has set region back
     * to NTSC (which most presets do) but the ROM is PAL, flip
     * the pipeline now. preset_set_region handles the full
     * teardown + rebuild. */
    if (rom_loaded && rom.tv_system == NES_TV_PAL
        && preset_ctx.region != SIGNAL_REGION_PAL) {
        preset_set_region(&preset_ctx, SIGNAL_REGION_PAL);
    } else if (rom_loaded && rom.tv_system == NES_TV_NTSC
               && preset_ctx.region != SIGNAL_REGION_NTSC) {
        preset_set_region(&preset_ctx, SIGNAL_REGION_NTSC);
    }

    /* --- Debug tap manager (GPU buffer readback for visualiser) --- */
    tap_mgr = debug_tap_create(gpu,
                               gpu_video_enabled ? &video_gpu_chain.sig_chain : NULL,
                               NULL);
    if (tap_mgr) {
        LOGV("Debug tap manager: initialized (%d video, %d audio stages)\n",
               debug_tap_video_stage_count(tap_mgr),
               debug_tap_audio_stage_count(tap_mgr));
    }

    /* --- Debug server (for SwiftUI visualiser, --debug-server only) --- */
    if (debug_server_enabled) {
        debug_srv = debug_server_create(debug_socket);
        if (!debug_srv) {
            fprintf(stderr, "Warning: debug server failed to start on %s\n", debug_socket);
        } else {
            preset_register_debug_controls(&preset_ctx, debug_srv);
            printf("Debug server ready for visualiser connections on %s\n", debug_socket);
        }
    }

    /* ========================================================================
     * Main loop
     * ======================================================================== */

    Uint64 frame_deadline = 0;
    while (running) {
        /* The OSD region toggle can flip the pipeline from inside
         * preset_apply.c; it can't touch tap_mgr (owned by main) so
         * it raises chain_rebuilt_flag and we do the destroy+create
         * here, between frames. */
        if (preset_ctx.chain_rebuilt_flag) {
            preset_ctx.chain_rebuilt_flag = false;
            if (tap_mgr) {
                debug_tap_destroy(tap_mgr);
                tap_mgr = debug_tap_create(gpu,
                    gpu_video_enabled ? &video_gpu_chain.sig_chain : NULL,
                    NULL);
            }
        }

        /* --- Event processing --- */
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            switch (ev.type) {
                case SDL_EVENT_QUIT:
                    running = false;
                    break;

                case SDL_EVENT_KEY_DOWN:
                    /* ROM browser owns input when active. The browser
                     * writes into the PPU framebuffer and rides the same
                     * composite + CRT pipeline as the game, so there's
                     * nothing to disable when it opens. */
                    if (browser_active) {
                        int bk = sdl_to_browser_key(ev.key.scancode);
                        /* Quietly ignore unmapped keys when browsing. */
                        if (bk < 0) break;

                        BrowserResult r = browser_handle_key(&browser,
                                                             (BrowserKey)bk);
                        if (r == BROWSER_SELECTED) {
                            ROM new_rom;
                            int re = nes_rom_load(&new_rom, browser.chosen_path);
                            if (re == ROM_OK) {
                                nes_load_mapper(&nes, new_rom.mapper,
                                    new_rom.prg_rom, new_rom.prg_size,
                                    new_rom.chr_rom, new_rom.chr_size,
                                    new_rom.mirroring);
                                /* Auto-detect region from the iNES
                                 * header (bit 0 of byte 9). Flip the
                                 * PPU + APU + GPU pipeline so PAL
                                 * ROMs decode with the 2C07 table +
                                 * correct per-line V-phase inversion. */
                                int new_region = (new_rom.tv_system == NES_TV_PAL)
                                                 ? SIGNAL_REGION_PAL
                                                 : SIGNAL_REGION_NTSC;
                                if (new_region != preset_ctx.region) {
                                    bool rebuilt = preset_set_region(
                                        &preset_ctx, new_region);
                                    if (rebuilt && tap_mgr) {
                                        /* tap_mgr caches per-stage
                                         * metadata that's stale
                                         * after the rebuild. */
                                        debug_tap_destroy(tap_mgr);
                                        tap_mgr = debug_tap_create(gpu,
                                            gpu_video_enabled ? &video_gpu_chain.sig_chain : NULL,
                                            NULL);
                                    }
                                    region = new_region;
                                    ppu_set_region(&nes.ppu,
                                        new_region == SIGNAL_REGION_PAL
                                            ? PPU_REGION_PAL : PPU_REGION_NTSC);
                                    apu_set_region(&nes.apu,
                                        new_region == SIGNAL_REGION_PAL ? 1 : 0);
                                }
                                nes_reset(&nes);
                                audio_reset();
                                rom = new_rom;
                                rom_loaded = true;
                                free(static_frame_buf);
                                static_frame_buf = NULL;
                                mynes_config_add_recent(&mynes_config,
                                                        browser.chosen_path);
                                mynes_config_save(&mynes_config);
                                fprintf(stderr, "Loaded %s\n", browser.chosen_path);
                            } else {
                                fprintf(stderr, "Failed to load %s: %s\n",
                                    browser.chosen_path, nes_rom_error_str(re));
                            }
                        }
                        if (r == BROWSER_CANCELLED && !rom_loaded) {
                            /* Started without a ROM and the user cancelled — quit. */
                            running = false;
                        }
                        if (r != BROWSER_BROWSING) browser_active = false;
                        break;
                    }
                    /* O: reopen the browser mid-session. */
                    if (ev.key.scancode == SDL_SCANCODE_O) {
                        browser_init(&browser, NULL, &mynes_config);
                        browser_active = true;
                        break;
                    }
                    /* L: toggle chain visualiser. */
                    if (ev.key.scancode == SDL_SCANCODE_L) {
                        chain_vis_toggle(chain_vis);
                        break;
                    }
                    /* Chain vis consumes navigation keys when open. */
                    if (chain_vis_handle_key(chain_vis, ev.key.scancode, true))
                        break;
                    /* OSD menu navigation. */
                    if (osd_menu_is_open) {
                        if (ev.key.scancode == SDL_SCANCODE_UP)        { osd_menu_move(-1); break; }
                        if (ev.key.scancode == SDL_SCANCODE_DOWN)      { osd_menu_move(+1); break; }
                        if (ev.key.scancode == SDL_SCANCODE_LEFT)      { osd_menu_adjust(-1); break; }
                        if (ev.key.scancode == SDL_SCANCODE_RIGHT)     { osd_menu_adjust(+1); break; }
                        if (ev.key.scancode == SDL_SCANCODE_RETURN)    { osd_menu_activate(); break; }
                        if (ev.key.scancode == SDL_SCANCODE_BACKSPACE ||
                            ev.key.scancode == SDL_SCANCODE_ESCAPE)    { osd_menu_back(); break; }
                        if (ev.key.scancode == SDL_SCANCODE_M)         { osd_menu_close(); break; }
                    }
                    /* Escape: quit. */
                    if (ev.key.scancode == SDL_SCANCODE_ESCAPE) { running = false; break; }
                    /* M: open OSD menu. */
                    if (ev.key.scancode == SDL_SCANCODE_M) {
                        osd_menu_open_root(preset_menu_root,
                                           preset_menu_root_count, "SETUP");
                        break;
                    }
                    /* Shift+C: toggle split-view (left CRT, right raw palette).
                     * C alone: toggle composite vs raw RGB; also switches off CRT
                     * when composite is off (raw palette shouldn't have CRT effects). */
                    if (ev.key.scancode == SDL_SCANCODE_C) {
                        if (ev.key.mod & SDL_KMOD_SHIFT) {
                            render_ctx.split_mode = !render_ctx.split_mode;
                            printf("Split view: %s\n",
                                   render_ctx.split_mode ? "ON (left=CRT, right=raw)" : "OFF");
                        } else {
                            composite_enabled = !composite_enabled;
                            /* Raw palette mode → no CRT shader. */
                            render_ctx.crt_shader_enabled = composite_enabled;
                            preset_ctx.display_bypass = 0;
                            printf("Composite: %s (CRT auto %s)\n",
                                   composite_enabled ? "ON" : "OFF (raw RGB)",
                                   render_ctx.crt_shader_enabled ? "on" : "off");
                        }
                    }
                    /* F11 or F: toggle fullscreen + hide cursor. */
                    if (ev.key.scancode == SDL_SCANCODE_F11 ||
                        ev.key.scancode == SDL_SCANCODE_F) {
                        Uint32 flags = SDL_GetWindowFlags(window);
                        if (flags & SDL_WINDOW_FULLSCREEN) {
                            SDL_SetWindowFullscreen(window, false);
                            SDL_ShowCursor();
                        } else {
                            SDL_SetWindowFullscreen(window, true);
                            SDL_HideCursor();
                        }
                    }
                    /* D: dump GPU pipeline output as PPM for debugging. */
                    if (ev.key.scancode == SDL_SCANCODE_D) {
                        if (gpu_rgb_out && gpu_video_enabled &&
                            gpu_buffer_download(gpu, video_gpu_chain.buf_rgb, gpu_rgb_out,
                                                video_gpu_chain.rgb_size)) {
                            int spl = sig_state.samples_per_line;
                            dump_frame_ppm("/tmp/gpu_rgb_out.ppm", gpu_rgb_out, spl, 240);
                            /* Print actual float values at key positions. */
                            printf("RGB float samples (scanline 120):\n");
                            int sl = 120 * spl;
                            for (int x = 0; x < spl; x += spl/8) {
                                float r = gpu_rgb_out[(sl+x)*3+0];
                                float g = gpu_rgb_out[(sl+x)*3+1];
                                float b = gpu_rgb_out[(sl+x)*3+2];
                                printf("  [%d] R=%.4f G=%.4f B=%.4f\n", x, r, g, b);
                            }
                            /* Print waveform float values. */
                            printf("Waveform float (scanline 120, first 16):\n  ");
                            for (int x = 0; x < 16; x++)
                                printf("%.3f ", waveform_buf[sl + x]);
                            printf("\n");
                            /* Check signal table. */
                            int nz_tab = 0;
                            for (int t = 0; t < SIG_TABLE_ENTRIES * SIG_TABLE_STRIDE; t++)
                                if (((float*)sig_state.table)[t] != 0.0f) nz_tab++;
                            printf("Signal table: %d/%d non-zero\n", nz_tab,
                                   SIG_TABLE_ENTRIES * SIG_TABLE_STRIDE);
                            /* Check index framebuffer. */
                            int nz_idx = 0;
                            for (int t = 0; t < 256*240; t++)
                                if (nes.ppu.index_framebuffer[t] != 0) nz_idx++;
                            printf("Index FB: %d/%d non-zero\n", nz_idx, 256*240);
                            /* Try generating waveform RIGHT NOW and check. */
                            waveform_generate(waveform_buf, nes.ppu.index_framebuffer,
                                              &sig_state, frame_count);
                            printf("After fresh generate, waveform[120*spl..+16]:\n  ");
                            for (int x = 0; x < 16; x++)
                                printf("%.3f ", waveform_buf[sl + x]);
                            printf("\n");
                            /* Also dump the raw waveform as a greyscale image. */
                            if (waveform_buf) {
                                float *wf_rgb = (float *)malloc(spl * 240 * 3 * sizeof(float));
                                if (wf_rgb) {
                                    for (int i = 0; i < spl * 240; i++) {
                                        float v = waveform_buf[i];
                                        wf_rgb[i*3+0] = v;
                                        wf_rgb[i*3+1] = v;
                                        wf_rgb[i*3+2] = v;
                                    }
                                    dump_frame_ppm("/tmp/gpu_waveform.ppm", wf_rgb, spl, 240);
                                    free(wf_rgb);
                                }
                            }
                        } else {
                            printf("No GPU output to dump (composite disabled?)\n");
                        }
                    }
                    /* B: toggle temporal blend (dot crawl cancel). */
                    if (ev.key.scancode == SDL_SCANCODE_B) {
                        if (video_gpu_chain.temporal_blend < 0.01f) {
                            video_gpu_chain.temporal_blend = 0.5f;
                            printf("Temporal blend: ON (dot crawl cancelled)\n");
                        } else {
                            video_gpu_chain.temporal_blend = 0.0f;
                            printf("Temporal blend: OFF (dot crawl visible)\n");
                        }
                    }
                    /* F5: screenshot — two-phase NTSC blend + HDR float16. */
                    if (ev.key.scancode == SDL_SCANCODE_F5) {
                        int bw, bh;
                        SDL_GPUTexture *btex = video_gpu_get_beam_texture(&video_gpu_chain);
                        if (btex && video_gpu_get_beam_size(&video_gpu_chain, &bw, &bh)) {
                            /* Run a second frame to get the opposite NTSC phase. */
                            float old_blend = video_gpu_chain.temporal_blend;
                            video_gpu_chain.temporal_blend = 0.5f;

                            /* Re-process current frame with blend=0.5
                             * (prev buffer already has the previous phase). */
                            waveform_generate(waveform_buf, nes.ppu.index_framebuffer,
                                              &sig_state, frame_count);
                            {
                                int field_ph = signal_frame_phase(&sig_state, frame_count);
                                float bp = (float)(sig_state.phase_base + field_ph + sig_state.demod_rotate)
                                           * (2.0f * (float)M_PI / 12.0f);
                                video_gpu_chain.signal_phase_base = sig_state.phase_base + field_ph;
                video_gpu_chain.signal_line_phase = sig_state.phase_line_adv;
                video_gpu_chain.demod_line_phase = sig_state.phase_line_adv * (2.0f * (float)M_PI / 12.0f);
                video_gpu_set_demod(&video_gpu_chain, bp, video_gpu_chain.demod_dp);
                            }
                            video_gpu_process(&video_gpu_chain, gpu, waveform_buf, NULL);

                            /* Download the blended beam (float16). */
                            int beam_bytes = bw * bh * 8;
                            uint8_t *shot = (uint8_t *)malloc(beam_bytes);
                            if (shot && video_gpu_download_beam(&video_gpu_chain, gpu, shot)) {
                                /* Convert float16 to 16-bit PNG via PPM (lossless). */
                                char path[256];
                                snprintf(path, sizeof(path), "/tmp/nes_screenshot_%u.ppm", frame_count);
                                FILE *fp = fopen(path, "wb");
                                if (fp) {
                                    /* Output at 4:3 aspect: scale Y to match.
                                     * Beam is bw × bh where bh = 240*rps. Target: bw × (bw * 3/4). */
                                    int out_w = bw;
                                    int out_h = bw * 3 / 4;  /* 4:3 aspect */
                                    fprintf(fp, "P6\n%d %d\n255\n", out_w, out_h);
                                    for (int oy = 0; oy < out_h; oy++) {
                                        /* Map output Y to beam Y. */
                                        int sy = (int)((float)oy / (float)out_h * (float)bh);
                                        if (sy >= bh) sy = bh - 1;
                                        for (int ox = 0; ox < out_w; ox++) {
                                            int idx = sy * bw + ox;
                                            uint32_t rg, ba;
                                            memcpy(&rg, shot + idx * 8, 4);
                                            memcpy(&ba, shot + idx * 8 + 4, 4);
                                            uint16_t rh = rg & 0xFFFF, gh = (rg >> 16) & 0xFFFF;
                                            uint16_t bh_val = ba & 0xFFFF;
                                            float rf = gpu_half_to_float(rh);
                                            float gf = gpu_half_to_float(gh);
                                            float bf = gpu_half_to_float(bh_val);
                                            /* Reinhard tone map: x/(1+x) for soft HDR rolloff. */
                                            if (rf > 0) rf = rf / (1.0f + rf);
                                            if (gf > 0) gf = gf / (1.0f + gf);
                                            if (bf > 0) bf = bf / (1.0f + bf);
                                            uint8_t px[3] = {
                                                (uint8_t)fminf(rf * 255.0f, 255),
                                                (uint8_t)fminf(gf * 255.0f, 255),
                                                (uint8_t)fminf(bf * 255.0f, 255)
                                            };
                                            fwrite(px, 3, 1, fp);
                                        }
                                    }
                                    fclose(fp);
                                    printf("Screenshot: %s (%dx%d, 16-bit PPM, NTSC blended)\n", path, bw, bh);
                                }
                            }
                            free(shot);
                            video_gpu_chain.temporal_blend = old_blend;
                        }
                    }
                    /* P: cycle presets (scanned from presets/). */
                    if (ev.key.scancode == SDL_SCANCODE_P) {
                        int n = preset_total_count();
                        if (n > 0) {
                            int next = (current_preset + 1) % n;
                            preset_load_index(next);
                        }
                    }
                    /* T: cycle test signal patterns. */
                    if (ev.key.scancode == SDL_SCANCODE_T) {
                        test_signal_mode = (test_signal_mode + 1) % 3;
                        const char *names[] = {"NES", "Color Bars", "Sine Sweep"};
                        printf("Test signal: %s\n", names[test_signal_mode]);
                        break;
                    }
                    /* A: toggle between CPU and GPU audio. */
                    if (ev.key.scancode == SDL_SCANCODE_A) {
                        if (gpu_audio_enabled) {
                            use_gpu_audio = !use_gpu_audio;
                            printf("Audio: %s\n",
                                   use_gpu_audio ? "GPU chain" : "CPU chain");
                        }
                        break;
                    }
                    /* V: toggle performance overlay. */
                    if (ev.key.scancode == SDL_SCANCODE_V) {
                        perf_overlay = !perf_overlay;
                        printf("Perf overlay: %s\n", perf_overlay ? "ON" : "OFF");
                        break;
                    }
                    handle_key(ev.key.scancode, true);
                    break;

                case SDL_EVENT_KEY_UP:
                    handle_key(ev.key.scancode, false);
                    break;

                case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
                    if (gpu_video_enabled && video_gpu_chain.beam_out_w > 0) {
                        int w = ev.window.data1, h = ev.window.data2;
                        if (w * 3 > h * 4) w = h * 4 / 3;
                        else h = w * 3 / 4;
                        if (w > 0 && h > 0 && (w != video_gpu_chain.beam_out_w || h != video_gpu_chain.beam_out_h)) {
                            // No stale texture pointer may survive the beam allocation change.
                            if (!render_ctx.owns_display_tex) render_ctx.display_tex = NULL;
                            if (!video_gpu_set_beam_params(&video_gpu_chain, gpu, w, h,
                                    h / 240 > 0 ? h / 240 : 1,
                                    video_gpu_chain.beam_sigma_narrow, video_gpu_chain.beam_sigma_wide))
                                fprintf(stderr, "Could not resize CRT beam: %s\n", SDL_GetError());
                        }
                    }
                    if (gpu_display_enabled) {
                        gpu_display_resize(&gpu_disp, gpu,
                                           ev.window.data1, ev.window.data2);
                    }
                    break;
            }
        }

        /* Advance emulation at the console rate, independently of monitor Hz.
         * A deadline (not a second full-frame sleep after vsync) avoids drift. */
        if (!rom_loaded && !browser_active) { SDL_Delay(10); continue; }
        Uint64 now = SDL_GetTicksNS();
        Uint64 period = (Uint64)(signal_region_frame_ms(preset_ctx.region) * 1000000.0);
        if (!frame_deadline || now > frame_deadline + period * 3) frame_deadline = now;
        if (now < frame_deadline) SDL_DelayPrecise(frame_deadline - now);
        frame_deadline += period;

        /* --- Audio sync: correct device clock drift before producing a block --- */
        bool play_audio = rom_loaded && !browser_active && !static_frame_buf;
        if (play_audio != audio_playing) {
            audio_reset();
            audio_playing = play_audio;
        }
        if (play_audio) audio_adjust_rate(period / 1e9);
        // The frontend owns the analog chain; retain core anti-alias FIR and
        // DAC mixing but avoid applying the console filters a second time.
        nes.apu.filter_config = (APUFilterConfig){1.0, 1.0, 1.0};
        nes.apu.sample_rate = AUDIO_STREAM_RATE;

        /* --- Run one NES frame (skip when the browser is the source).
         * The browser writes directly into the PPU framebuffer below,
         * then rides the same composite + CRT pipeline a real game would. */
        Uint64 t_emu0 = SDL_GetPerformanceCounter();
        if (static_frame_buf) {
            /* Static-frame mode: paint a fixed palette-index framebuffer
             * into the PPU every frame, bypass CPU/PPU emulation so the
             * composite/GPU chain runs against a known reference image. */
            const uint8_t (*pal)[3] = nes.ppu.color_palette
                                      ? nes.ppu.color_palette
                                      : ppu_palette_2c02;
            for (int i = 0; i < 256 * 240; i++) {
                uint8_t idx = static_frame_buf[i] & 0x3F;
                nes.ppu.index_framebuffer[i] = (uint16_t)idx;
                nes.ppu.framebuffer[i*3+0] = pal[idx][0];
                nes.ppu.framebuffer[i*3+1] = pal[idx][1];
                nes.ppu.framebuffer[i*3+2] = pal[idx][2];
            }
        } else if (rom_loaded && !browser_active) {
            nes_run_frame(&nes);
        }
        /* Recover this picture's line-zero clock from the PPU position at
         * VBlank. Actual skipped dots are already reflected in its clock. */
        sig_state.frame_phase_override = -1;
        if (rom_loaded && !browser_active && !static_frame_buf) {
            uint64_t dots = nes.ppu.next_dot_master_tick / 4;
            uint64_t position = (uint64_t)nes.ppu.scanline * 341 + nes.ppu.dot;
            if (dots >= position)
                sig_state.frame_phase_override = (int)(((dots - position) % 12)
                                                       * sig_state.samples_per_pixel % 12);
        }
        Uint64 t_emu1 = SDL_GetPerformanceCounter();

        if (play_audio) audio_submit_frame();
        else audio_frame_pos = 0;

        /* --- Overlays (before signal processing) --- */
        preset_composite_overlays(&preset_ctx);

        /* ROM browser overlay — fullscreen, supersedes everything else. */
        if (browser_active) {
            const uint8_t (*pal)[3] = nes.ppu.color_palette
                                      ? nes.ppu.color_palette
                                      : ppu_palette_2c02;
            browser_render(&browser, nes.ppu.framebuffer,
                           nes.ppu.index_framebuffer, pal);
        }

        /* OSD menu overlay (must be in main.c — osd.h state is per-TU static). */
        if (osd_menu_is_open && !browser_active) {
            const uint8_t (*pal)[3] = nes.ppu.color_palette
                                      ? nes.ppu.color_palette
                                      : ppu_palette_2c02;
            osd_menu_render_nes(nes.ppu.framebuffer,
                                nes.ppu.index_framebuffer, pal);
        }

        /* Performance overlay (V key) — drawn into NES framebuffer so it
         * gets the NTSC composite treatment like the OSD menu. */
        if (perf_overlay && perf_text[0]) {
            const uint8_t (*pal)[3] = nes.ppu.color_palette
                                      ? nes.ppu.color_palette
                                      : ppu_palette_2c02;
            OSDNesFB t;
            t.rgb = nes.ppu.framebuffer;
            t.idx = nes.ppu.index_framebuffer;
            t.pal = pal;

            int tw = osd_nesfb_text_width(perf_text, 1);
            int pw = tw + 8, ph = 11;
            int px = (256 - pw) / 2;
            int py = 240 - ph - 6;

            osd_nesfb_dim_rect(&t, px, py, pw, ph, 2);
            const uint8_t COL_BORDER = 0x2C;  /* cyan */
            osd_nesfb_fill(&t, px,          py,          pw, 1, COL_BORDER);
            osd_nesfb_fill(&t, px,          py + ph - 1, pw, 1, COL_BORDER);
            osd_nesfb_fill(&t, px,          py,          1,  ph, COL_BORDER);
            osd_nesfb_fill(&t, px + pw - 1, py,          1,  ph, COL_BORDER);
            osd_nesfb_text(&t, px + 4, py + 2, perf_text, 0x30, 1);
        }

        /* --- Video output --- */
        Uint64 t_gpu0 = SDL_GetPerformanceCounter(), t_gpu1 = t_gpu0;
        render_ctx.frame_brightness =
            gpu_render_compute_frame_brightness(nes.ppu.framebuffer);
        render_ctx.raw_ppu_rgb = nes.ppu.framebuffer;
        extern float g_audio_bass_rms;
        render_ctx.audio_bass_rms = g_audio_bass_rms;
        gpu_render_update_dynamic_state(&render_ctx);

        bool signal_decode_active = composite_enabled
                                 && gpu_video_enabled
                                 && video_connection_uses_signal_decode(video_chain.connection);
        bool frame_advanced = false;

        if (signal_decode_active) {
            /* Generate the DAC waveform on GPU when no CPU-only edge effects
             * or waveform diagnostics are requested. Both paths use PPU codes. */
            const TVDisplayParams *tv = &video_chain.tv;
            bool gpu_dac = test_signal_mode == 0 &&
                video_gpu_chain.pipe_dac.pipeline && video_gpu_chain.buf_signal_table &&
                (video_chain.connection == VIDEO_CONN_SVIDEO || (!debug_dump &&
                 tv->beam_edge_fade == 0 && tv->beam_edge_overshoot == 0 &&
                 (tv->burst_lock_drift == 0 || tv->burst_lock_drift_width == 0) &&
                 tv->beam_current_load == 0));
            if (test_signal_mode == 0 && (!gpu_dac || debug_dump)) {
                waveform_generate(waveform_buf, nes.ppu.index_framebuffer,
                                  &sig_state, frame_count);
                waveform_apply_beam_edges(waveform_buf,
                                          nes.ppu.index_framebuffer,
                                          &sig_state, &video_chain.tv,
                                          frame_count);
            } else if (test_signal_mode != 0) {
                int spl_ts = sig_state.samples_per_line;
                float fsc = signal_region_subcarrier_hz(sig_state.region);
                float fs = signal_region_sample_rate_hz(sig_state.region);
                float dp = 2.0f * (float)M_PI * fsc / fs;
                if (test_signal_mode == 1) {
                    test_signal_color_bars(waveform_buf, spl_ts, 240, dp);
                } else {
                    /* Sweep from DC to Nyquist across the full frame. */
                    test_signal_sweep(waveform_buf, spl_ts * 240,
                                      0.0f, 0.5f, 0.5f);
                }
            }
            frame_count++;
            frame_advanced = true;

            /* Update demod base phase to match this frame's waveform phase.
             * The waveform generator uses: field_ph = (fc % num_fields) * phase_field_adv
             * base_phase = (base_ph + field_ph) * (2π/12).
             * The demod must start at the same phase. */
            {
                int field_ph = signal_frame_phase(&sig_state, frame_count - 1);
                float base_phase = (float)(sig_state.phase_base + field_ph + sig_state.demod_rotate)
                                   * (2.0f * (float)M_PI / 12.0f);
                video_gpu_chain.signal_phase_base = sig_state.phase_base + field_ph;
                video_gpu_chain.signal_line_phase = sig_state.phase_line_adv;
                video_gpu_chain.demod_line_phase = sig_state.phase_line_adv * (2.0f * (float)M_PI / 12.0f);
                video_gpu_set_demod(&video_gpu_chain, base_phase, video_gpu_chain.demod_dp);
            }

            int spl = sig_state.samples_per_line;
            int out_w = spl;
            int out_h = 240;

            bool dump_this_frame = debug_dump &&
                frame_wanted(frame_count, debug_dump_frames, debug_dump_count);

            /* Pre-process waveform snapshot. */
            if (dump_this_frame) {
                int _spl = sig_state.samples_per_line;
                int _sl = 120 * _spl;
                printf("[dump frame %u] waveform scanline 120:\n", frame_count);
                printf("  [0..7]: %.4f %.4f %.4f %.4f %.4f %.4f %.4f %.4f\n",
                       waveform_buf[_sl], waveform_buf[_sl+1], waveform_buf[_sl+2], waveform_buf[_sl+3],
                       waveform_buf[_sl+4], waveform_buf[_sl+5], waveform_buf[_sl+6], waveform_buf[_sl+7]);
                fflush(stdout);
            }

            video_gpu_set_dynamic_state(&video_gpu_chain,
                                        render_ctx.hv_sag_state,
                                        render_ctx.audio_bass_rms);
            t_gpu0 = SDL_GetPerformanceCounter();
            float *readback = (dump_this_frame || (screenshot_after > 0 && frame_count >= (unsigned)screenshot_after) ||
                !video_gpu_get_beam_texture(&video_gpu_chain)) ? gpu_rgb_out : NULL;
            bool gpu_ok = gpu_dac
                ? video_gpu_process_full(&video_gpu_chain, gpu, nes.ppu.index_framebuffer,
                    sig_state.phase_base + signal_frame_phase(&sig_state, frame_count - 1),
                    sig_state.phase_line_adv, 0, readback)
                : video_gpu_process(&video_gpu_chain, gpu, waveform_buf, readback);
            t_gpu1 = SDL_GetPerformanceCounter();
            if (dump_this_frame) {
                printf("[dump frame %u] video_gpu_process: %s\n",
                       frame_count, gpu_ok ? "OK" : "FAILED");
                fflush(stdout);
            }
            if (gpu_ok) {
                /* Full RGB + I/Q + PPM snapshot on dumped frames. */
                if (dump_this_frame) {
                    /* Download I and Q aux buffers to check chroma. */
                    SignalChain *_sc = &video_gpu_chain.sig_chain;
                    int _ts = sig_state.samples_per_line * 240;
                    float *_i_buf = (float *)malloc(_ts * sizeof(float));
                    float *_q_buf = (float *)malloc(_ts * sizeof(float));
                    if (_i_buf && _q_buf) {
                        int i_idx = 2, q_idx = 3; /* aux[2]=filtered I, aux[3]=filtered Q */
                        gpu_buffer_download(gpu, _sc->aux[i_idx], _i_buf, _ts * sizeof(float));
                        gpu_buffer_download(gpu, _sc->aux[q_idx], _q_buf, _ts * sizeof(float));
                        int _sl = 120 * sig_state.samples_per_line;
                        printf("Chroma I[120,0..7]: %.4f %.4f %.4f %.4f %.4f %.4f %.4f %.4f\n",
                               _i_buf[_sl], _i_buf[_sl+1], _i_buf[_sl+2], _i_buf[_sl+3],
                               _i_buf[_sl+4], _i_buf[_sl+5], _i_buf[_sl+6], _i_buf[_sl+7]);
                        printf("Chroma Q[120,0..7]: %.4f %.4f %.4f %.4f %.4f %.4f %.4f %.4f\n",
                               _q_buf[_sl], _q_buf[_sl+1], _q_buf[_sl+2], _q_buf[_sl+3],
                               _q_buf[_sl+4], _q_buf[_sl+5], _q_buf[_sl+6], _q_buf[_sl+7]);
                        /* Check if all zero. */
                        int i_nz = 0, q_nz = 0;
                        for (int k = 0; k < _ts; k++) {
                            if (_i_buf[k] != 0.0f) i_nz++;
                            if (_q_buf[k] != 0.0f) q_nz++;
                        }
                        printf("Chroma I non-zero: %d/%d, Q non-zero: %d/%d\n", i_nz, _ts, q_nz, _ts);
                        free(_i_buf);
                        free(_q_buf);
                    }
                    int spl = sig_state.samples_per_line;
                    int sl = 120 * spl;
                    printf("  RGB[0]:    R=%.4f G=%.4f B=%.4f\n",
                           gpu_rgb_out[sl*3], gpu_rgb_out[sl*3+1], gpu_rgb_out[sl*3+2]);
                    printf("  RGB[128]:  R=%.4f G=%.4f B=%.4f\n",
                           gpu_rgb_out[(sl+128)*3], gpu_rgb_out[(sl+128)*3+1], gpu_rgb_out[(sl+128)*3+2]);
                    printf("  RGB[1024]: R=%.4f G=%.4f B=%.4f\n",
                           gpu_rgb_out[(sl+1024)*3], gpu_rgb_out[(sl+1024)*3+1], gpu_rgb_out[(sl+1024)*3+2]);
                    char ppm_rgb[256], ppm_wf[256];
                    snprintf(ppm_rgb, sizeof(ppm_rgb), "/tmp/gpu_rgb_f%u.ppm", frame_count);
                    snprintf(ppm_wf,  sizeof(ppm_wf),  "/tmp/gpu_waveform_f%u.ppm", frame_count);
                    dump_frame_ppm(ppm_rgb, gpu_rgb_out, spl, 240);
                    dump_frame_ppm(ppm_wf,  waveform_buf, spl, 240);
                    printf("[dump frame %u] wrote %s, %s\n", frame_count, ppm_rgb, ppm_wf);
                }
                /* Use beam profile (RGBA8 with intensity-dependent scanline
                 * structure) if configured, otherwise flat RGB conversion. */
                int beam_w, beam_h;
                SDL_GPUTexture *beam_tex = video_gpu_get_beam_texture(&video_gpu_chain);
                if (beam_tex &&
                    video_gpu_get_beam_size(&video_gpu_chain, &beam_w, &beam_h)) {
                    /* Zero-copy: temporal_blit shader wrote directly to this
                     * storage texture. No download, no CPU blend, no upload. */
                    render_ctx.display_tex = beam_tex;
                    render_ctx.display_tex_w = beam_w;
                    render_ctx.display_tex_h = beam_h;
                    render_ctx.owns_display_tex = false;
                } else {
                    gpu_render_ensure_texture(&render_ctx, out_w, out_h);
                    uint8_t *rgba = gpu_render_float_rgb_to_rgba8(gpu_rgb_out,
                                                        out_w * out_h);
                    gpu_render_upload_rgba(&render_ctx, rgba, out_w, out_h);
                }
            } else {
                /* GPU dispatch failed: fall back to raw RGB. */
                gpu_render_ensure_texture(&render_ctx, 256, 240);
                gpu_render_upload_rgba(&render_ctx,
                    gpu_render_ppu_to_rgba8(nes.ppu.framebuffer), 256, 240);
            }
        } else if (composite_enabled) {
            /* Either GPU video is unavailable, or this preset's
             * connection type is source-domain RGB/Direct and should
             * bypass the composite decoder entirely. */
            gpu_render_ensure_texture(&render_ctx, 256, 240);
            gpu_render_upload_rgba(&render_ctx,
                gpu_render_ppu_to_rgba8(nes.ppu.framebuffer), 256, 240);
        } else {
            /* Raw RGB path: palette LUT output, no composite. */
            gpu_render_ensure_texture(&render_ctx, 256, 240);
            gpu_render_upload_rgba(&render_ctx,
                gpu_render_ppu_to_rgba8(nes.ppu.framebuffer), 256, 240);
        }

        if (!frame_advanced) {
            frame_count++;
        }

        /* --- Capture debug tap snapshots (GPU buffer readback for visualiser) --- */
        if (signal_decode_active && tap_mgr && debug_tap_enabled_count(tap_mgr) > 0) {
            debug_tap_capture(tap_mgr, gpu, frame_count);
        }

        Uint64 t_render0 = SDL_GetPerformanceCounter();
        gpu_render_frame(&render_ctx, &video_chain);
        Uint64 t_render1 = SDL_GetPerformanceCounter();

        /* --- Screenshot + exit hook ---
         * Dumps the beam-profile output (bw × bh) scaled to 4:3 aspect
         * so the PPM looks like what the user sees on screen, instead
         * of the raw 2048×240 signal-resolution RGB which has wrong
         * aspect and height. Falls back to the raw signal if the beam
         * output isn't available (GPU beam stage not initialised). */
        if (screenshot_after > 0 && frame_count >= (unsigned)screenshot_after) {
            int bw, bh;
            if (gpu_video_enabled &&
                video_gpu_get_beam_size(&video_gpu_chain, &bw, &bh) &&
                bw > 0 && bh > 0) {
                int beam_bytes = bw * bh * 8;   /* float16x4 = 8 bytes/px */
                uint8_t *shot = (uint8_t *)malloc(beam_bytes);
                if (shot && video_gpu_download_beam(&video_gpu_chain, gpu, shot)) {
                    int out_w = bw;
                    int out_h = bw * 3 / 4;   /* scale to 4:3 */
                    FILE *fp = fopen(screenshot_path, "wb");
                    if (fp) {
                        fprintf(fp, "P6\n%d %d\n255\n", out_w, out_h);
                        for (int oy = 0; oy < out_h; oy++) {
                            int sy = (int)((float)oy / (float)out_h * (float)bh);
                            if (sy >= bh) sy = bh - 1;
                            for (int ox = 0; ox < out_w; ox++) {
                                int idx = sy * bw + ox;
                                uint32_t rg, ba;
                                memcpy(&rg, shot + idx * 8, 4);
                                memcpy(&ba, shot + idx * 8 + 4, 4);
                                uint16_t rh = rg & 0xFFFF, gh = (rg >> 16) & 0xFFFF;
                                uint16_t bh_val = ba & 0xFFFF;
                                float rf = gpu_half_to_float(rh);
                                float gf = gpu_half_to_float(gh);
                                float bf = gpu_half_to_float(bh_val);
                                /* Reinhard tone map for HDR→SDR rolloff. */
                                if (rf > 0) rf = rf / (1.0f + rf);
                                if (gf > 0) gf = gf / (1.0f + gf);
                                if (bf > 0) bf = bf / (1.0f + bf);
                                uint8_t px[3] = {
                                    (uint8_t)(rf < 0 ? 0 : (rf > 1 ? 255 : rf * 255)),
                                    (uint8_t)(gf < 0 ? 0 : (gf > 1 ? 255 : gf * 255)),
                                    (uint8_t)(bf < 0 ? 0 : (bf > 1 ? 255 : bf * 255))
                                };
                                fwrite(px, 1, 3, fp);
                            }
                        }
                        fclose(fp);
                        fprintf(stderr, "Captured frame %u to %s (%dx%d, 4:3 from beam)\n",
                                frame_count, screenshot_path, out_w, out_h);
                    }
                }
                free(shot);
            } else {
                /* Fallback: raw signal dump. */
                int spl = sig_state.samples_per_line;
                dump_frame_ppm(screenshot_path, gpu_rgb_out, spl, 240);
                fprintf(stderr, "Captured frame %u to %s (%dx%d raw signal)\n",
                        frame_count, screenshot_path, spl, 240);
            }
            running = false;
        }

        /* --- Send frame snapshot to debug server (visualiser) --- */
        if (debug_srv && debug_server_has_client(debug_srv) && gpu_video_enabled) {
            debug_server_frame(debug_srv, &video_gpu_chain.sig_chain, NULL, frame_count);
        }

        /* --- Performance stats accumulation --- */
        {
            static Uint64 perf_emu_total = 0, perf_gpu_total = 0, perf_render_total = 0;
            static int perf_frames = 0;
            static Uint64 perf_last_print = 0;

            perf_emu_total += t_emu1 - t_emu0;
            if (composite_enabled && gpu_video_enabled)
                perf_gpu_total += t_gpu1 - t_gpu0;
            perf_render_total += t_render1 - t_render0;
            perf_frames++;

            Uint64 freq = SDL_GetPerformanceFrequency();
            if (!perf_last_print) perf_last_print = t_render1;
            if (t_render1 - perf_last_print >= freq) {
                double emu_ms = (double)perf_emu_total * 1000.0 / (double)freq / perf_frames;
                double gpu_ms = (double)perf_gpu_total * 1000.0 / (double)freq / perf_frames;
                double ren_ms = (double)perf_render_total * 1000.0 / (double)freq / perf_frames;
                double tot_ms = emu_ms + gpu_ms + ren_ms;
                snprintf(perf_text, sizeof(perf_text),
                         "EMU %.1f GPU %.1f REN %.1f TOT %.1f",
                         emu_ms, gpu_ms, ren_ms, tot_ms);
                perf_emu_total = perf_gpu_total = perf_render_total = 0;
                perf_frames = 0;
                perf_last_print = t_render1;
            }
        }
    }

    /* ========================================================================
     * Cleanup
     * ======================================================================== */

    if (audio_capture) fclose(audio_capture);
    if (audio_trace) fclose(audio_trace);
    if (debug_srv) debug_server_destroy(debug_srv);
    if (tap_mgr) debug_tap_destroy(tap_mgr);
    chain_vis_destroy(chain_vis);
    if (gpu_display_enabled) gpu_display_destroy(&gpu_disp, gpu);
    free(waveform_buf);
    free(gpu_rgb_out);
    if (gpu_video_enabled) video_gpu_destroy(&video_gpu_chain, gpu);
    if (gpu_audio_enabled) audio_gpu_destroy(&audio_gpu, gpu);
    if (render_ctx.display_tex && render_ctx.owns_display_tex)
        SDL_ReleaseGPUTexture(gpu, render_ctx.display_tex);
    nes_rom_free(&rom);
    if (audio_stream) SDL_DestroyAudioStream(audio_stream);
    SDL_ReleaseWindowFromGPUDevice(gpu, window);
    SDL_DestroyWindow(window);
    SDL_DestroyGPUDevice(gpu);
    SDL_Quit();
    return 0;
}
