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
#ifndef _WIN32
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>
#endif

/* NES core. */
#include "nes/nes.h"
#include "ppu/ppu.h"
#include "nes/apu.h"
#include "nes/rom.h"
#include "nes/mapper.h"
#include "nes/osd.h"
#include "nes/state.h"

/* GPU signal chain (NO composite.h). */
#include "signal_precompute.h"
#include "video_gpu.h"
#include "gpu_display.h"
#include "audio_chain.h"
#include "audio_gpu.h"
#include "playback.h"
#include "video_chain.h"
#include "presets.h"
#include "chain_vis.h"
#include "dump_frame.h"
#include "debug_tap.h"
#include "debug_server.h"

/* Extracted modules. */
#include "gpu_render.h"
#include "gpu_benchmark.h"
#include "preset_apply.h"
#include "waveform_gen.h"
#include "test_signals.h"
#include "gpu_log.h"
#include "gpu_osd.h"
#include "recorder.h"

/* Shared frontend helpers. */
#include "browser.h"
#include "config.h"
#include "saves.h"

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

/* Rendering owns a picture copy; NES and audio belong to the playback worker. */
static Playback *playback;
static PlaybackFrame picture;
static PPU display_ppu;
static APUAnalog analog_controls;
static bool playback_active;

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

/* Controllers: keyboard masks per player plus up to two hot-plugged
 * gamepads. The first pad to arrive is player 1, the next player 2; a
 * removed pad frees its slot for the next arrival. */
typedef struct {
    SDL_Gamepad   *pad;
    SDL_JoystickID id;
    uint8_t        buttons;      /* d-pad and face buttons */
    uint8_t        stick;        /* left stick past the deadzone */
    bool           fast_forward; /* right shoulder held */
    char           name[48];
} GamepadSlot;
static uint8_t          keyboard_buttons[2];
static GamepadSlot      gamepads[2];
static bool             key_fast_forward;    /* Backquote held */
static bool             paused;              /* Space / OSD Game menu */

/* Battery RAM and save states (frontends/shared/saves.h). While the worker
 * runs, the console is only touched through playback_with_console. */
#define BATTERY_FLUSH_MS 2000
static MynesSaves       saves;
static int              state_slot = 1;      /* 1..MYNES_STATE_SLOTS, as shown to the player */
static bool             state_save_requested, state_load_requested, battery_write_requested;
static uint8_t          battery_image[MYNES_PRG_RAM_SIZE];
static Uint64           battery_next_check;
static unsigned         state_load_frame;    /* frames emulated when the last state was applied */

/* --input-record: player-1 mask changes as replay rows, numbered from the
 * frame after the most recent console start, reset or state load. */
static InputRecord      input_record;
static unsigned         input_record_base;

/* Render and preset contexts. */
static GPURenderCtx     render_ctx;
static PresetCtx        preset_ctx;

/* Internal CRT resolution. The beam and phosphor stages render at the tube
 * viewport size times this factor and the display pass upsamples; the mask
 * is still sampled at panel pitch. Auto starts at full size and only ever
 * steps down. Offscreen captures and benchmarks pin the requested size. */
enum { RENDER_SCALE_AUTO, RENDER_SCALE_FULL, RENDER_SCALE_3_4, RENDER_SCALE_HALF };
static const float      render_scale_factor[] = { 1.0f, 1.0f, 0.75f, 0.5f };
static const char *const render_scale_name[] = { "auto", "1.0", "0.75", "0.5" };
static int              render_scale_mode;      /* OSD cyclic target, persisted */
static int              render_scale_auto_level = RENDER_SCALE_FULL;
static int              render_scale_over_windows;
static bool             render_scale_fixed;     /* offscreen / benchmark */
static int              render_scale_settle;    /* timing windows Auto ignores */
static int              low_latency;            /* OSD toggle target, persisted */

static float render_scale_effective(void) {
    if (render_scale_fixed) return 1.0f;
    return render_scale_factor[render_scale_mode == RENDER_SCALE_AUTO
                               ? render_scale_auto_level : render_scale_mode];
}

/* Fit the tube aspect into the drawable, then apply the render scale. Two
 * device rows per scanline is the floor below which scanline structure
 * cannot be represented; the drawable fit itself is the ceiling. */
static void beam_target_size(int win_w, int win_h, int aspect_w, int aspect_h,
                             float scale, int *out_w, int *out_h) {
    int w = win_w, h = win_h;
    if (w * aspect_h > h * aspect_w) w = h * aspect_w / aspect_h;
    else h = w * aspect_h / aspect_w;
    int scaled_h = (int)((float)h * scale);
    if (scaled_h < 480) scaled_h = 480;
    if (scaled_h > h) scaled_h = h;
    *out_h = scaled_h;
    *out_w = scaled_h == h ? w : scaled_h * aspect_w / aspect_h;
}

/* Browser + persistent config. Host UI is composited after the receiver. */
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
    case SDL_SCANCODE_HOME:      return BROWSER_KEY_HOME;
    case SDL_SCANCODE_END:       return BROWSER_KEY_END;
    case SDL_SCANCODE_TAB:       return BROWSER_KEY_TAB;
    default:                     return -1;
    }
}

/* Custom 64×3 RGB palette used for the raw-RGB display path (C key).
 * Loaded from a .pal file at startup; falls back to the built-in 2C02
 * palette on failure. */
static uint8_t raw_palette[64][3];
static bool    raw_palette_loaded = false;

/* Resources sit beside bin/ in a build tree or release tarball and in
 * Contents/Resources of a macOS app bundle, where SDL_GetBasePath() is
 * either Contents/MacOS/ or Resources/ itself. The first existing candidate
 * wins; when none exists buf is left naming the conventional ../<sub> so
 * the caller's error message stays meaningful. */
static const char *resource_dir(char *buf, size_t n, const char *base, const char *sub) {
    static const char *const prefixes[] = {"../Resources/", "", "../"};
    for (size_t i = 0; i < sizeof(prefixes) / sizeof(*prefixes); i++) {
        int len = snprintf(buf, n, "%s%s%s", base, prefixes[i], sub);
        if (len > 0 && (size_t)len < n && SDL_GetPathInfo(buf, NULL)) break;
    }
    return buf;
}

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
static bool             osd_parameter_editing;
static uint32_t         osd_pixels[GPU_OSD_PIXELS];
static char             perf_text[128] = "";

/* One transient host notice at a time; a newer one replaces the previous.
 * Any feature can post one (gamepad hot-plug, room reflections, ...). */
static char             notice_title[40], notice_value[96];
static Uint64           notice_until;
static void show_notice(const char *title, const char *value) {
    snprintf(notice_title, sizeof(notice_title), "%s", title);
    snprintf(notice_value, sizeof(notice_value), "%s", value);
    notice_until = SDL_GetTicks() + 2000;
}

static void open_osd_menu(void) {
    osd_parameter_editing = false;
    osd_menu_open_root(preset_menu_root, preset_menu_root_count, "SETUP");
}
static void close_osd_menu(void) {
    osd_parameter_editing = false;
    osd_menu_close();
}

/* OSD Game submenu actions. Pausing from the menu closes it so the paused
 * picture and its notice are what the player sees. */
static void action_toggle_pause(void) {
    if (!rom_loaded) return;
    paused = !paused;
    close_osd_menu();
}
static void action_quit(void) {
    running = false;
}

/* Host display performance settings persist like mask alignment. Choosing
 * Auto again restarts it from full size. */
static void action_render_scale_changed(void) {
    render_scale_auto_level = RENDER_SCALE_FULL;
    render_scale_over_windows = 0;
    mynes_config.gpu_render_scale = render_scale_mode;
    mynes_config_save(&mynes_config);
}
static void action_low_latency_changed(void) {
    mynes_config.gpu_low_latency = low_latency;
    mynes_config_save(&mynes_config);
}

/* Reviews and captures compare frame sequences bit-for-bit; they keep the
 * three-picture FIFO whatever the saved preference says. */
static bool review_env_present(void) {
    static const char *const names[] = {
        "MYNES_REVIEW_FRAME", "MYNES_REVIEW_INPUT_SCRIPT", "MYNES_REVIEW_NO_INPUT",
        "MYNES_REVIEW_OSD", "MYNES_REVIEW_PRESET", "MYNES_REVIEW_SIDE",
        "MYNES_REVIEW_START_FRAME", "MYNES_REVIEW_TITLE" };
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++)
        if (getenv(names[i])) return true;
    return false;
}

/* ============================================================================
 * Battery RAM and save states
 * ============================================================================ */

/* Console visitors for playback_with_console: the worker owns the NES while
 * playback is active, so cartridge RAM and machine state cross over here. */
static void console_copy_prg_ram(NES *console, void *user) {
    memcpy(user, console->mapper.prg_ram, MYNES_PRG_RAM_SIZE);
}
static void console_restore_prg_ram(NES *console, void *user) {
    memcpy(console->mapper.prg_ram, user, MYNES_PRG_RAM_SIZE);
}
typedef struct {
    void  *data;
    size_t size;
    bool   ok;
    char   error[96];   /* sized like a notice value, so it shows in full */
} StateJob;
static void console_save_state(NES *console, void *user) {
    StateJob *job = user;
    job->ok = nes_state_save(console, job->data, job->size);
}
static void console_load_state(NES *console, void *user) {
    StateJob *job = user;
    job->ok = nes_state_load(console, job->data, job->size, job->error, sizeof(job->error));
}
/* Until the worker exists (startup, benchmark) the main thread owns the
 * console. Returns the frames emulated when fn ran (0 without a worker). */
static unsigned with_console(void (*fn)(NES *console, void *user), void *user) {
    if (playback) return playback_with_console(playback, fn, user);
    fn(&nes, user);
    return 0;
}

/* Bind the cartridge in `rom` to its save files and restore battery RAM.
 * Runs right after a load, while the mapper's RAM is still blank. */
static void saves_attach(const char *rom_path) {
    uint32_t crc = nes_state_rom_crc(rom.prg_rom, rom.prg_size, rom.chr_rom, rom.chr_size);
    mynes_saves_open(&saves, rom_path, crc, rom.has_battery);
    battery_next_check = SDL_GetTicks() + BATTERY_FLUSH_MS;
    if (!saves.battery) return;
    with_console(console_copy_prg_ram, battery_image);
    if (mynes_saves_restore(&saves, battery_image)) {
        with_console(console_restore_prg_ram, battery_image);
        fprintf(stderr, "Battery RAM restored from %s\n", saves.sav_path);
    } else {
        fprintf(stderr, "Battery RAM will be saved to %s\n", saves.sav_path);
    }
}

/* Write battery RAM when it changed since the last write (always with force). */
static bool saves_flush(bool force) {
    if (!saves.battery || !nes.mapper_loaded) return true;
    with_console(console_copy_prg_ram, battery_image);
    return mynes_saves_flush(&saves, battery_image, force);
}

static void state_save_slot(void) {
    char value[64];
    StateJob job = { .size = nes_state_size(&nes) };
    job.data = malloc(job.size);
    if (job.data) with_console(console_save_state, &job);
    bool written = job.ok && mynes_state_write(&saves, state_slot, job.data, job.size);
    free(job.data);
    if (written) {
        snprintf(value, sizeof(value), "Slot %d", state_slot);
        show_notice("STATE SAVED", value);
        fprintf(stderr, "State saved to slot %d\n", state_slot);
    } else {
        snprintf(value, sizeof(value), "Slot %d: cannot write the state file", state_slot);
        show_notice("SAVE STATE FAILED", value);
        fprintf(stderr, "Save state to slot %d failed\n", state_slot);
    }
}

/* Apply a state image; true when the console now runs it. `source` names
 * where it came from for the notice and the log. Shared by the F7 / OSD
 * slot load and --load-state, so both restart the worker's pictures and
 * audio the same way; the caller resets the renderer's temporal state. */
static bool state_load_image(void *data, size_t size, const char *source) {
    StateJob job = { .data = data, .size = size };
    state_load_frame = with_console(console_load_state, &job);
    if (!job.ok) {
        show_notice("LOAD STATE FAILED", job.error);
        fprintf(stderr, "Load state from %s: %s\n", source, job.error);
        return false;
    }
    if (playback) playback_restart(playback);
    show_notice("STATE LOADED", source);
    fprintf(stderr, "State loaded from %s\n", source);
    return true;
}

/* Returns true when the console now runs the loaded state. */
static bool state_load_slot(void) {
    char value[64];
    size_t size = 0;
    void *data = mynes_state_read(&saves, state_slot, &size);
    if (!data) {
        snprintf(value, sizeof(value), "Slot %d is empty", state_slot);
        show_notice("LOAD STATE FAILED", value);
        fprintf(stderr, "Load state: slot %d is empty\n", state_slot);
        return false;
    }
    snprintf(value, sizeof(value), "Slot %d", state_slot);
    bool loaded = state_load_image(data, size, value);
    free(data);
    return loaded;
}

/* --load-state FILE: a state file from anywhere, not only the slots. */
static bool state_load_file(const char *path) {
    FILE *f = fopen(path, "rb");
    long size = 0;
    void *data = NULL;
    if (f && !fseek(f, 0, SEEK_END) && (size = ftell(f)) > 0 && !fseek(f, 0, SEEK_SET))
        data = malloc((size_t)size);
    bool read = data && fread(data, 1, (size_t)size, f) == (size_t)size;
    if (f) fclose(f);
    if (!read) {
        fprintf(stderr, "Load state from %s: cannot read the file\n", path);
        free(data);
        return false;
    }
    bool loaded = state_load_image(data, (size_t)size, path);
    free(data);
    return loaded;
}

static void cycle_state_slot(void) {
    char value[32];
    state_slot = state_slot % MYNES_STATE_SLOTS + 1;
    snprintf(value, sizeof(value), "Slot %d", state_slot);
    show_notice("STATE SLOT", value);
}

/* OSD Game submenu actions. The menu closes so the notice is what the
 * player sees; the requests are honoured between frames in the main loop. */
static void action_save_state(void) { state_save_requested = true; close_osd_menu(); }
static void action_load_state(void) { state_load_requested = true; close_osd_menu(); }
static void action_write_battery(void) { battery_write_requested = true; close_osd_menu(); }

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

/* ============================================================================
 * Input handling
 * ============================================================================ */

/* F (alone or with Globe), F11 and Alt+Return. Panel-pixel mask alignment
 * asks for the panel's native mode so the desktop scaler never touches the
 * picture. */
static void toggle_fullscreen(void) {
    render_scale_settle = 2;
    if (!gpu_output_toggle_fullscreen(window, render_ctx.mask_alignment == 0))
        fprintf(stderr, "Fullscreen: %s\n", SDL_GetError());
}

/* Batch runs (offscreen, screenshots, recordings, benchmarks) take turns on
 * a lock file: scripts start many at once, and together they fight over the
 * GPU. The lock is held until exit; interactive play never waits.
 * MYNES_BATCH_LOCK names another lock file, or 0 turns the queue off. */
static void batch_lock_wait(void) {
#ifndef _WIN32
    const char *path = getenv("MYNES_BATCH_LOCK");
    if (path && !strcmp(path, "0")) return;
    if (!path || !*path) path = "/tmp/mynes_gpu-batch.lock";
    int fd = open(path, O_RDWR | O_CREAT | O_CLOEXEC, 0666);
    if (fd < 0) return;
    if (flock(fd, LOCK_EX | LOCK_NB) == 0) return;
    fprintf(stderr, "Waiting for another batch run to finish (%s)\n", path);
    if (flock(fd, LOCK_EX) != 0) close(fd);
#endif
}

static void handle_key(SDL_Scancode sc, bool down) {
    uint8_t mask = 0;
    int player = 0;
    switch (sc) {
        case SDL_SCANCODE_X:      mask = 0x01; break;  /* A */
        case SDL_SCANCODE_Z:      mask = 0x02; break;  /* B */
        case SDL_SCANCODE_TAB:    mask = 0x04; break;  /* Select */
        case SDL_SCANCODE_RETURN: mask = 0x08; break;  /* Start */
        case SDL_SCANCODE_UP:     mask = 0x10; break;
        case SDL_SCANCODE_DOWN:   mask = 0x20; break;
        case SDL_SCANCODE_LEFT:   mask = 0x40; break;
        case SDL_SCANCODE_RIGHT:  mask = 0x80; break;
        /* Player 2 sits on the left hand: WASD plus J/H/U/Y. */
        case SDL_SCANCODE_J:      mask = 0x01; player = 1; break;  /* A */
        case SDL_SCANCODE_H:      mask = 0x02; player = 1; break;  /* B */
        case SDL_SCANCODE_U:      mask = 0x04; player = 1; break;  /* Select */
        case SDL_SCANCODE_Y:      mask = 0x08; player = 1; break;  /* Start */
        case SDL_SCANCODE_W:      mask = 0x10; player = 1; break;
        case SDL_SCANCODE_S:      mask = 0x20; player = 1; break;
        case SDL_SCANCODE_A:      mask = 0x40; player = 1; break;
        case SDL_SCANCODE_D:      mask = 0x80; player = 1; break;
        default: return;
    }
    if (down) keyboard_buttons[player] |= mask;
    else      keyboard_buttons[player] &= ~mask;
}

/* ============================================================================
 * Gamepads (RetroArch convention: EAST = A, SOUTH = B)
 * ============================================================================ */

#define GAMEPAD_DEADZONE 16384  /* half of the axis range */

static int gamepad_slot(SDL_JoystickID id) {
    for (int i = 0; i < 2; i++)
        if (gamepads[i].pad && gamepads[i].id == id) return i;
    return -1;
}

static void gamepad_added(SDL_JoystickID id) {
    if (gamepad_slot(id) >= 0) return;
    for (int i = 0; i < 2; i++) {
        if (gamepads[i].pad) continue;
        SDL_Gamepad *pad = SDL_OpenGamepad(id);
        if (!pad) { fprintf(stderr, "Gamepad open failed: %s\n", SDL_GetError()); return; }
        const char *name = SDL_GetGamepadName(pad);
        gamepads[i] = (GamepadSlot){ .pad = pad, .id = id };
        snprintf(gamepads[i].name, sizeof(gamepads[i].name), "%s", name ? name : "Gamepad");
        char value[80];
        snprintf(value, sizeof(value), "%s (P%d)", gamepads[i].name, i + 1);
        show_notice("GAMEPAD CONNECTED", value);
        fprintf(stderr, "Gamepad connected: %s (P%d)\n", gamepads[i].name, i + 1);
        return;
    }
}

static void gamepad_removed(SDL_JoystickID id) {
    int i = gamepad_slot(id);
    if (i < 0) return;
    char value[80];
    snprintf(value, sizeof(value), "%s (P%d)", gamepads[i].name, i + 1);
    show_notice("GAMEPAD REMOVED", value);
    fprintf(stderr, "Gamepad removed: %s (P%d)\n", gamepads[i].name, i + 1);
    SDL_CloseGamepad(gamepads[i].pad);
    gamepads[i] = (GamepadSlot){0};
}

static uint8_t gamepad_button_mask(int button) {
    switch (button) {
        case SDL_GAMEPAD_BUTTON_EAST:
        case SDL_GAMEPAD_BUTTON_NORTH:      return 0x01;  /* A */
        case SDL_GAMEPAD_BUTTON_SOUTH:
        case SDL_GAMEPAD_BUTTON_WEST:       return 0x02;  /* B */
        case SDL_GAMEPAD_BUTTON_BACK:       return 0x04;  /* Select */
        case SDL_GAMEPAD_BUTTON_START:      return 0x08;  /* Start */
        case SDL_GAMEPAD_BUTTON_DPAD_UP:    return 0x10;
        case SDL_GAMEPAD_BUTTON_DPAD_DOWN:  return 0x20;
        case SDL_GAMEPAD_BUTTON_DPAD_LEFT:  return 0x40;
        case SDL_GAMEPAD_BUTTON_DPAD_RIGHT: return 0x80;
        default:                            return 0;
    }
}

/* Menu/browser navigation equivalent of a pad button, or UNKNOWN. */
static SDL_Scancode gamepad_nav_key(int button) {
    switch (button) {
        case SDL_GAMEPAD_BUTTON_DPAD_UP:    return SDL_SCANCODE_UP;
        case SDL_GAMEPAD_BUTTON_DPAD_DOWN:  return SDL_SCANCODE_DOWN;
        case SDL_GAMEPAD_BUTTON_DPAD_LEFT:  return SDL_SCANCODE_LEFT;
        case SDL_GAMEPAD_BUTTON_DPAD_RIGHT: return SDL_SCANCODE_RIGHT;
        case SDL_GAMEPAD_BUTTON_SOUTH:      return SDL_SCANCODE_RETURN;
        case SDL_GAMEPAD_BUTTON_EAST:       return SDL_SCANCODE_ESCAPE;
        default:                            return SDL_SCANCODE_UNKNOWN;
    }
}

/* Direction bits (0x10..0x80) of the left stick after the deadzone. */
static uint8_t gamepad_stick_mask(uint8_t stick, int axis, int value) {
    if (axis == SDL_GAMEPAD_AXIS_LEFTX)
        return (stick & ~0xc0) | (value < -GAMEPAD_DEADZONE ? 0x40 : value > GAMEPAD_DEADZONE ? 0x80 : 0);
    if (axis == SDL_GAMEPAD_AXIS_LEFTY)
        return (stick & ~0x30) | (value < -GAMEPAD_DEADZONE ? 0x10 : value > GAMEPAD_DEADZONE ? 0x20 : 0);
    return stick;
}

static SDL_Scancode direction_nav_key(uint8_t bit) {
    switch (bit) {
        case 0x10: return SDL_SCANCODE_UP;
        case 0x20: return SDL_SCANCODE_DOWN;
        case 0x40: return SDL_SCANCODE_LEFT;
        default:   return SDL_SCANCODE_RIGHT;
    }
}

/* ============================================================================
 * ROM browser and host UI navigation
 * ============================================================================ */

/* Feed one browser key; on selection replace the running console. The
 * static frame buffer belongs to main, so it is handed in by reference. */
static void browser_key(BrowserKey key, bool *console_changed, uint8_t **static_frame) {
    BrowserResult r = browser_handle_key(&browser, key);
    if (r == BROWSER_SELECTED) {
        ROM new_rom;
        int re = nes_rom_load(&new_rom, browser.chosen_path);
        if (re == ROM_OK) {
            /* The loader resolves header timing plus
             * explicit legacy PAL filename tags. Flip the
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
            }
            /* The outgoing cartridge's battery RAM goes to disk first. */
            saves_flush(false);
            playback_load_cartridge(playback,&new_rom,new_region);
            playback_active = false;
            *console_changed = true;
            nes_rom_free(&rom);
            rom = new_rom;
            rom_loaded = true;
            saves_attach(browser.chosen_path);
            free(*static_frame);
            *static_frame = NULL;
            mynes_config_add_recent(&mynes_config,
                                    browser.chosen_path);
            mynes_config_save(&mynes_config);
            fprintf(stderr, "Loaded %s\n", browser.chosen_path);
        } else {
            fprintf(stderr, "Failed to load %s: %s\n",
                browser.chosen_path, nes_rom_error_str(re));
            browser_set_error(&browser,nes_rom_error_str(re));
            r=BROWSER_BROWSING;
        }
    }
    if (r == BROWSER_CANCELLED && !rom_loaded) {
        /* Started without a ROM and the user cancelled — quit. */
        running = false;
    }
    if (r != BROWSER_BROWSING) {
        browser_active = false;
        SDL_StopTextInput(window);
    }
}

/* Gamepad navigation reuses the keyboard path of whichever host UI is open. */
static void ui_navigate(SDL_Scancode sc, bool *console_changed, uint8_t **static_frame) {
    if (browser_active) {
        int bk = sdl_to_browser_key(sc);
        if (bk >= 0) browser_key((BrowserKey)bk, console_changed, static_frame);
    } else if (osd_menu_is_open) {
        gpu_osd_handle_key(sc, &osd_parameter_editing);
    }
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
    bool benchmark = false, force_sdr = false, screenshot_requested = false, native_fullscreen = false;
    int room_reflections_override=-1;
    int mask_alignment_override=-1,render_scale_override=-1,window_width=1280,window_height=960;
    char manual_screenshot_path[256];
    int exit_status = 0;
    int offscreen_w=0,offscreen_h=0;
    int presentation_mode=GPU_PRESENT_60HZ;
    float dark_frame_level=0.15f;
    FILE *playback_trace=NULL;
    const char *trace_path=getenv("MYNES_PLAYBACK_TRACE"), *limit_env=getenv("MYNES_PLAYBACK_FRAMES");
    const char *bench_capture=getenv("MYNES_PLAYBACK_READBACK_PATH");
    unsigned playback_limit=limit_env ? (unsigned)strtoul(limit_env,NULL,10) : 0;
    unsigned last_trace_frame=0, next_bench_capture=120;
    const char *debug_socket = getenv("MYNES_DEBUG_SOCKET");
    if (!debug_socket) debug_socket = "/tmp/mynes_gpu_debug.sock";
    const char *static_frame_path = getenv("MYNES_REVIEW_FRAME");  /* --simulate-frame overrides */
    const char *preset_path = getenv("MYNES_REVIEW_PRESET"); /* --preset overrides */
    int screenshot_count = 1, screenshots_taken = 0;
    char next_screenshot_path[4096];
    bool review_no_input = getenv("MYNES_REVIEW_NO_INPUT") != NULL;
    int screenshot_after = 0;              /* --screenshot-after <N>: dump and exit */
    const char *screenshot_path = "/tmp/gpu_capture.ppm";
    uint8_t *static_frame_buf = NULL;      /* 256*240 palette indices when loaded */
    /* Clip recording and scripted play; docs/gpu-controls.md, Recording clips. */
    const char *record_path = NULL, *load_state_path = NULL, *save_state_path = NULL;
    const char *input_replay_path = NULL, *input_record_path = NULL;
    double record_seconds = 0;
    int record_after = 2;
    Recorder *recorder = NULL;
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
        } else if (strcmp(argv[i], "--benchmark") == 0) {
            benchmark = true;
        } else if(strcmp(argv[i],"--offscreen")==0 && i+1<argc) {
            if(sscanf(argv[++i],"%dx%d",&offscreen_w,&offscreen_h)!=2 || offscreen_w<64 || offscreen_h<64 || offscreen_w>8192 || offscreen_h>8192) {
                fprintf(stderr,"Offscreen size must be WIDTHxHEIGHT in pixels\n");return 1;
            }
            review_no_input=true;
        } else if (strcmp(argv[i], "--native-fullscreen") == 0) {
            native_fullscreen=true;
        } else if (strcmp(argv[i], "--mask-alignment") == 0 && i+1<argc) {
            const char *mode=argv[++i];
            if(strcmp(mode,"pixels")==0) mask_alignment_override=0;
            else if(strcmp(mode,"physical")==0) mask_alignment_override=1;
            else { fprintf(stderr,"Mask alignment must be pixels or physical\n");return 1; }
        } else if (strcmp(argv[i], "--render-scale") == 0 && i+1<argc) {
            const char *scale=argv[++i];
            if(strcmp(scale,"auto")==0) render_scale_override=RENDER_SCALE_AUTO;
            else if(strcmp(scale,"1")==0 || strcmp(scale,"1.0")==0) render_scale_override=RENDER_SCALE_FULL;
            else if(strcmp(scale,"0.75")==0) render_scale_override=RENDER_SCALE_3_4;
            else if(strcmp(scale,"0.5")==0) render_scale_override=RENDER_SCALE_HALF;
            else { fprintf(stderr,"Render scale must be 1, 0.75, 0.5 or auto\n");return 1; }
        } else if (strcmp(argv[i], "--window-size") == 0 && i+1<argc) {
            if(sscanf(argv[++i],"%dx%d",&window_width,&window_height)!=2 || window_width<64 || window_height<64 || window_width>8192 || window_height>8192) {
                fprintf(stderr,"Window size must be WIDTHxHEIGHT in window coordinates\n");return 1;
            }
        } else if (strcmp(argv[i], "--presentation") == 0 && i+1<argc) {
            const char *mode=argv[++i];
            if (!strcmp(mode,"hold")) presentation_mode=GPU_PRESENT_HOLD;
            else if (!strcmp(mode,"bfi")) presentation_mode=GPU_PRESENT_BFI;
            else if (!strcmp(mode,"60hz")) presentation_mode=GPU_PRESENT_60HZ;
            else { fprintf(stderr,"Presentation must be hold, bfi or 60hz\n"); return 1; }
        } else if (strcmp(argv[i], "--dark-frame-level") == 0 && i+1<argc) {
            char *end;
            dark_frame_level=strtof(argv[++i],&end);
            if (*end || !*argv[i] || !isfinite(dark_frame_level) || dark_frame_level<0 || dark_frame_level>1) {
                fprintf(stderr,"Dark frame level must be 0..1\n"); return 1;
            }
        } else if (strcmp(argv[i], "--room-reflections") == 0) {
            room_reflections_override = 1;
        } else if (strcmp(argv[i], "--no-room-reflections") == 0) {
            room_reflections_override = 0;
        } else if (strcmp(argv[i], "--sdr") == 0) {
            force_sdr = true;
        } else if (strcmp(argv[i], "--simulate-frame") == 0 && i + 1 < argc) {
            static_frame_path = argv[++i];
        } else if (strcmp(argv[i], "--preset") == 0 && i + 1 < argc) {
            preset_path = argv[++i];
        } else if (strcmp(argv[i], "--screenshot-after") == 0 && i + 1 < argc) {
            screenshot_after = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--screenshot-pair") == 0) {
            screenshot_count = 2;
        } else if (strcmp(argv[i], "--screenshot-frames") == 0 && i + 1 < argc) {
            screenshot_count = atoi(argv[++i]);
            if (screenshot_count < 1 || screenshot_count > 240) {
                fprintf(stderr, "Screenshot count must be 1..240\n"); return 1;
            }
        } else if (strcmp(argv[i], "--screenshot-path") == 0 && i + 1 < argc) {
            screenshot_path = argv[++i];
        } else if (strcmp(argv[i], "--record") == 0 && i + 1 < argc) {
            record_path = argv[++i];
        } else if (strcmp(argv[i], "--record-seconds") == 0 && i + 1 < argc) {
            char *end;
            record_seconds = strtod(argv[++i], &end);
            if (*end || !*argv[i] || !isfinite(record_seconds) || record_seconds <= 0) {
                fprintf(stderr, "Record seconds must be a positive number\n"); return 1;
            }
        } else if (strcmp(argv[i], "--record-after") == 0 && i + 1 < argc) {
            char *end;
            long after = strtol(argv[++i], &end, 10);
            if (*end || !*argv[i] || after < 0 || after > 1000000) {
                fprintf(stderr, "Record-after must be a number of frames\n"); return 1;
            }
            record_after = (int)after;
        } else if (strcmp(argv[i], "--load-state") == 0 && i + 1 < argc) {
            load_state_path = argv[++i];
        } else if (strcmp(argv[i], "--save-state") == 0 && i + 1 < argc) {
            save_state_path = argv[++i];
        } else if (strcmp(argv[i], "--input-replay") == 0 && i + 1 < argc) {
            input_replay_path = argv[++i];
        } else if (strcmp(argv[i], "--input-record") == 0 && i + 1 < argc) {
            input_record_path = argv[++i];
        } else if (strcmp(argv[i], "--verbose") == 0 || strcmp(argv[i], "-v") == 0) {
            gpu_verbose = true;
        } else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            printf("Usage: %s [options] <rom.nes>\n"
                   "\n"
                   "NES emulator — GPU frontend (SDL3 + SDL_GPU).\n"
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
                   "  --screenshot-after N  Capture final CRT output and exit at frame N\n"
                   "  --screenshot-path P   PPM destination (+ linear-light PFM)\n"
                   "  --screenshot-pair     Capture N and N+1 for phase comparison\n"
                   "  --screenshot-frames N Capture 1..240 consecutive frames\n"
                   "  --benchmark           Fence complete preset chain at four resolutions\n"
                   "  --offscreen WxH       Hidden, silent playback into a pixel-sized target\n"
                   "  --presentation M     hold, bfi, or 60hz (default, paced hold)\n"
                   "  --dark-frame-level F  Dark-refresh emission, 0..1 (default 0.15)\n"
                   "  --room-reflections    Enable simulated room light and glare (G toggles)\n"
                   "  --no-room-reflections Disable simulated room light (default)\n"
                   "  --sdr                 Use SDR output for display comparisons\n"
                   "  --native-fullscreen   Enter native panel mode (F toggles back)\n"
                   "  --mask-alignment M    pixels (default) or physical CRT pitch\n"
                   "  --render-scale S      Internal CRT resolution: 1, 0.75, 0.5 or auto\n"
                   "                        (default 1; auto starts at 1 and steps down\n"
                   "                        if the GPU cannot keep up; M > Host display)\n"
                   "  --window-size WxH     Initial window size (UI coordinates)\n"
                   "  --record OUT          Record every emulated frame to OUT (.mov or .mp4)\n"
                   "                        with the APU audio; needs --offscreen and\n"
                   "                        --record-seconds. ffmpeg from MYNES_FFMPEG or PATH;\n"
                   "                        MYNES_RECORD_CODEC_ARGS replaces the video codec\n"
                   "                        arguments (see docs/gpu-controls.md)\n"
                   "  --record-seconds N    Clip length; frames = round(N x region rate)\n"
                   "  --record-after F      Emulated frames run before the first recorded\n"
                   "                        one (default 2)\n"
                   "  --load-state FILE     Load a save-state file once the ROM is running\n"
                   "  --save-state FILE     With --screenshot-after N: save the console state\n"
                   "                        after frame N (input from --input-replay counts\n"
                   "                        frames from power-on)\n"
                   "  --input-replay FILE   Scripted player-1 input, rows \"frame hexmask\";\n"
                   "                        with --record, row 1 is the first recorded frame\n"
                   "  --input-record FILE   Write player-1 input changes in that format during\n"
                   "                        windowed play, numbered from the last reset or\n"
                   "                        state load\n"
                   "  -h, --help            Show this help\n"
                   "\n"
                   "Controls (full table in docs/gpu-controls.md):\n"
                   "  Player 1: arrows, X = A, Z = B, Tab = Select, Return = Start\n"
                   "  Player 2: W/A/S/D, J = A, H = B, U = Select, Y = Start\n"
                   "  Gamepads hot-plug as P1 then P2: d-pad/left stick, EAST = A,\n"
                   "  SOUTH = B, BACK = Select, START = Start, GUIDE = menu,\n"
                   "  right shoulder = fast-forward\n"
                   "  Escape/M menu, Space pause, ` fast-forward, F, Globe+F, F11 or\n"
                   "  Alt+Return fullscreen, R reset, G room reflections, O ROM browser,\n"
                   "  P presets, F5 save state, F7 load state, F6 next state slot\n"
                   "  (4 slots), F12 screenshot, Ctrl+Q quit. Developer keys sit behind\n"
                   "  Ctrl.\n"
                   "  Battery RAM (.sav) and state slots are kept under the config\n"
                   "  directory, e.g. ~/.config/mynes/saves and ~/.config/mynes/states.\n",
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
    /* Recording needs the exact-size hidden target and a fixed length; the
     * other clip flags need a cartridge to act on before the loop starts. */
    if (record_path && !offscreen_w) { fprintf(stderr, "--record requires --offscreen WxH\n"); return 1; }
    if (record_path && record_seconds <= 0) { fprintf(stderr, "--record requires --record-seconds N\n"); return 1; }
    if (save_state_path && screenshot_after <= 0) {
        fprintf(stderr, "--save-state needs --screenshot-after N: the state is taken at frame N\n");
        return 1;
    }
    if ((record_path || load_state_path || input_replay_path || save_state_path) && !rom_path) {
        fprintf(stderr, "--record, --load-state and --input-replay need a ROM path\n"); return 1;
    }
    if (input_record_path && (offscreen_w || record_path || review_no_input)) {
        fprintf(stderr, "--input-record needs windowed play with live input\n"); return 1;
    }

    /* rom_path may be NULL — in that case the startup ROM browser runs
     * after the SDL/GPU init below, then sets rom_path before continuing. */

    fprintf(stderr, "MyNES — SDL3 GPU signal / CRT frontend\n");

    /* Persistent config (recent ROMs, last preset). */
    mynes_config_load(&mynes_config);
    render_ctx.room_reflections_enabled=room_reflections_override>=0
        ? room_reflections_override : mynes_config.gpu_room_reflections;
    render_ctx.mask_alignment=mask_alignment_override>=0 ? mask_alignment_override : mynes_config.gpu_mask_alignment;
    render_scale_mode=render_scale_override>=0 ? render_scale_override : mynes_config.gpu_render_scale;
    render_scale_fixed=benchmark || offscreen_w!=0;
    low_latency=mynes_config.gpu_low_latency;

    /* --- SDL3 init --- */
    if (offscreen_w || screenshot_after > 0 || benchmark || record_path) {
        /* An offscreen run shows nothing: no Dock icon, no activation. */
        if (offscreen_w) SDL_SetHint(SDL_HINT_MAC_BACKGROUND_APP, "1");
        batch_lock_wait();
    }
    gpu_output_disable_desktop_spaces();
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_GAMEPAD)) {
        fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
        return 1;
    }
    gpu_output_watch_globe();

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

    SDL_WindowFlags window_flags=SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY;
    if (benchmark || offscreen_w) window_flags |= SDL_WINDOW_HIDDEN;
    else if (screenshot_after > 0 || review_no_input) window_flags |= SDL_WINDOW_NOT_FOCUSABLE;
    const char *review_title = getenv("MYNES_REVIEW_TITLE");
    window = SDL_CreateWindow(
        review_title && *review_title ? review_title : "MyNES (GPU)",
        window_width, window_height,
        window_flags
    );
    if (!window) {
        fprintf(stderr, "SDL_CreateWindow failed: %s\n", SDL_GetError());
        SDL_DestroyGPUDevice(gpu);
        SDL_Quit();
        return 1;
    }
    /* Nothing is clicked; the pointer stays hidden over the picture in a
     * window and in fullscreen while MyNES is the active app. macOS lets
     * the frontmost app set the pointer, so it shows over a background
     * window. The title bar keeps it. */
    SDL_HideCursor();

    /* Optional native side-by-side review, sized in UI coordinates while
     * the renderer still resolves the mask in actual drawable pixels. */
    const char *review_side = getenv("MYNES_REVIEW_SIDE");
    if (!offscreen_w && review_side &&
        (!strcmp(review_side,"left") || !strcmp(review_side,"right"))) {
        SDL_Rect bounds;
        if (SDL_GetDisplayUsableBounds(SDL_GetPrimaryDisplay(), &bounds)) {
            int width = (bounds.w - 48) / 2;
            int height = width * 3 / 4;
            if (height > bounds.h - 80) height = bounds.h - 80;
            SDL_SetWindowSize(window, width, height);
            SDL_SetWindowPosition(window, bounds.x + 16 +
                (!strcmp(review_side,"right") ? width + 16 : 0), bounds.y + 40);
        }
    }

    if (!SDL_ClaimWindowForGPUDevice(gpu, window)) {
        fprintf(stderr, "SDL_ClaimWindowForGPUDevice failed: %s\n", SDL_GetError());
        SDL_DestroyWindow(window);
        SDL_DestroyGPUDevice(gpu);
        SDL_Quit();
        return 1;
    }

    if(native_fullscreen && !offscreen_w) {
        if(!gpu_output_toggle_fullscreen(window,true) || !SDL_SyncWindow(window)) {
            fprintf(stderr,"Native fullscreen failed: %s\n",SDL_GetError());
            SDL_DestroyGPUDevice(gpu);SDL_DestroyWindow(window);SDL_Quit();return 1;
        }
    }
    /* Enable vsync + HDR (EDR on macOS) if supported. */
    bool hdr_available = !force_sdr && SDL_WindowSupportsGPUSwapchainComposition(gpu, window,
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
        if(offscreen_w) SDL_SetAudioStreamGain(audio_stream,0);
        SDL_ResumeAudioStreamDevice(audio_stream);
    }

    /* --- NES init --- */
    nes_init(&nes);
    analog_controls = nes.apu.analog;

    /* Try to load a realistic RGB palette for the raw-display path.
     * Checked in priority order; first hit wins. The last candidate is the
     * copy shipped beside the executable (build tree, tarball, app bundle). */
    char pal_bundled[1024];
    const char *pal_base = SDL_GetBasePath();
    const char *pal_candidates[] = {
        "palettes/Digital Prime (FBX).pal",
        "../palettes/Digital Prime (FBX).pal",
        "../../palettes/Digital Prime (FBX).pal",
        pal_base ? resource_dir(pal_bundled, sizeof(pal_bundled), pal_base,
                                "palettes/Digital Prime (FBX).pal") : NULL,
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
     * preset, display) runs with NTSC defaults for the post-decoder browser. */
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
     * — the browser is a separate RGB overlay before the tube. */
    if (rom_path) {
        if (rom.tv_system == NES_TV_PAL) {
            region = 1;
            ppu_set_region(&nes.ppu, PPU_REGION_PAL);
            apu_set_region(&nes.apu, 1);
        }
        nes_reset(&nes);
        rom_loaded = true;
        if (gpu_verbose) nes_rom_print_info(&rom);
        saves_attach(rom_path);
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
    display_ppu.color_palette = nes.ppu.color_palette;
    preset_ctx.display_ppu = &display_ppu;
    preset_ctx.analog_controls = &analog_controls;
    preset_ctx.chain_vis = NULL;  /* set after chain_vis_create */
    preset_ctx.shader_dir = NULL; /* set after shader_dir_buf is resolved */
    preset_ctx.render_ctx = &render_ctx;
    preset_ctx_init(&preset_ctx);
    {
        /* Game submenu: play controls that are not television settings. */
        OSDMenuItem item = {0};
        item.type = OSD_MI_ACTION;
        item.label = "Pause / Resume (Space)"; item.action = action_toggle_pause;
        bool added = preset_menu_game_append(item);
        OSDMenuItem slot = {0};
        slot.type = OSD_MI_INT_CYCLIC; slot.label = "State slot (F6)";
        slot.target = &state_slot; slot.step = 1;
        slot.min_val = 1; slot.max_val = MYNES_STATE_SLOTS; slot.format = "%d";
        added = preset_menu_game_append(slot) && added;
        item.label = "Save state (F5)"; item.action = action_save_state;
        added = preset_menu_game_append(item) && added;
        item.label = "Load state (F7)"; item.action = action_load_state;
        added = preset_menu_game_append(item) && added;
        item.label = "Write battery save now"; item.action = action_write_battery;
        added = preset_menu_game_append(item) && added;
        item.label = "Quit (Ctrl+Q)"; item.action = action_quit;
        added = preset_menu_game_append(item) && added;
        if (!added) fprintf(stderr, "OSD Game submenu is full; some play actions are missing\n");
    }
    {
        /* Host display: performance settings, not television settings. */
        OSDMenuItem item = {0};
        item.type = OSD_MI_INT_CYCLIC; item.label = "Render scale";
        item.target = &render_scale_mode; item.step = 1;
        item.min_val = RENDER_SCALE_AUTO; item.max_val = RENDER_SCALE_HALF;
        item.on_change = action_render_scale_changed; item.format = "Auto|1.0|0.75|0.5";
        bool added = preset_menu_display_append(item);
        item = (OSDMenuItem){0};
        item.type = OSD_MI_TOGGLE; item.label = "Low latency";
        item.target = &low_latency; item.step = 1; item.max_val = 1;
        item.on_change = action_low_latency_changed;
        added = preset_menu_display_append(item) && added;
        if (!added) fprintf(stderr, "OSD Host display submenu is full; performance settings are missing\n");
    }

    /* Default preset selection. Apply once pre-GPU (populates FIR taps,
     * signal table, audio-chain sizing needed by video_gpu_init /
     * audio_gpu_init) and once post-GPU (pushes beam params, temporal
     * blend weights, stage param re-push against the live chain).
     * Single-call flows leave either GPU-side state stale (post-only)
     * or init with zero-size buffers (pre-only). */
    int startup_preset_idx = -1;
    if (preset_path) {
        startup_preset_idx=preset_register_file(preset_path);
        if(startup_preset_idx<0 && !strchr(preset_path,'/') && !strstr(preset_path,".json"))
            startup_preset_idx=preset_find_by_slug(preset_path);
        if(startup_preset_idx<0) { fprintf(stderr,"Could not load requested preset: %s\n",preset_path);return 1; }
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
        resource_dir(shader_dir_buf, sizeof(shader_dir_buf), base, "shaders/compute");
    } else {
        snprintf(shader_dir_buf, sizeof(shader_dir_buf),
                 "frontends/gpu/shaders/compute");
    }
    const char *shader_dir = shader_dir_buf;

    /* GPU and CPU backends consume the same band-limited APU samples. */
    gpu_audio_enabled = audio_gpu_init(&audio_gpu, gpu, &audio_chain, shader_dir);
    if (!gpu_audio_enabled) fprintf(stderr, "GPU audio unavailable; CPU chain remains active\n");
    const char *audio_override = getenv("MYNES_GPU_AUDIO");
    use_gpu_audio = gpu_audio_enabled &&
                    (!audio_override || strcmp(audio_override, "0") != 0);
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

        /* Render beam deposition at the tube viewport resolution times the
         * render scale. Offscreen captures use their requested pixel
         * dimensions exactly. */
        {
            int win_pw, win_ph;
            SDL_GetWindowSizeInPixels(window, &win_pw, &win_ph);
            if(offscreen_w) { win_pw=offscreen_w;win_ph=offscreen_h; }
            int beam_w, beam_h, beam_rps;
            beam_target_size(win_pw, win_ph, video_chain.tv.monitor_model==1 ? 16 : 4,
                             video_chain.tv.monitor_model==1 ? 10 : 3,
                             render_scale_effective(), &beam_w, &beam_h);
            beam_rps = beam_h / 240;
            if (beam_rps < 1) beam_rps = 1;

            LOGV("Beam target: %dx%d (%d rps, window %dx%d, scale %.2f)\n",
                 beam_w, beam_h, beam_rps, win_pw, win_ph, render_scale_effective());
            if (video_gpu_set_beam_params(&video_gpu_chain, gpu,
                                          beam_w, beam_h, beam_rps,
                                          0.20f, 0.70f)) {
                LOGV("Beam profile: %dx%d (%d rows/scanline)\n",
                     beam_w, beam_h, beam_rps);
                /* Push preset beam params — the hardcoded sigma above is just
                 * for buffer allocation. The real values come from the preset. */
                {
                    TVDisplayParams *tv = &video_chain.tv;
                    float sig_n = video_beam_sigma(tv, false);
                    float sig_w = video_beam_sigma(tv, true);
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
            resource_dir(render_shader_dir, sizeof(render_shader_dir), base, "shaders/render");
        } else {
            snprintf(render_shader_dir, sizeof(render_shader_dir),
                     "frontends/gpu/shaders/render");
        }
        bool display_ready=offscreen_w
            ? gpu_display_init_target(&gpu_disp,gpu,SDL_GetGPUSwapchainTextureFormat(gpu,window),
                                      offscreen_w,offscreen_h,render_shader_dir)
            : gpu_display_init(&gpu_disp,gpu,window,render_shader_dir);
        if (display_ready) {
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

    if (benchmark) {
        /* shader_dir came out of shader_dir_buf; leave room for the suffix. */
        char render_path[sizeof(shader_dir_buf) + 16];
        snprintf(render_path,sizeof(render_path),"%s/../render",shader_dir);
        printf("BENCH preset=%s\n",preset_path ? preset_path : preset_display_name(current_preset));
        if (!gpu_video_enabled || !gpu_benchmark(&video_gpu_chain,gpu,&sig_state,render_path,render_ctx.mask_alignment==0)) {
            fprintf(stderr,"Benchmark failed: %s\n",SDL_GetError());
            exit_status=1;
        }
        goto cleanup;
    }

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
    render_ctx.presentation_mode=presentation_mode;
    render_ctx.dark_frame_level=dark_frame_level;
    const char *present_trace=getenv("MYNES_PRESENT_TRACE");
    if (present_trace) {
        render_ctx.presentation_trace=fopen(present_trace,"w");
        if (render_ctx.presentation_trace)
            fprintf(render_ctx.presentation_trace,"submit_ns,source_frame,slot,slots,reported_hz,source_phase,mode\n");
    }
    render_ctx.offscreen_w=offscreen_w;render_ctx.offscreen_h=offscreen_h;
    const char *headroom_env=getenv("MYNES_OFFSCREEN_HEADROOM");
    render_ctx.offscreen_headroom=headroom_env ? fmaxf(1,atof(headroom_env)) : 1.6f;
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

    if(trace_path) {
        playback_trace=fopen(trace_path,"w");
        if(!playback_trace) { perror(trace_path); exit_status=1; goto cleanup; }
        fprintf(playback_trace,"frame,skipped,start_ns,ready_ns,submit_ns,emulation_ms,audio_ms,encode_ms,present_ms,swap_wait_ms,capture_ms,width,height\n");
    }
    playback = playback_create(&nes, gpu, gpu_audio_enabled ? &audio_gpu : NULL,
                               audio_stream, screenshot_after ? screenshot_after + screenshot_count - 1 : playback_limit,
                               screenshot_count > 1 ? screenshot_after : 0);
    if (!playback) { fprintf(stderr, "Playback worker: %s\n", SDL_GetError()); return 1; }
    /* Scripted input, the starting state and the recording are armed here,
     * before the worker runs its first frame, so frame 1 of the clip is the
     * first frame after the loaded state. A loaded state resets the picture
     * history and temporal CRT state in the loop like an F7 load does. */
    if (input_replay_path && !playback_load_input_script(playback, input_replay_path)) {
        fprintf(stderr, "Input replay: %s\n", SDL_GetError());
        exit_status = 1; goto cleanup;
    }
    bool state_loaded = false;
    if (load_state_path) {
        if (!state_load_file(load_state_path)) { exit_status = 1; goto cleanup; }
        state_loaded = true;
    }
    if (record_path) {
        RecorderOptions options = { .output = record_path, .seconds = record_seconds,
            .after = (unsigned)record_after, .region = preset_ctx.region,
            .width = offscreen_w, .height = offscreen_h };
        char error[2048];
        recorder = recorder_create(&options, error, sizeof(error));
        if (!recorder) { fprintf(stderr, "Recording: %s\n", error); exit_status = 1; goto cleanup; }
        playback_arm_capture(playback, recorder_first_frame(recorder), recorder_last_frame(recorder),
                             recorder_audio_file(recorder));
    }
    if (input_record_path) {
        if (!input_record_open(&input_record, input_record_path)) {
            fprintf(stderr, "Input record: cannot create %s\n", input_record_path);
            exit_status = 1; goto cleanup;
        }
        input_record_base = playback_frames_sampled(playback);
        fprintf(stderr, "Input record: %s\n", input_record_path);
    }
    const char *stall_env = getenv("MYNES_PRESENT_STALL_MS");
    int presentation_stall_ms = stall_env ? atoi(stall_env) : 0;
    const char *review_osd=getenv("MYNES_REVIEW_OSD");
    if (review_osd) {
        osd_menu_open_root(preset_menu_root,preset_menu_root_count,"Setup");
        /* The Game submenu sits first; reviews capture the picture controls. */
        for (int i = 0; i < preset_menu_root_count; i++)
            if (!strcmp(preset_menu_root[i].label,"Picture")) osd_menu_current()->selected = i;
        if (!strcmp(review_osd,"adjust")) {
            gpu_osd_handle_key(SDL_SCANCODE_RETURN,&osd_parameter_editing);
            gpu_osd_handle_key(SDL_SCANCODE_RETURN,&osd_parameter_editing);
        }
    }
    if (browser_active) SDL_StartTextInput(window);
    bool playback_fifo_forced = offscreen_w || screenshot_after > 0 || review_env_present();
    unsigned previous_picture = 0;
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
        bool console_changed = false;
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            /* Offline captures use explicit frame-script input only. Live
             * keyboard events must not change the test ROM or its preset.
             * Hot-plug events are dropped too: SDL reports every pad already
             * attached at startup, and its CONNECTED notice would land in
             * the captured frames, which are compared bit-for-bit. */
            if ((screenshot_after > 0 || review_no_input) &&
                (ev.type == SDL_EVENT_KEY_DOWN || ev.type == SDL_EVENT_KEY_UP ||
                 ev.type == SDL_EVENT_TEXT_INPUT || ev.type == SDL_EVENT_MOUSE_WHEEL ||
                 ev.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN || ev.type == SDL_EVENT_GAMEPAD_BUTTON_UP ||
                 ev.type == SDL_EVENT_GAMEPAD_AXIS_MOTION ||
                 ev.type == SDL_EVENT_GAMEPAD_ADDED || ev.type == SDL_EVENT_GAMEPAD_REMOVED)) continue;
            /* Resizes, fullscreen switches and display changes reallocate the
             * swapchain and CRT targets; the frames around them say nothing
             * about the steady GPU load Auto render scale judges. */
            if ((ev.type >= SDL_EVENT_WINDOW_FIRST && ev.type <= SDL_EVENT_WINDOW_LAST) ||
                (ev.type >= SDL_EVENT_DISPLAY_FIRST && ev.type <= SDL_EVENT_DISPLAY_LAST))
                render_scale_settle = 2;
            switch (ev.type) {
                case SDL_EVENT_QUIT:
                    running = false;
                    break;

                case SDL_EVENT_TEXT_INPUT:
                    if (browser_active) browser_handle_text(&browser,ev.text.text);
                    break;
                case SDL_EVENT_MOUSE_WHEEL:
                    if (browser_active) {
                        float dy=ev.wheel.y;
                        if(ev.wheel.direction==SDL_MOUSEWHEEL_FLIPPED) dy=-dy;
                        int steps=(int)ceilf(fabsf(dy));
                        if(steps>BROWSER_VISIBLE_ROWS) steps=BROWSER_VISIBLE_ROWS;
                        for(int i=0;i<steps;i++) browser_handle_key(&browser,
                            dy>0 ? BROWSER_KEY_UP : BROWSER_KEY_DOWN);
                    }
                    break;
                case SDL_EVENT_KEY_DOWN:
                    /* Browser input must not reach emulator hotkeys. */
                    if (browser_active) {
                        /* Globe+F toggles fullscreen here too; plain F types. */
                        if (ev.key.scancode == SDL_SCANCODE_F && gpu_output_globe_key(&ev.key)) {
                            if (!ev.key.repeat) toggle_fullscreen();
                            break;
                        }
                        int bk = sdl_to_browser_key(ev.key.scancode);
                        /* Quietly ignore unmapped keys when browsing. */
                        if (bk >= 0) browser_key((BrowserKey)bk, &console_changed, &static_frame_buf);
                        break;
                    }
                    bool ctrl = (ev.key.mod & SDL_KMOD_CTRL) != 0;
                    bool alt = (ev.key.mod & SDL_KMOD_ALT) != 0;
                    /* Ctrl+Q: quit through the normal cleanup path. */
                    if (ctrl && ev.key.scancode == SDL_SCANCODE_Q) { running = false; break; }
                    /* O: reopen the browser mid-session. */
                    if (ev.key.scancode == SDL_SCANCODE_O) {
                        if (!browser.current_dir[0]) browser_init(&browser, NULL, &mynes_config);
                        else browser_refresh(&browser);
                        browser.can_resume=rom_loaded;
                        close_osd_menu();
                        memset(keyboard_buttons, 0, sizeof(keyboard_buttons));
                        playback_pause(playback);
                        playback_active = false;
                        browser_active = true;
                        SDL_StartTextInput(window);
                        break;
                    }
                    /* R: console reset with the cartridge retained. */
                    if (ev.key.scancode == SDL_SCANCODE_R) {
                        if (!ev.key.repeat) preset_ctx.console_reset_requested = true;
                        break;
                    }
                    /* G: optional simulated room light; never repeats while held. */
                    if (ev.key.scancode == SDL_SCANCODE_G) {
                        if (!ev.key.repeat) preset_toggle_room_reflections();
                        break;
                    }
                    /* Ctrl+L: toggle chain visualiser. */
                    if (ctrl && ev.key.scancode == SDL_SCANCODE_L) {
                        chain_vis_toggle(chain_vis);
                        break;
                    }
                    /* Chain vis consumes navigation keys when open. */
                    if (chain_vis_handle_key(chain_vis, ev.key.scancode, true))
                        break;
                    /* OSD menu navigation (Escape backs out and closes it). */
                    if (gpu_osd_handle_key(ev.key.scancode, &osd_parameter_editing)) break;
                    /* Escape or M: open the OSD menu. Escape never quits play;
                     * the menu's Quit action and Ctrl+Q do. Ignore key repeat so
                     * a held key does not flap the menu open and closed. */
                    if (ev.key.scancode == SDL_SCANCODE_ESCAPE ||
                        ev.key.scancode == SDL_SCANCODE_M) {
                        if (!ev.key.repeat) open_osd_menu();
                        break;
                    }
                    /* Space: pause / resume. */
                    if (ev.key.scancode == SDL_SCANCODE_SPACE) {
                        if (!ev.key.repeat && rom_loaded) paused = !paused;
                        break;
                    }
                    /* Backquote: fast-forward while held. */
                    if (ev.key.scancode == SDL_SCANCODE_GRAVE) {
                        key_fast_forward = true;
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
                    /* F (also with Globe), F11 or Alt+Return: toggle fullscreen. */
                    if (ev.key.scancode == SDL_SCANCODE_F || ev.key.scancode == SDL_SCANCODE_F11 ||
                        (alt && ev.key.scancode == SDL_SCANCODE_RETURN)) {
                        if (!ev.key.repeat) toggle_fullscreen();
                        break;
                    }
                    /* Ctrl+D: dump GPU pipeline output as PPM for debugging. */
                    if (ctrl && ev.key.scancode == SDL_SCANCODE_D) {
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
                                if (display_ppu.index_framebuffer[t] != 0) nz_idx++;
                            printf("Index FB: %d/%d non-zero\n", nz_idx, 256*240);
                            /* Try generating waveform RIGHT NOW and check. */
                            waveform_generate(waveform_buf, display_ppu.index_framebuffer,
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
                        break;
                    }
                    /* Ctrl+B: toggle temporal blend (dot crawl cancel). */
                    if (ctrl && ev.key.scancode == SDL_SCANCODE_B) {
                        if (video_gpu_chain.temporal_blend < 0.01f) {
                            video_gpu_chain.temporal_blend = 0.5f;
                            printf("Temporal blend: ON (dot crawl cancelled)\n");
                        } else {
                            video_gpu_chain.temporal_blend = 0.0f;
                            printf("Temporal blend: OFF (dot crawl visible)\n");
                        }
                        break;
                    }
                    /* F5/F7 save and load the current state slot, F6 picks the
                     * next slot; none repeat while held. */
                    if (ev.key.scancode == SDL_SCANCODE_F5) {
                        if (!ev.key.repeat) state_save_requested = true;
                        break;
                    }
                    if (ev.key.scancode == SDL_SCANCODE_F7) {
                        if (!ev.key.repeat) state_load_requested = true;
                        break;
                    }
                    if (ev.key.scancode == SDL_SCANCODE_F6) {
                        if (!ev.key.repeat) cycle_state_slot();
                        break;
                    }
                    /* F12 captures the actual display pass, including mask/glass. */
                    if (ev.key.scancode == SDL_SCANCODE_F12) screenshot_requested = true;
                    /* P: cycle presets (scanned from presets/). */
                    if (ev.key.scancode == SDL_SCANCODE_P) {
                        int n = preset_total_count();
                        if (n > 0) {
                            int next = (current_preset + 1) % n;
                            preset_load_index(next);
                        }
                    }
                    /* Ctrl+T: cycle test signal patterns. */
                    if (ctrl && ev.key.scancode == SDL_SCANCODE_T) {
                        test_signal_mode = (test_signal_mode + 1) % 3;
                        const char *names[] = {"NES", "Color Bars", "Sine Sweep"};
                        printf("Test signal: %s\n", names[test_signal_mode]);
                        break;
                    }
                    /* Ctrl+A: toggle between CPU and GPU audio. */
                    if (ctrl && ev.key.scancode == SDL_SCANCODE_A) {
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
                    /* Ctrl chords are hotkeys, never game input (D is P2 right). */
                    if (!ctrl) handle_key(ev.key.scancode, true);
                    break;

                case SDL_EVENT_KEY_UP:
                    handle_key(ev.key.scancode, false);
                    if (ev.key.scancode == SDL_SCANCODE_GRAVE) key_fast_forward = false;
                    break;

                case SDL_EVENT_GAMEPAD_ADDED:
                    gamepad_added(ev.gdevice.which);
                    break;
                case SDL_EVENT_GAMEPAD_REMOVED:
                    gamepad_removed(ev.gdevice.which);
                    break;
                case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
                case SDL_EVENT_GAMEPAD_BUTTON_UP: {
                    int slot = gamepad_slot(ev.gbutton.which);
                    if (slot < 0) break;
                    GamepadSlot *g = &gamepads[slot];
                    bool down = ev.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN;
                    if (ev.gbutton.button == SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER) {
                        g->fast_forward = down;
                        break;
                    }
                    if (ev.gbutton.button == SDL_GAMEPAD_BUTTON_GUIDE) {
                        if (down && !browser_active) {
                            if (osd_menu_is_open) close_osd_menu(); else open_osd_menu();
                        }
                        break;
                    }
                    /* With a host UI open, presses navigate it instead of the
                     * game. Releases always clear so no button sticks when the
                     * UI closes underneath a held finger. */
                    if (down && (browser_active || osd_menu_is_open)) {
                        SDL_Scancode nav = gamepad_nav_key(ev.gbutton.button);
                        if (nav != SDL_SCANCODE_UNKNOWN)
                            ui_navigate(nav, &console_changed, &static_frame_buf);
                        break;
                    }
                    uint8_t mask = gamepad_button_mask(ev.gbutton.button);
                    if (down) g->buttons |= mask; else g->buttons &= ~mask;
                    break;
                }
                case SDL_EVENT_GAMEPAD_AXIS_MOTION: {
                    int slot = gamepad_slot(ev.gaxis.which);
                    if (slot < 0) break;
                    GamepadSlot *g = &gamepads[slot];
                    uint8_t before = g->stick;
                    g->stick = gamepad_stick_mask(before, ev.gaxis.axis, ev.gaxis.value);
                    /* Only a fresh deflection navigates; holding the stick
                     * past the deadzone repeats nothing. */
                    uint8_t pressed = g->stick & ~before;
                    if (pressed && (browser_active || osd_menu_is_open))
                        for (uint8_t bit = 0x10; bit; bit <<= 1)
                            if (pressed & bit)
                                ui_navigate(direction_nav_key(bit), &console_changed, &static_frame_buf);
                    break;
                }

                case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
                    if(offscreen_w) break;
                    if (gpu_display_enabled) {
                        gpu_display_resize(&gpu_disp, gpu,
                                           ev.window.data1, ev.window.data2);
                    }
                    break;
            }
        }

        if (preset_ctx.console_reset_requested) {
            preset_ctx.console_reset_requested = false;
            if (rom_loaded && nes.mapper_loaded && !static_frame_buf) {
                playback_reset_console(playback);
                playback_active = false;
                console_changed = true;
                close_osd_menu();
                fprintf(stderr,"Console reset (cartridge retained)\n");
            }
        }
        if (state_save_requested || state_load_requested || battery_write_requested) {
            bool console_ready = rom_loaded && nes.mapper_loaded && !static_frame_buf;
            if (state_save_requested && console_ready) state_save_slot();
            if (state_load_requested && console_ready) state_loaded = state_load_slot();
            if (battery_write_requested && console_ready) {
                if (!saves.battery) show_notice("BATTERY SAVE", "This cartridge has no battery RAM");
                else if (saves_flush(true)) show_notice("BATTERY SAVE", "Written");
                else show_notice("BATTERY SAVE FAILED", saves.sav_path);
            }
            state_save_requested = state_load_requested = battery_write_requested = false;
        }
        if (console_changed) memset(keyboard_buttons, 0, sizeof(keyboard_buttons));
        if (console_changed || state_loaded) {
            /* A fresh console starts playing; a stale pause would freeze it
             * behind the previous cartridge's last picture. A loaded state is
             * a new time line as well: the picture history and the temporal
             * CRT state (persistence, supply sag) must not blend across it. */
            paused = false;
            previous_picture = 0;
            frame_deadline = 0;
            render_ctx.presentation_slot = 0;
            render_ctx.pacing_deadline_ns = 0;
            render_ctx.presentation_epoch++;
            if (gpu_video_enabled) video_gpu_reset_temporal_state(&video_gpu_chain,gpu);
            /* Input rows count from the frame after the new time line began. */
            input_record_base = state_loaded ? state_load_frame : playback_frames_sampled(playback);
            if (input_record.file && !input_record_restart(&input_record))
                fprintf(stderr, "Input record: cannot restart %s\n", input_record_path);
        }
        state_loaded = false;
        if (!rom_loaded && !browser_active) { SDL_Delay(10); continue; }
        // Check viewport after events, preset changes and render scale steps,
        // including offscreen mode.
        if (gpu_video_enabled && video_gpu_chain.beam_out_w>0) {
            int w,h;
            SDL_GetWindowSizeInPixels(window,&w,&h);
            if(offscreen_w) { w=offscreen_w;h=offscreen_h; }
            beam_target_size(w,h,video_chain.tv.monitor_model==1 ? 16 : 4,
                             video_chain.tv.monitor_model==1 ? 10 : 3,render_scale_effective(),&w,&h);
            if(w>0 && h>0 && (w!=video_gpu_chain.beam_out_w || h!=video_gpu_chain.beam_out_h)) {
                render_scale_settle = 2;
                if(!render_ctx.owns_display_tex) render_ctx.display_tex=NULL;
                if(!video_gpu_set_beam_params(&video_gpu_chain,gpu,w,h,h/240>0 ? h/240 : 1,
                       video_gpu_chain.beam_sigma_narrow,video_gpu_chain.beam_sigma_wide))
                    fprintf(stderr,"Could not resize CRT beam: %s\n",SDL_GetError());
            }
        }
        bool live = rom_loaded && !browser_active && !static_frame_buf && !paused;
        /* Host UI captures every controller; nothing leaks into the game
         * while a menu or the browser is up. */
        bool ui_open = browser_active || osd_menu_is_open;
        bool fast_forward = live && (key_fast_forward ||
            gamepads[0].fast_forward || gamepads[1].fast_forward);
        gpu_render_presentation_update(&render_ctx, 1000.0f / signal_region_frame_ms(preset_ctx.region));
        PlaybackControls controls = { .audio = audio_chain, .analog = analog_controls,
            .region = preset_ctx.region, .gpu_audio = use_gpu_audio != 0,
            .presentation_mode = render_ctx.presentation_mode,
            .display_paced = render_ctx.vsync_paced,
            .display_hz = render_ctx.presentation_hz,
            .low_latency = low_latency && !playback_fifo_forced,
            .speed = fast_forward ? 8.0f : 1.0f };
        for (int i = 0; i < 2; i++)
            controls.controller[i] = ui_open ? 0 :
                keyboard_buttons[i] | gamepads[i].buttons | gamepads[i].stick;
        playback_controls(playback, &controls);
        /* The mask just handed over is first sampled by the frame after the
         * ones already started; that is the row's frame number. */
        if (input_record.file && rom_loaded && !static_frame_buf &&
            !input_record_update(&input_record, playback_frames_sampled(playback) - input_record_base + 1,
                                 controls.controller[0])) {
            fprintf(stderr, "Input record: cannot write %s\n", input_record_path);
            input_record_close(&input_record);
        }
        if (live != playback_active) {
            if (live) playback_resume(playback); else playback_pause(playback);
            playback_active = live;
        }
        /* Battery RAM reaches disk shortly after a game changes it, so a
         * crash loses at most a couple of seconds of progress. A failing
         * disk is retried less often than it is reported. */
        if (saves.battery && SDL_GetTicks() >= battery_next_check)
            battery_next_check = SDL_GetTicks() + (saves_flush(false) ? BATTERY_FLUSH_MS : 15 * BATTERY_FLUSH_MS);
        if (render_ctx.presentation_slot > 0) {
            /* Re-present phosphor light only. Do not advance the PPU, audio,
             * signal phase, beam history, CRT load or diagnostic frame count. */
            gpu_render_frame(&render_ctx, &video_chain);
            if (render_ctx.submit_ns)
                render_ctx.presentation_slot = (render_ctx.presentation_slot + 1) % render_ctx.presentation_slots;
            else SDL_Delay(1);
            continue;
        }
        Uint64 t_emu0 = 0, t_emu1 = 0;
        sig_state.frame_phase_override = -1;
        if (live) {
            if(!gpu_render_prepare(&render_ctx)) { SDL_Delay(1); continue; }
            if (!playback_read(playback, &picture)) { SDL_Delay(1); continue; }
            memcpy(display_ppu.framebuffer, picture.rgb, sizeof(picture.rgb));
            memcpy(display_ppu.index_framebuffer, picture.codes, sizeof(picture.codes));
            video_gpu_chain.elapsed_frames = previous_picture ? picture.number - previous_picture : 1;
            previous_picture = picture.number;
            video_gpu_chain.signal_frame_counter = picture.number - 1;
            video_gpu_chain.beam_frame_counter = picture.number - 1;
            render_ctx.frame_counter = picture.number - 1;
            frame_count = picture.number - 1;
            sig_state.frame_phase_override = picture.phase;
            t_emu1 = picture.emulation_ticks;
            render_ctx.audio_bass_rms = picture.audio_energy;
            if (presentation_stall_ms > 0 && picture.number >= 60) {
                SDL_Delay(presentation_stall_ms);
                presentation_stall_ms = 0;
            }
        } else {
            Uint64 now = SDL_GetTicksNS();
            Uint64 period = (Uint64)(signal_region_frame_ms(preset_ctx.region) * 1000000.0);
            period = gpu_presentation_playback_period(render_ctx.presentation_mode, period,
                render_ctx.presentation_hz, render_ctx.vsync_paced);
            if (!frame_deadline || now > frame_deadline + period * 3) frame_deadline = now;
            /* Wait in short steps and sample the frame fence on each one.
             * Nothing else looks at the fence until the next submit, so one
             * long sleep would time every frame here at a full period
             * whatever the GPU cost, and Auto render scale would step down
             * while the browser, the menu or a pause is up. */
            while (now < frame_deadline) {
                gpu_render_poll_frame_fence(&render_ctx);
                Uint64 remaining = frame_deadline - now;
                SDL_DelayPrecise(remaining < 1000000 ? remaining : 1000000);
                now = SDL_GetTicksNS();
            }
            frame_deadline += period;
            if (static_frame_buf) {
                const uint8_t (*pal)[3] = display_ppu.color_palette ? display_ppu.color_palette : ppu_palette_2c02;
                for (int i = 0; i < 256 * 240; i++) {
                    uint8_t idx = static_frame_buf[i] & 0x3f;
                    display_ppu.index_framebuffer[i] = idx;
                    memcpy(display_ppu.framebuffer + i * 3, pal[idx], 3);
                }
            } else {
                /* The raw RGB paths blend the OSD into this buffer in place,
                 * so repaint the retained picture every frame or a menu, an
                 * expired notice and the PAUSED backdrop accumulate on the
                 * frozen picture for as long as the pause lasts. */
                memcpy(display_ppu.framebuffer, picture.rgb, sizeof(picture.rgb));
                memcpy(display_ppu.index_framebuffer, picture.codes, sizeof(picture.codes));
            }
        }

        /* The core exposes the backdrop at frame handoff. Raster-side palette
         * writes are not observed here; this snapshot is used only outside the picture. */
        unsigned backdrop = live ? picture.backdrop : 0x0f;
        memcpy(video_gpu_chain.backdrop, sig_state.table[backdrop], 12 * sizeof(float));
        memcpy(video_gpu_chain.gray_backdrop, sig_state.table[backdrop & 0x1f0], 12 * sizeof(float));

        /* --- Overlays (before signal processing) --- */
        preset_composite_overlays(&preset_ctx);

        /* All host UI uses one RGB plane after the receiver, before the tube.
         * Browser/menu take priority over the performance panel. */
        bool osd_visible=false;
        memset(osd_pixels,0,sizeof(osd_pixels));
        if (browser_active) {
            browser_render_rgba(&browser,osd_pixels);
            osd_visible=true;
        } else if (osd_menu_is_open) {
            gpu_osd_render(osd_pixels,osd_menu_current(),osd_parameter_editing,
                preset_display_name(preset_active_index()),preset_is_modified(),
                preset_ctx.region==SIGNAL_REGION_PAL,&render_ctx,gpu_render_headroom(&render_ctx));
            osd_visible=true;
        } else {
            const char *notice=preset_cycle_notice();
            /* preset_apply.c raises this from both the G key and the OSD
             * toggle; consume it as a request for the shared notice. */
            if (render_ctx.room_reflections_notice_until) {
                render_ctx.room_reflections_notice_until=0;
                show_notice("ROOM REFLECTIONS (G)",render_ctx.room_reflections_enabled ? "ON" : "OFF");
            }
            if (SDL_GetTicks()<notice_until) {
                gpu_osd_notice(osd_pixels,notice_title,notice_value);
                osd_visible=true;
            } else if (notice && *notice) {
                gpu_osd_preset_notice(osd_pixels,notice);
                osd_visible=true;
            } else if (paused && rom_loaded && !static_frame_buf) {
                gpu_osd_notice(osd_pixels,"PAUSED","Space resumes");
                osd_visible=true;
            } else if (fast_forward) {
                gpu_osd_notice(osd_pixels,"FAST FORWARD","Up to 8x while held");
                osd_visible=true;
            } else if (perf_overlay && perf_text[0]) {
                gpu_osd_performance(osd_pixels,perf_text);
                osd_visible=true;
            }
        }
        if (gpu_video_enabled && !video_gpu_set_osd(&video_gpu_chain,gpu,osd_visible ? osd_pixels : NULL))
            fprintf(stderr,"OSD upload failed: %s\n",SDL_GetError());
        if (osd_visible && (!composite_enabled || !gpu_video_enabled))
            gpu_osd_blend_rgb(display_ppu.framebuffer,osd_pixels);

        /* --- Video output --- */
        Uint64 t_gpu0 = SDL_GetPerformanceCounter(), t_gpu1 = t_gpu0;
        render_ctx.frame_brightness =
            gpu_render_compute_frame_brightness(display_ppu.framebuffer);
        render_ctx.raw_ppu_rgb = display_ppu.framebuffer;
        gpu_render_update_dynamic_state(&render_ctx);

        bool gpu_crt_active = composite_enabled && gpu_video_enabled;
        bool signal_decode_active = gpu_crt_active
                                 && video_connection_uses_signal_decode(video_chain.connection);
        bool frame_advanced = false;

        if (gpu_crt_active) {
            /* Generate the DAC waveform on GPU when no CPU-only edge effects
             * or waveform diagnostics are requested. Both paths use PPU codes. */
            const TVDisplayParams *tv = &video_chain.tv;
            bool gpu_dac = !signal_decode_active || (test_signal_mode == 0 &&
                video_gpu_chain.pipe_dac.pipeline && video_gpu_chain.buf_signal_table &&
                (video_chain.connection == VIDEO_CONN_SVIDEO || (!debug_dump &&
                 tv->beam_edge_fade == 0 && tv->beam_edge_overshoot == 0 &&
                 (tv->burst_lock_drift == 0 || tv->burst_lock_drift_width == 0))));
            if (test_signal_mode == 0 && (!gpu_dac || debug_dump)) {
                waveform_generate(waveform_buf, display_ppu.index_framebuffer,
                                  &sig_state, frame_count);
                waveform_apply_beam_edges(waveform_buf,
                                          display_ppu.index_framebuffer,
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

            bool dump_this_frame = signal_decode_active && debug_dump &&
                frame_wanted(frame_count, debug_dump_frames, debug_dump_count);

            /* Pre-process waveform snapshot. */
            if (dump_this_frame) {
                if(gpu_dac) waveform_generate(waveform_buf,display_ppu.index_framebuffer,&sig_state,frame_count-1);
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
                                        render_ctx.apl_slow_state,
                                        render_ctx.audio_bass_rms);
            t_gpu0 = SDL_GetPerformanceCounter();
            float *readback = (dump_this_frame ||
                !video_gpu_get_beam_texture(&video_gpu_chain)) ? gpu_rgb_out : NULL;
            bool gpu_ok = gpu_dac
                ? video_gpu_process_full(&video_gpu_chain, gpu, display_ppu.index_framebuffer,
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
                    dump_waveform_ppm(ppm_wf, waveform_buf, spl, 240);
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
                /* GPU dispatch failed: keep host UI usable on the raw fallback. */
                if(osd_visible) gpu_osd_blend_rgb(display_ppu.framebuffer,osd_pixels);
                gpu_render_ensure_texture(&render_ctx, 256, 240);
                gpu_render_upload_rgba(&render_ctx,
                    gpu_render_ppu_to_rgba8(display_ppu.framebuffer), 256, 240);
            }
        } else if (composite_enabled) {
            /* GPU video is unavailable. */
            gpu_render_ensure_texture(&render_ctx, 256, 240);
            gpu_render_upload_rgba(&render_ctx,
                gpu_render_ppu_to_rgba8(display_ppu.framebuffer), 256, 240);
        } else {
            /* Raw RGB path: palette LUT output, no composite. */
            gpu_render_ensure_texture(&render_ctx, 256, 240);
            gpu_render_upload_rgba(&render_ctx,
                gpu_render_ppu_to_rgba8(display_ppu.framebuffer), 256, 240);
        }

        if (!frame_advanced) {
            frame_count++;
        }

        /* --- Capture debug tap snapshots (GPU buffer readback for visualiser) --- */
        if (gpu_crt_active && tap_mgr && debug_tap_enabled_count(tap_mgr) > 0) {
            debug_tap_capture(tap_mgr, gpu, frame_count);
        }

        if(live && bench_capture && picture.number>=next_bench_capture) {
            render_ctx.capture_path=bench_capture;
            next_bench_capture=picture.number+60;
        }
        if (screenshot_requested) {
            snprintf(manual_screenshot_path,sizeof(manual_screenshot_path),"/tmp/nes_screenshot_%u.ppm",frame_count);
            render_ctx.capture_path = manual_screenshot_path;
        }
        if (screenshot_after > 0 && frame_count >= (unsigned)screenshot_after) {
            if (screenshot_count == 2)
                snprintf(next_screenshot_path, sizeof(next_screenshot_path), "%s.next.ppm", screenshot_path);
            else
                snprintf(next_screenshot_path, sizeof(next_screenshot_path), "%s.frame-%03d.ppm", screenshot_path, screenshots_taken);
            render_ctx.capture_path = screenshots_taken ? next_screenshot_path : screenshot_path;
        }
        // Batch captures finish on this frame; interactive captures write
        // owned pixels in the background and are drained before shutdown.
        render_ctx.capture_async = screenshot_after <= 0;
        /* A recorded picture is handed over from inside the render, before
         * the worker may produce the next one; a frame that did not arrive
         * is a failed recording, never a skipped one. */
        bool record_frame = recorder && live && recorder_want_frame(recorder, picture.number);
        unsigned recorded_before = recorder ? recorder_frames_written(recorder) : 0;
        render_ctx.capture_sink = record_frame ? recorder_push_frame : NULL;
        render_ctx.capture_sink_user = recorder;
        Uint64 t_render0 = SDL_GetPerformanceCounter();
        render_ctx.source_phase = signal_frame_phase(&sig_state, frame_count - 1);
        gpu_render_frame(&render_ctx, &video_chain);
        Uint64 t_render1 = SDL_GetPerformanceCounter();
        if (record_frame) {
            if (recorder_frames_written(recorder) != recorded_before + 1) {
                fprintf(stderr, "Recording: frame %u was not captured\n", picture.number);
                exit_status = 1;
                running = false;
            } else if (recorder_complete(recorder)) {
                running = false;
            }
        }
        if (render_ctx.submit_ns && render_ctx.presentation_slots > 1)
            render_ctx.presentation_slot = 1;
        if(live && playback_trace && render_ctx.submit_ns) {
            double ms=1000.0/SDL_GetPerformanceFrequency();
            fprintf(playback_trace,"%u,%u,%llu,%llu,%llu,%.5f,%.5f,%.5f,%.5f,%.5f,%.5f,%d,%d\n",
                picture.number,last_trace_frame ? picture.number-last_trace_frame-1 : 0,
                (unsigned long long)picture.start_ns,(unsigned long long)picture.ready_ns,
                (unsigned long long)render_ctx.submit_ns,picture.emulation_ticks*ms,picture.audio_ns/1e6,
                (t_gpu1-t_gpu0)*ms,(t_render1-t_render0)*ms+render_ctx.swap_wait_ns/1e6,render_ctx.swap_wait_ns/1e6,
                render_ctx.capture_ns/1e6,render_ctx.drawable_w,render_ctx.drawable_h);
            last_trace_frame=picture.number;
        }
        if(live && playback_limit && picture.number>=playback_limit) running=false;

        if (render_ctx.capture_path) {
            if (!render_ctx.capture_accepted) fprintf(stderr, "Final display capture failed: %s\n", SDL_GetError());
            fprintf(stderr, "Capture frame %u, carrier phase %d\n", frame_count,
                    signal_frame_phase(&sig_state, frame_count - 1));
            if (screenshot_after > 0) {
                ++screenshots_taken;
                running = render_ctx.capture_accepted && screenshots_taken < screenshot_count;
                /* The worker stops at the last listed frame, so the console
                 * holds exactly the state after it; saved the way F5 does. */
                if (!running && save_state_path) {
                    StateJob job = { .size = nes_state_size(&nes) };
                    job.data = malloc(job.size);
                    if (job.data) with_console(console_save_state, &job);
                    if (job.ok && mynes_write_file_atomic(save_state_path, job.data, job.size)) {
                        fprintf(stderr, "State after frame %u saved to %s\n", frame_count, save_state_path);
                    } else {
                        fprintf(stderr, "Save state to %s failed\n", save_state_path);
                        exit_status = 1;
                    }
                    free(job.data);
                }
            }
            screenshot_requested = false;
            render_ctx.capture_path = NULL;
        }

        /* --- Send frame snapshot to debug server (visualiser) --- */
        if (debug_srv && debug_server_has_client(debug_srv) && gpu_video_enabled) {
            debug_server_frame(debug_srv, &video_gpu_chain.sig_chain, NULL, frame_count);
        }

        /* --- Performance stats accumulation --- */
        {
            static Uint64 perf_emu_total = 0, perf_gpu_total = 0, perf_render_total = 0;
            static int perf_frames = 0;
            static bool perf_fast_window = false, perf_idle_window = false;
            static Uint64 perf_last_print = 0;

            perf_emu_total += t_emu1 - t_emu0;
            if (composite_enabled && gpu_video_enabled)
                perf_gpu_total += t_gpu1 - t_gpu0;
            perf_render_total += t_render1 - t_render0;
            perf_frames++;
            perf_fast_window = perf_fast_window || fast_forward;
            perf_idle_window = perf_idle_window || !live;

            Uint64 freq = SDL_GetPerformanceFrequency();
            if (!perf_last_print) perf_last_print = t_render1;
            if (t_render1 - perf_last_print >= freq) {
                double emu_ms = (double)perf_emu_total * 1000.0 / (double)freq / perf_frames;
                double gpu_ms = (double)perf_gpu_total * 1000.0 / (double)freq / perf_frames;
                double ren_ms = (double)perf_render_total * 1000.0 / (double)freq / perf_frames;
                /* ENC and PRESENT are CPU encode times; GPU is the fenced
                 * submit-to-completion time, the number a GPU-bound Retina
                 * frame actually shows up in. */
                unsigned gpu_samples = render_ctx.gpu_frame_samples;
                double gpu_frame_ms = gpu_samples ? render_ctx.gpu_frame_total_ns / 1e6 / gpu_samples : 0;
                render_ctx.gpu_frame_total_ns = 0;
                render_ctx.gpu_frame_samples = 0;
                float scale = render_scale_effective();
                snprintf(perf_text, sizeof(perf_text),
                         "EMU %.1f ENC %.1f PRESENT %.1f GPU %.1f%s%s",
                         emu_ms, gpu_ms, ren_ms, gpu_frame_ms,
                         scale < 1 ? " SCALE " : "",
                         scale < 1 ? render_scale_name[render_scale_mode == RENDER_SCALE_AUTO
                                                      ? render_scale_auto_level : render_scale_mode] : "");
                /* Auto render scale: a GPU that needs more than 90% of the
                 * presentation interval for two consecutive windows has no
                 * margin for a heavier scene, so drop one notch before frames
                 * start to drop. Fast-forward windows queue GPU work on
                 * purpose and are not evidence; neither is a window with a
                 * frame that was not live (browser, menu, pause, static
                 * review), where no game frame can drop and the CPU-paced
                 * wait shapes the timing, nor the window of a resize,
                 * fullscreen switch or display change and the one after it.
                 * It never steps back up. */
                double native_ms = signal_region_frame_ms(preset_ctx.region);
                double budget_ms = gpu_presentation_playback_period(render_ctx.presentation_mode,
                    (uint64_t)(native_ms * 1000000.0), render_ctx.presentation_hz,
                    render_ctx.vsync_paced) / 1e6;
                if (render_ctx.presentation_slots > 1) budget_ms /= render_ctx.presentation_slots;
                bool settling = render_scale_settle > 0;
                if (settling) render_scale_settle--;
                if (render_scale_mode == RENDER_SCALE_AUTO && !render_scale_fixed &&
                    render_scale_auto_level < RENDER_SCALE_HALF && gpu_samples >= 10 &&
                    !perf_fast_window && !perf_idle_window && !settling &&
                    gpu_frame_ms > 0.9 * budget_ms) {
                    if (++render_scale_over_windows >= 2) {
                        render_scale_over_windows = 0;
                        render_scale_auto_level++;
                        char value[40];
                        snprintf(value, sizeof(value), "%s (auto)", render_scale_name[render_scale_auto_level]);
                        show_notice("RENDER SCALE", value);
                        fprintf(stderr, "Render scale %s (auto): GPU %.1f ms of %.1f ms per frame\n",
                                render_scale_name[render_scale_auto_level], gpu_frame_ms, budget_ms);
                    }
                } else {
                    render_scale_over_windows = 0;
                }
                perf_emu_total = perf_gpu_total = perf_render_total = 0;
                perf_frames = 0;
                perf_fast_window = perf_idle_window = false;
                perf_last_print = t_render1;
            }
        }
    }

    /* ========================================================================
     * Cleanup
     * ======================================================================== */

cleanup:
    if(playback_trace) fclose(playback_trace);
    if(render_ctx.presentation_trace) fclose(render_ctx.presentation_trace);
    saves_flush(false);
    playback_destroy(playback);
    if (!gpu_render_release_pending(&render_ctx)) exit_status=1;
    /* The worker has stopped writing audio, so the clip can be muxed. */
    if (recorder) {
        char error[2048];
        if (!recorder_finish(recorder, error, sizeof(error))) {
            fprintf(stderr, "Recording failed: %s\n", error);
            exit_status = 1;
        }
        recorder_destroy(recorder);
    }
    if (input_record.file && !input_record_close(&input_record)) {
        fprintf(stderr, "Input record: cannot write %s\n", input_record_path);
        exit_status = 1;
    }
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
    for (int i = 0; i < 2; i++)
        if (gamepads[i].pad) SDL_CloseGamepad(gamepads[i].pad);
    if (audio_stream) SDL_DestroyAudioStream(audio_stream);
    SDL_ReleaseWindowFromGPUDevice(gpu, window);
    SDL_DestroyWindow(window);
    SDL_DestroyGPUDevice(gpu);
    SDL_Quit();
    return exit_status;
}
