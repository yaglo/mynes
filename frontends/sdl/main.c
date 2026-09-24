/*
 * Interactive NES Emulator with SDL2
 *
 * Controls:
 *   Arrow keys - D-pad
 *   X          - A button
 *   Z          - B button
 *   Enter      - Start
 *   Shift      - Select
 *   Escape     - Quit
 *   Space      - Pause/Resume
 *   S          - Save screenshot
 *   F          - Toggle fullscreen
 *   Tab        - Toggle fast mode (no FPS limit)
 *   P          - Cycle through palettes
 */

#include <errno.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <dirent.h>
#include <SDL.h>
#include "nes/rom.h"
#include "nes/nes.h"
#include "nes/composite.h"
#ifdef MYNES_CRT_CAPTURE
#include "crt_capture.h"
#include "crt_live.h"
static crt_live *crt_usb;
static FILE *crt_capture_file;
static int crt_capture_failed;
#endif

/* Shared frontend helpers (fullscreen ROM browser + persistent
 * recent/last-preset config). */
#include "browser.h"
#include "config.h"
#include "saves.h"
#include "nes/state.h"

static int scale = 3;
static bool composite_enabled = true;
static Composite composite;
/* NES state — declared early so the menu tables below can reference
 * &nes.apu.analog.X as static-initializer address constants. */
static NES nes;
static ROM rom;

/* ROM browser + config. The browser draws into the PPU framebuffer and
 * rides the existing render pipeline, just like a game frame would. */
static MynesConfig mynes_config;
static Browser     browser;
static bool        browser_active = false;
static bool        rom_loaded     = false;

/* Battery-backed cartridge RAM (frontends/shared/saves.h). This frontend
 * runs the console on the main thread, so its RAM is read directly. */
static MynesSaves  saves;
static Uint32      battery_next_check;
static void saves_attach(const char *rom_path) {
    uint32_t crc = nes_state_rom_crc(rom.prg_rom, rom.prg_size, rom.chr_rom, rom.chr_size);
    mynes_saves_open(&saves, rom_path, crc, rom.has_battery);
    if (saves.battery && mynes_saves_restore(&saves, nes.mapper.prg_ram))
        printf("Battery RAM restored from %s\n", saves.sav_path);
}

/* Translate SDL2 scancode → BrowserKey, or -1 if not handled. */
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

/* Static-frame injection mode — when non-NULL, the main loop skips
 * emulation and paints this 256×240 palette-index buffer into the PPU
 * framebuffers every frame, so the composite pipeline runs against a
 * fixed reference image. Used to tune presets against a CRT photo. */
static uint8_t *static_frame_idx = NULL;
/* Optional overlay for side-by-side visual comparison with the CRT
 * photo. Held as RGB888 at PPU_WIDTH × PPU_HEIGHT. NULL = disabled. */
static uint8_t *static_frame_overlay = NULL;

/* RAM pokes: list of (addr, val, frame_trigger) tuples. Applied each
 * frame until consumed. Used for stage-warp experiments on specific
 * ROMs — no save state support, so we just jam bytes into CPU RAM
 * (0x0000-0x07FF) at a given frame number and hope the game picks
 * them up on its next state transition. */
#define POKES_MAX 32
typedef struct { uint16_t addr; uint8_t val; int trigger_frame; int done; } RamPoke;
static RamPoke ram_pokes[POKES_MAX];
static int ram_pokes_n = 0;

/* Auto-input: when >0, the emulator pulses BTN_START on controller 1
 * for 2 frames then releases for 10 frames, up to `auto_start_until`
 * frames. Lets us boot Contra past the "1 PLAYER / 2 PLAYERS" title
 * into stage 1 without a human pressing Enter. */
static int auto_start_until = 0;
/* Display vsync — syncs SDL_RenderPresent to monitor vblank so there's
 * no horizontal tearing. Frame pacing is still audio-ring-driven, with
 * a ~2% rate-adjustment window that easily absorbs the ~0.2% mismatch
 * between NES 60.0988 Hz and display 60 Hz. Togglable at runtime. */
static int vsync_enabled = 1;
static SDL_Renderer *renderer;   /* forward decl for cb_apply_vsync */
static SDL_Window   *window;     /* forward decl for apply_display_for_nes_region */

/* Current NES-level region (COMP_REGION_NTSC / COMP_REGION_PAL equivalent,
 * but using NES_REGION_* enum). Mirrored at file scope so toggle_fullscreen
 * can re-apply the appropriate display mode when entering/leaving
 * exclusive fullscreen. */
static int nes_region_current = NES_REGION_NTSC;
/* --pal: every ROM runs as PAL, whatever its header says. */
static bool force_pal = false;
static int apply_rom_region(void);   /* fwd decl for the ROM browser */

/* ============================================================================
 * Audio Ring Buffer
 *
 * Lock-free SPSC ring buffer. The emulator thread pushes samples, and the
 * SDL audio callback thread pulls them.
 * ============================================================================ */

#define AUDIO_BUF_SIZE 16384  /* ~370ms at 44100Hz — enough to absorb jitter */

/*
 * Target fill level: we want the buffer roughly half full so there's room
 * to absorb bursts in both directions.  The emulator produces ~735 samples
 * per frame; the SDL callback consumes 1024 per pull.
 */
#define AUDIO_TARGET_FILL (AUDIO_BUF_SIZE / 2)

/* How much the sample rate can drift for sync. Tight window (±0.5%)
 * because the NES 60.0988 Hz vs display 60 Hz mismatch is only 0.16%,
 * so the absorption room is mostly for render-load hiccups. Larger
 * windows produce audible pitch warble on sustained notes. */
#define AUDIO_RATE_ADJUST_MAX 220  /* 44100 * 0.005 ≈ 220 samples/sec */

/* Integral rate-controller state. We sum the fill-level error over
 * time and nudge the sample rate by a fraction of the integrated
 * error, which gives zero-static-error tracking with a slow time
 * constant (no fast warble). */
typedef struct {
    double integral;       /* integrated fill-level error in sample*seconds */
    int    last_fill;      /* for rate-of-change damping */
} AudioRateCtrl;
static AudioRateCtrl audio_ctrl;

/* One producer (the emulation thread) and one consumer (SDL's audio
 * thread). Each side publishes its index with release after touching a
 * slot and reads the other's with acquire, so a slot is never read before
 * its sample is visible or overwritten before it was read. */
typedef struct {
    float buffer[AUDIO_BUF_SIZE];
    atomic_int write_pos;
    atomic_int read_pos;
    float last_sample;  /* held on underrun to avoid discontinuity */
} AudioRingBuffer;

static AudioRingBuffer audio_ring;

static void audio_ring_init(void) {
    memset(audio_ring.buffer, 0, sizeof(audio_ring.buffer));
    atomic_init(&audio_ring.write_pos, 0);
    atomic_init(&audio_ring.read_pos, 0);
    audio_ring.last_sample = 0.0f;
}

static inline int audio_ring_available(void) {
    int avail = atomic_load_explicit(&audio_ring.write_pos, memory_order_acquire)
              - atomic_load_explicit(&audio_ring.read_pos, memory_order_acquire);
    if (avail < 0) avail += AUDIO_BUF_SIZE;
    return avail;
}

static void audio_ring_push(float sample) {
    int write = atomic_load_explicit(&audio_ring.write_pos, memory_order_relaxed);
    int next = (write + 1) % AUDIO_BUF_SIZE;
    if (next != atomic_load_explicit(&audio_ring.read_pos, memory_order_acquire)) {
        audio_ring.buffer[write] = sample;
        atomic_store_explicit(&audio_ring.write_pos, next, memory_order_release);
    }
    /* If full, drop the sample — the dynamic rate adjust below will
       slow production so this rarely fires in steady state. */
}

/* SDL audio callback — runs on a dedicated audio thread */
static void sdl_audio_callback(void *userdata, Uint8 *stream, int len) {
    (void)userdata;
    float *out = (float *)stream;
    int samples = len / (int)sizeof(float);
    int read = atomic_load_explicit(&audio_ring.read_pos, memory_order_relaxed);

    for (int i = 0; i < samples; i++) {
        if (read != atomic_load_explicit(&audio_ring.write_pos, memory_order_acquire)) {
            float s = audio_ring.buffer[read];
            read = (read + 1) % AUDIO_BUF_SIZE;
            atomic_store_explicit(&audio_ring.read_pos, read, memory_order_release);
            audio_ring.last_sample = s;
            out[i] = s;
        } else {
            /* Underrun: hold last sample instead of silence to avoid pop */
            out[i] = audio_ring.last_sample;
        }
    }
}

/* APU callback — pushes one sample into the ring buffer */
static void apu_sample_callback(void *user_data, float sample) {
    (void)user_data;
    audio_ring_push(sample);
}

/* ============================================================================
 * OSD — on-screen debug overlay for NTSC tuning
 * ============================================================================
 *
 * Tiny 5×7 bitmap font rendered to a streaming RGBA texture that we blit
 * over the game view. Auto-hides after a short delay (no key activity).
 * Font covers uppercase letters, digits, and a handful of symbols — more
 * than enough for debug text. Each glyph is 7 bytes (rows), 5 bits wide
 * (bit 4 = leftmost pixel).
 */

#define OSD_W 640
#define OSD_H 200
#define OSD_VISIBLE_FRAMES 240   /* ~4 seconds at 60 Hz */

static SDL_Texture *osd_texture = NULL;
static uint8_t      osd_pixels[OSD_W * OSD_H * 4];
static int          osd_visible_frames = 0;
static bool         osd_pinned = false;  /* O key toggles "always visible" */

static const uint8_t osd_font5x7[128][7] = {
    [' '] = {0x00,0x00,0x00,0x00,0x00,0x00,0x00},
    ['0'] = {0x0E,0x11,0x13,0x15,0x19,0x11,0x0E},
    ['1'] = {0x04,0x0C,0x04,0x04,0x04,0x04,0x0E},
    ['2'] = {0x0E,0x11,0x01,0x06,0x08,0x10,0x1F},
    ['3'] = {0x1F,0x02,0x04,0x02,0x01,0x11,0x0E},
    ['4'] = {0x02,0x06,0x0A,0x12,0x1F,0x02,0x02},
    ['5'] = {0x1F,0x10,0x1E,0x01,0x01,0x11,0x0E},
    ['6'] = {0x06,0x08,0x10,0x1E,0x11,0x11,0x0E},
    ['7'] = {0x1F,0x01,0x02,0x04,0x08,0x08,0x08},
    ['8'] = {0x0E,0x11,0x11,0x0E,0x11,0x11,0x0E},
    ['9'] = {0x0E,0x11,0x11,0x0F,0x01,0x02,0x0C},
    ['A'] = {0x0E,0x11,0x11,0x1F,0x11,0x11,0x11},
    ['B'] = {0x1E,0x11,0x11,0x1E,0x11,0x11,0x1E},
    ['C'] = {0x0E,0x11,0x10,0x10,0x10,0x11,0x0E},
    ['D'] = {0x1E,0x11,0x11,0x11,0x11,0x11,0x1E},
    ['E'] = {0x1F,0x10,0x10,0x1E,0x10,0x10,0x1F},
    ['F'] = {0x1F,0x10,0x10,0x1E,0x10,0x10,0x10},
    ['G'] = {0x0E,0x11,0x10,0x17,0x11,0x11,0x0E},
    ['H'] = {0x11,0x11,0x11,0x1F,0x11,0x11,0x11},
    ['I'] = {0x0E,0x04,0x04,0x04,0x04,0x04,0x0E},
    ['J'] = {0x07,0x02,0x02,0x02,0x02,0x12,0x0C},
    ['K'] = {0x11,0x12,0x14,0x18,0x14,0x12,0x11},
    ['L'] = {0x10,0x10,0x10,0x10,0x10,0x10,0x1F},
    ['M'] = {0x11,0x1B,0x15,0x15,0x11,0x11,0x11},
    ['N'] = {0x11,0x11,0x19,0x15,0x13,0x11,0x11},
    ['O'] = {0x0E,0x11,0x11,0x11,0x11,0x11,0x0E},
    ['P'] = {0x1E,0x11,0x11,0x1E,0x10,0x10,0x10},
    ['Q'] = {0x0E,0x11,0x11,0x11,0x15,0x12,0x0D},
    ['R'] = {0x1E,0x11,0x11,0x1E,0x14,0x12,0x11},
    ['S'] = {0x0F,0x10,0x10,0x0E,0x01,0x01,0x1E},
    ['T'] = {0x1F,0x04,0x04,0x04,0x04,0x04,0x04},
    ['U'] = {0x11,0x11,0x11,0x11,0x11,0x11,0x0E},
    ['V'] = {0x11,0x11,0x11,0x11,0x11,0x0A,0x04},
    ['W'] = {0x11,0x11,0x11,0x15,0x15,0x15,0x0A},
    ['X'] = {0x11,0x11,0x0A,0x04,0x0A,0x11,0x11},
    ['Y'] = {0x11,0x11,0x11,0x0A,0x04,0x04,0x04},
    ['Z'] = {0x1F,0x01,0x02,0x04,0x08,0x10,0x1F},
    ['.'] = {0x00,0x00,0x00,0x00,0x00,0x00,0x04},
    [','] = {0x00,0x00,0x00,0x00,0x00,0x04,0x08},
    ['-'] = {0x00,0x00,0x00,0x1F,0x00,0x00,0x00},
    ['+'] = {0x00,0x04,0x04,0x1F,0x04,0x04,0x00},
    ['='] = {0x00,0x00,0x1F,0x00,0x1F,0x00,0x00},
    [':'] = {0x00,0x04,0x04,0x00,0x00,0x04,0x04},
    ['('] = {0x02,0x04,0x08,0x08,0x08,0x04,0x02},
    [')'] = {0x08,0x04,0x02,0x02,0x02,0x04,0x08},
    ['/'] = {0x01,0x02,0x04,0x04,0x04,0x08,0x10},
    ['%'] = {0x18,0x19,0x02,0x04,0x08,0x13,0x03},
    ['*'] = {0x00,0x0A,0x04,0x1F,0x04,0x0A,0x00},
    ['>'] = {0x10,0x08,0x04,0x02,0x04,0x08,0x10},
    ['<'] = {0x01,0x02,0x04,0x08,0x04,0x02,0x01},
    ['!'] = {0x04,0x04,0x04,0x04,0x00,0x00,0x04},
    ['_'] = {0x00,0x00,0x00,0x00,0x00,0x00,0x1F},
    ['#'] = {0x0A,0x0A,0x1F,0x0A,0x1F,0x0A,0x0A},
    ['?'] = {0x0E,0x11,0x01,0x02,0x04,0x00,0x04},
};

/* Scale at which font pixels are drawn: 2 = 10×14 per char, 3 = 15×21 */
#define OSD_FONT_SCALE 2
#define OSD_CHAR_W (5 * OSD_FONT_SCALE)
#define OSD_CHAR_H (7 * OSD_FONT_SCALE)
#define OSD_CHAR_ADVANCE (OSD_CHAR_W + OSD_FONT_SCALE)  /* column gap */
#define OSD_LINE_ADVANCE (OSD_CHAR_H + OSD_FONT_SCALE * 2)

static void osd_clear(void) {
    memset(osd_pixels, 0, sizeof(osd_pixels));
}

static void osd_set_pixel(int x, int y, uint32_t rgba) {
    if ((unsigned)x >= OSD_W || (unsigned)y >= OSD_H) return;
    int idx = (y * OSD_W + x) * 4;
    osd_pixels[idx + 0] = (uint8_t)(rgba >> 24);
    osd_pixels[idx + 1] = (uint8_t)(rgba >> 16);
    osd_pixels[idx + 2] = (uint8_t)(rgba >> 8);
    osd_pixels[idx + 3] = (uint8_t)(rgba);
}

static void osd_fill_rect(int x, int y, int w, int h, uint32_t rgba) {
    for (int dy = 0; dy < h; dy++)
        for (int dx = 0; dx < w; dx++)
            osd_set_pixel(x + dx, y + dy, rgba);
}

static void osd_draw_char(int x, int y, char c, uint32_t rgba) {
    if ((unsigned char)c >= 128) return;
    /* Fold lowercase → uppercase for the font table. */
    if (c >= 'a' && c <= 'z') c -= 32;
    const uint8_t *glyph = osd_font5x7[(int)(unsigned char)c];
    for (int row = 0; row < 7; row++) {
        uint8_t bits = glyph[row];
        for (int col = 0; col < 5; col++) {
            if (bits & (0x10 >> col)) {
                osd_fill_rect(x + col * OSD_FONT_SCALE, y + row * OSD_FONT_SCALE,
                              OSD_FONT_SCALE, OSD_FONT_SCALE, rgba);
            }
        }
    }
}

static void osd_draw_text(int x, int y, const char *text, uint32_t rgba) {
    int cx = x, cy = y;
    for (const char *p = text; *p; p++) {
        if (*p == '\n') {
            cx = x;
            cy += OSD_LINE_ADVANCE;
            continue;
        }
        osd_draw_char(cx, cy, *p, rgba);
        cx += OSD_CHAR_ADVANCE;
    }
}

static void osd_draw_panel(int x, int y, int w, int h, uint32_t bg_rgba) {
    osd_fill_rect(x, y, w, h, bg_rgba);
}

static void osd_kick(void) {
    osd_visible_frames = OSD_VISIBLE_FRAMES;
}

/* ============================================================================
 * OSD menu system — hierarchical, arrow-navigated tuning UI
 * ============================================================================
 *
 * Menu tables are static const and reference &composite.X directly. When a
 * user picks an item with Up/Down and adjusts with Left/Right, the
 * navigation code reads the item's type tag and dispatches. Submenus
 * are tiny stacks; Enter descends, Esc/Backspace ascends.
 */

typedef enum {
    MI_FLOAT,         /* adjustable float (clamped to [min, max]) */
    MI_INT,           /* adjustable int (clamped to [min, max]) */
    MI_INT_CYCLIC,    /* adjustable int (wraps at boundaries) */
    MI_TOGGLE,        /* int used as bool; Left/Right toggles */
    MI_SUBMENU,       /* Enter descends into `submenu` */
    MI_ACTION,        /* Enter calls `action()` */
} MenuItemType;

typedef struct MenuItem {
    const char *label;
    MenuItemType type;
    void       *target;
    float       step;
    float       min_val;
    float       max_val;
    void      (*on_change)(void);   /* called after any value edit */
    const struct MenuItem *submenu;
    int         submenu_count;
    void      (*action)(void);
    const char *format;             /* printf format for value display */
} MenuItem;

#define MENU_STACK_MAX 4

typedef struct {
    const MenuItem *items;
    int             count;
    int             selected;
    const char     *title;
} MenuLevel;

static MenuLevel menu_stack[MENU_STACK_MAX];
static int       menu_depth = 0;
static bool      menu_open  = false;
/* Menu background style: 1 = solid panel, 0 = transparent (dimmed
 * underlying game pixels). Bound to a menu item so the user can toggle. */
static int       menu_bg_solid = 1;

/* Performance stats overlay — replaces the old stdout printf that
 * caused per-second terminal-write jitter. The main loop updates
 * `perf_text` once per second; render_frame draws it into the NES
 * framebuffer alongside the menu so it gets the NTSC treatment.
 * Off by default — toggle with V or via the SETUP menu. */
static char      perf_text[96] = "";
static int       perf_overlay = 0;

/* --- Menu callbacks --- */

static void cb_redesign_firs(void) {
    comp_redesign_firs(&composite.signal);
}

static void cb_rebuild_gamma_lut(void) {
    comp_build_gamma_lut(&composite);
}

static void cb_apply_vsync(void) {
    /* Apply vsync toggle at runtime via SDL_RenderSetVSync (SDL 2.0.18+).
     * Silently no-ops if the renderer hasn't been created yet or the
     * driver doesn't support it. */
#if SDL_VERSION_ATLEAST(2, 0, 18)
    if (renderer) SDL_RenderSetVSync(renderer, vsync_enabled ? 1 : 0);
#endif
}

static void palette_refresh_composite_derived(void);   /* fwd decl */

static void cb_apply_region(void) {
    /* composite.signal.region was toggled by the menu's Left/Right
     * handler (MI_TOGGLE flips 0 ↔ 1). Feed that back through
     * comp_set_region so the signal table, FIR cutoffs, and output
     * color matrix are rebuilt for the new region. */
    int target_region = composite.signal.region ? COMP_REGION_PAL
                                                 : COMP_REGION_NTSC;
    comp_set_region(&composite, target_region);
    /* Recompute the derived PAL palette so the wave-off fallback
     * stays in sync with the (possibly new) decoder params. */
    palette_refresh_composite_derived();
    printf("Composite region: %s\n",
           target_region == COMP_REGION_PAL ? "PAL (2C07 / YUV)"
                                            : "NTSC (2C02 / YIQ)");
    fflush(stdout);
}

static void comp_reset_tuning(void);           /* forward */
static void menu_action_reset(void) { comp_reset_tuning(); }

/* ============================================================================
 * NTSC tuning presets
 * ============================================================================
 * A "preset" is a bundle of every tunable NTSC parameter that produces
 * a specific look: from a clean RGB-ish monitor to a cheap worn-out
 * 80s TV set. The user can apply them from the Presets submenu. */

typedef struct {
    const char *name;
    /* Signal region — COMP_REGION_NTSC or COMP_REGION_PAL. Applying a
     * preset also flips the composite pipeline to this region so a
     * PAL preset automatically switches the signal table, FIRs, and
     * output color matrix. */
    int   region;
    /* Post-processing effects */
    float luma_afterglow;
    float luma_ringing;
    float chroma_ringing;    /* new — composite chroma overshoot */
    float bloom;             /* 1D Y-channel bloom (legacy) */
    float bloom_2d;          /* new — full-frame 2D radial halo */
    float vignette;
    float aperture_grille;   /* now: RGB stripe phosphor mask strength */
    float chromatic_conv;    /* new — R/B beam misalignment */
    float gamma;             /* new — display gamma curve */
    float persistence;       /* new — vertical phosphor afterglow */
    float barrel;            /* new — CRT glass curvature */
    /* Signal-path analog effects */
    float hsync_wobble;      /* H-sync jitter */
    float ghosting;          /* Multipath reflection intensity */
    int   ghost_offset;      /* Ghost delay in output px */
    float snow;              /* RF luma noise */
    float hum_bar;           /* 60 Hz hum bar */
    int   black_floor;       /* Ambient light black level raise */
    int   svideo;            /* S-Video dispatch (NTSC only) */
    /* Color controls */
    float hue_deg;
    float saturation;
    float brightness;
    float contrast;
    /* Chroma phase */
    int   phase_line_adv;
    int   phase_field_adv;
    int   phase_num_fields;
    int   demod_rotate;
    float chroma_gain;
    /* Filters */
    float y_cutoff;
    float c_cutoff;
    int   fir_y_n;
    int   fir_c_n;
    int   comb_filter;
    float color_killer;
} CompositePreset;

static const CompositePreset comp_presets[] = {
    /* 0. Clean (RGB) — minimal artifacts, pixel-sharp. Everything new
     *    stays off; this is the "no CRT character" baseline. gamma=1.0
     *    is identity so the LUT is a no-op. S-Video ON so the composite
     *    waveform chain is bypassed entirely — the only way to get
     *    truly zero cross-color on sharp edges. */
    {
        .name = "Clean (RGB)",
        .region = COMP_REGION_NTSC,
        .luma_afterglow = 0.00f, .luma_ringing = 0.00f,
        .chroma_ringing = 0.00f,
        .bloom = 0.00f, .bloom_2d = 0.00f,
        .vignette = 0.00f, .aperture_grille = 0.00f,
        .chromatic_conv = 0.00f, .gamma = 1.00f,
        .persistence = 0.00f, .barrel = 0.00f, .svideo = 1,
        .hue_deg = 0.0f, .saturation = 1.00f,
        .brightness = 0.00f, .contrast = 1.00f,
        .phase_line_adv = 6, .phase_field_adv = 0, .phase_num_fields = 1,
        .demod_rotate = 4, .chroma_gain = 1.30f,
        .y_cutoff = 0.080f, .c_cutoff = 0.030f,
        .fir_y_n = 21, .fir_c_n = 15,
        .comb_filter = 1, .color_killer = 0.03f,
    },
    /* 1. PVM — professional broadcast monitor. The comb filter
     *    handles Y/C separation, which frees the Y FIR from having
     *    to kill the subcarrier — so y_cutoff can be LOOSE (high)
     *    and pixel edges stay crisp. Minimal bloom/afterglow, gentle
     *    scanlines, calibrated color, punchy contrast. Looks MUCH
     *    sharper than a consumer TV, as it should. No phosphor mask,
     *    no gamma — this is the "calibrated reference" preset. */
    {
        .name = "PVM",
        .region = COMP_REGION_NTSC,
        .luma_afterglow = 0.05f, .luma_ringing = 0.35f,
        .chroma_ringing = 0.00f,
        .bloom = 0.10f, .bloom_2d = 0.00f,
        .vignette = 0.08f, .aperture_grille = 0.00f,
        .chromatic_conv = 0.00f, .gamma = 1.00f,
        .persistence = 0.00f, .barrel = 0.00f, .svideo = 0,
        .hue_deg = 0.0f, .saturation = 1.00f,
        .brightness = 0.00f, .contrast = 1.10f,
        .phase_line_adv = 6, .phase_field_adv = 6, .phase_num_fields = 4,
        .demod_rotate = 4, .chroma_gain = 1.20f,
        .y_cutoff = 0.080f, .c_cutoff = 0.025f,   /* loose — comb kills subcarrier */
        .fir_y_n = 21, .fir_c_n = 25,             /* shorter filters OK */
        .comb_filter = 1, .color_killer = 0.03f,
    },
    /* 2. Consumer TV — typical 80s/90s Trinitron-ish, soft colors,
     *    moderate bloom, audible scanlines. Picks up a mild 2D bloom,
     *    touch of persistence, and slight gamma to sell the "warm TV"
     *    feel without going all the way to Bad TV. */
    {
        .name = "Consumer TV",
        .region = COMP_REGION_NTSC,
        .luma_afterglow = 0.25f, .luma_ringing = 0.40f,
        .chroma_ringing = 0.15f,
        .bloom = 0.40f, .bloom_2d = 0.20f,
        .vignette = 0.35f, .aperture_grille = 0.00f,
        .chromatic_conv = 0.10f, .gamma = 1.10f,
        .persistence = 0.10f, .barrel = 0.00f, .svideo = 0,
        .black_floor = 8,
        .hue_deg = 0.0f, .saturation = 0.90f,
        .brightness = 0.00f, .contrast = 1.00f,
        .phase_line_adv = 6, .phase_field_adv = 6, .phase_num_fields = 4,
        .demod_rotate = 4, .chroma_gain = 1.30f,
        .y_cutoff = 0.043f, .c_cutoff = 0.015f,
        .fir_y_n = 37, .fir_c_n = 47,
        .comb_filter = 0, .color_killer = 0.05f,
    },
    /* 3. Bad TV — aging consumer set in the corner of the basement.
     *    Everything is wrong: blurry luma, bleeding chroma, heavy
     *    bloom + halo, slow phosphor, badly converged beams, dim
     *    corners, slight barrel. aperture_grille dropped from 0.30
     *    (old uniform-darken) to 0.18 now that it's a real RGB
     *    stripe mask — 0.30 reads as an obvious candy pattern with
     *    the new semantics. */
    {
        .name = "Bad TV",
        .region = COMP_REGION_NTSC,
        .luma_afterglow = 0.80f, .luma_ringing = 0.55f,
        .chroma_ringing = 0.45f,
        .bloom = 0.95f, .bloom_2d = 0.50f,
        .vignette = 0.70f, .aperture_grille = 0.18f,
        .chromatic_conv = 0.50f, .gamma = 0.92f,
        .persistence = 0.35f, .barrel = 0.20f, .svideo = 0,
        .hsync_wobble = 0.30f, .ghosting = 0.15f, .ghost_offset = 10,
        .snow = 0.15f, .hum_bar = 0.10f, .black_floor = 12,
        .hue_deg = -8.0f, .saturation = 0.70f,
        .brightness = -0.05f, .contrast = 0.88f,
        .phase_line_adv = 6, .phase_field_adv = 4, .phase_num_fields = 3,
        .demod_rotate = 4, .chroma_gain = 1.80f,
        .y_cutoff = 0.022f, .c_cutoff = 0.008f,
        .fir_y_n = 41, .fir_c_n = 47,
        .comb_filter = 0, .color_killer = 0.00f,
    },
    /* 4. RF Modulator — NES antenna RF modulator on channel 3. RF
     *    severely reduces horizontal bandwidth, smears color
     *    horizontally, adds warm tint, persistent ghosting,
     *    misconverged beams. Small grille + heavy barrel +
     *    lifted gamma because the thing is a mess. aperture_grille
     *    tuned down from 0.20 to 0.12 for the new stripe semantics. */
    {
        .name = "RF Modulator",
        .region = COMP_REGION_NTSC,
        .luma_afterglow = 0.70f, .luma_ringing = 0.20f,
        .chroma_ringing = 0.60f,
        .bloom = 0.90f, .bloom_2d = 0.45f,
        .vignette = 0.50f, .aperture_grille = 0.12f,
        .chromatic_conv = 0.65f, .gamma = 0.88f,
        .persistence = 0.45f, .barrel = 0.25f, .svideo = 0,
        .hsync_wobble = 0.40f, .ghosting = 0.25f, .ghost_offset = 8,
        .snow = 0.30f, .hum_bar = 0.15f, .black_floor = 10,
        .hue_deg = 10.0f, .saturation = 0.60f,
        .brightness = -0.05f, .contrast = 0.82f,
        .phase_line_adv = 6, .phase_field_adv = 3, .phase_num_fields = 3,
        .demod_rotate = 4, .chroma_gain = 1.55f,  /* RF demod chips over-saturated */
        .y_cutoff = 0.018f, .c_cutoff = 0.006f,
        .fir_y_n = 47, .fir_c_n = 47,
        .comb_filter = 0, .color_killer = 0.00f,
    },
    /* 5. Trinitron PVM — pro Sony BVM/PVM with a visible aperture
     *    grille. This is the preset that specifically targets Retina
     *    displays: aperture_grille = 0.40 at 4× horizontal renders
     *    one RGB triad per 3 output pixels, and on Retina those 3
     *    pixels map to ~6 physical pixels — honest phosphor cells.
     *    Comb on, minimal bloom, gentle halo, subtle misconvergence,
     *    flat glass (no barrel), calibrated gamma. The SHARP preset. */
    {
        .name = "Trinitron PVM",
        .region = COMP_REGION_NTSC,
        .luma_afterglow = 0.03f, .luma_ringing = 0.35f,
        .chroma_ringing = 0.08f,
        .bloom = 0.08f, .bloom_2d = 0.12f,
        .vignette = 0.08f, .aperture_grille = 0.40f,
        .chromatic_conv = 0.15f, .gamma = 1.00f,
        .persistence = 0.06f, .barrel = 0.00f, .svideo = 0,
        .hue_deg = 0.0f, .saturation = 1.00f,
        .brightness = 0.00f, .contrast = 1.12f,
        .phase_line_adv = 6, .phase_field_adv = 6, .phase_num_fields = 4,
        .demod_rotate = 4, .chroma_gain = 1.20f,
        .y_cutoff = 0.080f, .c_cutoff = 0.025f,   /* comb handles subcarrier */
        .fir_y_n = 21, .fir_c_n = 25,
        .comb_filter = 1, .color_killer = 0.03f,
    },
    /* 6. Arcade Monitor — Wells Gardner K7000 / Tri-Sync vibe. Dark
     *    room, bright glowy phosphor, long afterglow + persistence,
     *    strong 2D bloom for that "neon inside a cabinet" halo, curved
     *    glass, noticeably punched gamma for the high-contrast look
     *    arcade CRTs had. Moderate phosphor mask so you can tell it's
     *    a CRT without it looking candy-coated. */
    {
        .name = "Arcade Monitor",
        .region = COMP_REGION_NTSC,
        .luma_afterglow = 0.30f, .luma_ringing = 0.30f,
        .chroma_ringing = 0.20f,
        .bloom = 0.55f, .bloom_2d = 0.55f,
        .vignette = 0.45f, .aperture_grille = 0.30f,
        .chromatic_conv = 0.25f, .gamma = 1.20f,
        .persistence = 0.35f, .barrel = 0.20f, .svideo = 0,
        .black_floor = 4,
        .hue_deg = 0.0f, .saturation = 1.10f,      /* punchy arcade */
        .brightness = 0.04f, .contrast = 1.15f,
        .phase_line_adv = 6, .phase_field_adv = 6, .phase_num_fields = 4,
        .demod_rotate = 4, .chroma_gain = 1.30f,
        .y_cutoff = 0.065f, .c_cutoff = 0.018f,
        .fir_y_n = 25, .fir_c_n = 37,
        .comb_filter = 0, .color_killer = 0.03f,
    },
    /* 7. VHS Deck — home-recorded tape played back through a cable
     *    box, worst-case composite pipeline. Heavy chroma ringing
     *    halos on saturated edges, really bad R/B convergence, slow
     *    phosphor with significant persistence (tape smear), lifted
     *    gamma, barrel, dim/yellow color. The "playing Mario on VHS
     *    at a friend's house" aesthetic. */
    {
        .name = "VHS Deck",
        .region = COMP_REGION_NTSC,
        .luma_afterglow = 0.60f, .luma_ringing = 0.25f,
        .chroma_ringing = 0.75f,
        .bloom = 0.80f, .bloom_2d = 0.40f,
        .vignette = 0.55f, .aperture_grille = 0.10f,
        .chromatic_conv = 0.70f, .gamma = 0.92f,
        .persistence = 0.55f, .barrel = 0.25f, .svideo = 0,
        .hsync_wobble = 0.55f, .ghosting = 0.10f, .ghost_offset = 6,
        .snow = 0.20f, .hum_bar = 0.08f, .black_floor = 10,
        .hue_deg = -5.0f, .saturation = 0.65f,
        .brightness = -0.03f, .contrast = 0.85f,
        .phase_line_adv = 6, .phase_field_adv = 3, .phase_num_fields = 3,
        .demod_rotate = 4, .chroma_gain = 1.50f,
        .y_cutoff = 0.020f, .c_cutoff = 0.007f,
        .fir_y_n = 47, .fir_c_n = 47,
        .comb_filter = 0, .color_killer = 0.00f,
    },
    /* 8. S-Video Sharp — the killer Retina preset: S-Video mode ON
     *    (composite chain bypassed, zero dot crawl, pin-sharp pixels),
     *    strong phosphor mask so the aperture grille resolves honestly
     *    on a Retina display, touch of 2D bloom and vignette for CRT
     *    character. This is what a real Sony BVM with S-Video input
     *    on a Retina monitor looks like. */
    {
        .name = "S-Video Sharp",
        .region = COMP_REGION_NTSC,
        .luma_afterglow = 0.08f, .luma_ringing = 0.25f,
        .chroma_ringing = 0.00f,   /* no composite → no chroma ringing */
        .bloom = 0.12f, .bloom_2d = 0.22f,
        .vignette = 0.15f, .aperture_grille = 0.45f,
        .chromatic_conv = 0.10f, .gamma = 1.08f,
        .persistence = 0.10f, .barrel = 0.05f, .svideo = 1,
        .hue_deg = 0.0f, .saturation = 1.00f,
        .brightness = 0.00f, .contrast = 1.05f,
        /* Phase / filters mostly irrelevant on the S-Video path (it
         * bypasses comp_process_waveform entirely), but set them to
         * PVM values so toggling svideo OFF inside the menu gives a
         * sane fallback instead of whatever stale state. */
        .phase_line_adv = 6, .phase_field_adv = 6, .phase_num_fields = 4,
        .demod_rotate = 4, .chroma_gain = 1.20f,
        .y_cutoff = 0.080f, .c_cutoff = 0.025f,
        .fir_y_n = 21, .fir_c_n = 25,
        .comb_filter = 1, .color_killer = 0.03f,
    },
    /* 9. Cursed CRT — the "haunted basement TV" maximum-effects
     *    preset. Every knob cranked: severe persistence, strong
     *    2D halo, max chroma ringing, heavy barrel, low gamma,
     *    visible grille, miscolored everything. Intentionally
     *    over-the-top. Useful as a sanity test that all the new
     *    effects ARE actually wired up, and as an aesthetic for the
     *    right kind of game. */
    {
        .name = "Cursed CRT",
        .region = COMP_REGION_NTSC,
        .luma_afterglow = 0.75f, .luma_ringing = 0.45f,
        .chroma_ringing = 0.90f,
        .bloom = 0.90f, .bloom_2d = 0.75f,
        .vignette = 0.70f, .aperture_grille = 0.35f,
        .chromatic_conv = 0.85f, .gamma = 0.85f,
        .persistence = 0.65f, .barrel = 0.35f, .svideo = 0,
        .hsync_wobble = 0.70f, .ghosting = 0.35f, .ghost_offset = 12,
        .snow = 0.40f, .hum_bar = 0.25f, .black_floor = 15,
        .hue_deg = -12.0f, .saturation = 0.80f,
        .brightness = -0.03f, .contrast = 0.92f,
        .phase_line_adv = 6, .phase_field_adv = 2, .phase_num_fields = 3,
        .demod_rotate = 4, .chroma_gain = 1.70f,
        .y_cutoff = 0.025f, .c_cutoff = 0.008f,
        .fir_y_n = 41, .fir_c_n = 47,
        .comb_filter = 0, .color_killer = 0.00f,
    },
    /* 10. Mono TV — a black-and-white set. Some households had old
     *     B&W TVs well into the NES era. saturation = 0 kills all
     *     chroma so you see only the Y-channel luminance decode.
     *     Mild bloom, moderate vignette, slight persistence for that
     *     warm grey phosphor look. black_floor raised a bit because
     *     B&W tubes had dim ambient blacks. */
    {
        .name = "Mono TV",
        .region = COMP_REGION_NTSC,
        .luma_afterglow = 0.30f, .luma_ringing = 0.30f,
        .chroma_ringing = 0.00f,
        .bloom = 0.30f, .bloom_2d = 0.15f,
        .vignette = 0.40f, .aperture_grille = 0.00f,
        .chromatic_conv = 0.00f, .gamma = 1.10f,
        .persistence = 0.15f, .barrel = 0.10f, .svideo = 0,
        .black_floor = 12,
        .hue_deg = 0.0f, .saturation = 0.00f,   /* zero saturation = B&W */
        .brightness = 0.02f, .contrast = 1.05f,
        .phase_line_adv = 6, .phase_field_adv = 6, .phase_num_fields = 4,
        .demod_rotate = 4, .chroma_gain = 1.30f,
        .y_cutoff = 0.050f, .c_cutoff = 0.015f,
        .fir_y_n = 31, .fir_c_n = 47,
        .comb_filter = 0, .color_killer = 0.00f,
    },
    /* 11. Commodore 1702 — the iconic 80s composite monitor. Sharper
     *     than a consumer TV but not as clinical as a PVM. No comb
     *     filter (it was a composite-only monitor), warm and slightly
     *     soft, moderate bloom. The "good-but-not-pro" monitor many
     *     NES owners aspired to. */
    {
        .name = "Commodore 1702",
        .region = COMP_REGION_NTSC,
        .luma_afterglow = 0.15f, .luma_ringing = 0.35f,
        .chroma_ringing = 0.10f,
        .bloom = 0.20f, .bloom_2d = 0.10f,
        .vignette = 0.20f, .aperture_grille = 0.00f,
        .chromatic_conv = 0.05f, .gamma = 1.05f,
        .persistence = 0.05f, .barrel = 0.00f, .svideo = 0,
        .black_floor = 6,
        .hue_deg = 0.0f, .saturation = 0.95f,
        .brightness = 0.00f, .contrast = 1.05f,
        .phase_line_adv = 6, .phase_field_adv = 6, .phase_num_fields = 4,
        .demod_rotate = 4, .chroma_gain = 1.25f,
        .y_cutoff = 0.060f, .c_cutoff = 0.020f,
        .fir_y_n = 27, .fir_c_n = 35,
        .comb_filter = 0, .color_killer = 0.04f,
    },
    /* 12. Sony Wega — late 90s flat-tube consumer TV. Comb filter
     *     on (these sets had decent Y/C separation), sharp luma,
     *     minimal bloom, visible aperture grille in the Trinitron
     *     tradition but less pronounced than a PVM. Slight chromatic
     *     convergence at corners (flat tubes were better than curved
     *     ones but not perfect). The "last great CRT" generation. */
    {
        .name = "Sony Wega",
        .region = COMP_REGION_NTSC,
        .luma_afterglow = 0.08f, .luma_ringing = 0.30f,
        .chroma_ringing = 0.05f,
        .bloom = 0.12f, .bloom_2d = 0.10f,
        .vignette = 0.15f, .aperture_grille = 0.20f,
        .chromatic_conv = 0.10f, .gamma = 1.05f,
        .persistence = 0.05f, .barrel = 0.00f, .svideo = 0,  /* flat tube */
        .black_floor = 6,
        .hue_deg = 0.0f, .saturation = 1.00f,
        .brightness = 0.00f, .contrast = 1.08f,
        .phase_line_adv = 6, .phase_field_adv = 6, .phase_num_fields = 4,
        .demod_rotate = 4, .chroma_gain = 1.25f,
        .y_cutoff = 0.070f, .c_cutoff = 0.022f,
        .fir_y_n = 23, .fir_c_n = 29,
        .comb_filter = 1, .color_killer = 0.04f,
    },
    /* 13. PAL Consumer TV — 80s/90s European set. 50 Hz field rate
     *     means slightly longer phosphor afterglow is visible. Wide
     *     C FIR for that "soft PAL chroma" look. demod_rotate = 3
     *     is the empirically-correct YUV reference for 2C07 — see
     *     comp_set_region for the rationale.
     *
     *     Note: PAL always runs through PAL-CRT (the vendored
     *     2C07 encoder/decoder) which has its own saturation /
     *     contrast / brightness knobs. Our color controls + YIQ
     *     effects don't apply on the PAL branch; the NTSC-style
     *     per-row effects (bloom, vignette, mask, convergence,
     *     gamma, persistence, barrel) DO apply because PAL-CRT
     *     feeds its output through comp_emit_output_row_rgb. */
    {
        .name = "PAL Consumer TV",
        .region = COMP_REGION_PAL,
        .luma_afterglow = 0.30f, .luma_ringing = 0.35f,
        .chroma_ringing = 0.00f,
        .bloom = 0.35f, .bloom_2d = 0.18f,
        .vignette = 0.30f, .aperture_grille = 0.00f,
        .chromatic_conv = 0.08f, .gamma = 1.10f,
        .persistence = 0.12f, .barrel = 0.00f, .svideo = 0,
        .black_floor = 8,
        .hue_deg = 0.0f, .saturation = 0.95f,
        .brightness = 0.00f, .contrast = 1.00f,
        .phase_line_adv = 0, .phase_field_adv = 0, .phase_num_fields = 1,
        .demod_rotate = 3, .chroma_gain = 1.20f,
        .y_cutoff = 0.040f, .c_cutoff = 0.012f,
        .fir_y_n = 37, .fir_c_n = 51,
        .comb_filter = 0, .color_killer = 0.05f,
    },
    /* 11. PAL PVM — broadcast reference monitor in PAL mode. Comb
     *     on, sharper luma. Minimal post-processing. Good for
     *     color-critical work on European content. */
    {
        .name = "PAL PVM",
        .region = COMP_REGION_PAL,
        .luma_afterglow = 0.05f, .luma_ringing = 0.30f,
        .chroma_ringing = 0.00f,
        .bloom = 0.08f, .bloom_2d = 0.00f,
        .vignette = 0.08f, .aperture_grille = 0.00f,
        .chromatic_conv = 0.00f, .gamma = 1.00f,
        .persistence = 0.00f, .barrel = 0.00f, .svideo = 0,
        .hue_deg = 0.0f, .saturation = 1.00f,
        .brightness = 0.00f, .contrast = 1.08f,
        .phase_line_adv = 0, .phase_field_adv = 0, .phase_num_fields = 1,
        .demod_rotate = 3, .chroma_gain = 1.15f,
        .y_cutoff = 0.080f, .c_cutoff = 0.022f,
        .fir_y_n = 21, .fir_c_n = 29,
        .comb_filter = 1, .color_killer = 0.03f,
    },
    /* 12. PAL Trinitron — PAL counterpart of the Trinitron PVM preset.
     *     Visible aperture grille, subtle misconvergence, calibrated
     *     gamma. The Retina showcase for PAL users. */
    {
        .name = "PAL Trinitron",
        .region = COMP_REGION_PAL,
        .luma_afterglow = 0.05f, .luma_ringing = 0.30f,
        .chroma_ringing = 0.00f,
        .bloom = 0.10f, .bloom_2d = 0.15f,
        .vignette = 0.10f, .aperture_grille = 0.40f,
        .chromatic_conv = 0.15f, .gamma = 1.00f,
        .persistence = 0.08f, .barrel = 0.00f, .svideo = 0,
        .hue_deg = 0.0f, .saturation = 1.00f,
        .brightness = 0.00f, .contrast = 1.10f,
        .phase_line_adv = 0, .phase_field_adv = 0, .phase_num_fields = 1,
        .demod_rotate = 3, .chroma_gain = 1.18f,
        .y_cutoff = 0.080f, .c_cutoff = 0.022f,
        .fir_y_n = 21, .fir_c_n = 29,
        .comb_filter = 1, .color_killer = 0.03f,
    },
    /* 16. CRT Match — tuned against the IMG_1766 reference photo of a
     *     real consumer CRT running the Metroid-style boss scene from
     *     IMG_1765. Characteristics copied from the photo:
     *       - Visible aperture-grille phosphor texture (strong but not
     *         candy-striped); horizontal scanlines modulated by it.
     *       - Noticeable bloom on bright highlights (orange circles,
     *         pedestal whites) with full-frame 2D halo.
     *       - Warmish gamma curve, slight desaturation, slight blue
     *         shift in deep shadow (halation + glass ambient).
     *       - Soft composite luma+chroma (narrow FIRs), no comb filter
     *         — the photo shows chroma bleed and a touch of dot crawl.
     *       - Mild chromatic convergence and a touch of barrel. */
    {
        .name = "CRT Match",
        .region = COMP_REGION_NTSC,
        .luma_afterglow = 0.10f, .luma_ringing = 0.25f,
        .chroma_ringing = 0.20f,
        .bloom = 0.60f, .bloom_2d = 0.25f,
        .vignette = 0.08f, .aperture_grille = 0.20f,
        .chromatic_conv = 0.06f, .gamma = 0.88f,
        .persistence = 0.15f, .barrel = 0.04f, .svideo = 0,
        .hsync_wobble = 0.02f, .ghosting = 0.00f, .ghost_offset = 0,
        .snow = 0.02f, .hum_bar = 0.00f, .black_floor = 4,
        .hue_deg = 0.0f, .saturation = 1.10f,
        .brightness = 0.18f, .contrast = 1.40f,
        .phase_line_adv = 6, .phase_field_adv = 6, .phase_num_fields = 4,
        .demod_rotate = 4, .chroma_gain = 1.35f,
        .y_cutoff = 0.050f, .c_cutoff = 0.014f,
        .fir_y_n = 29, .fir_c_n = 37,
        .comb_filter = 0, .color_killer = 0.03f,
    },
};
#define COMP_PRESET_CRT_MATCH 16

static void comp_apply_preset(const CompositePreset *p) {
    /* Switch region FIRST — comp_set_region rebuilds the signal table
     * and FIR coefficients, so any tuning we write after it sticks. */
    comp_set_region(&composite, p->region);
    composite.luma_afterglow  = p->luma_afterglow;
    composite.luma_ringing    = p->luma_ringing;
    composite.chroma_ringing  = p->chroma_ringing;
    composite.bloom           = p->bloom;
    composite.bloom_2d        = p->bloom_2d;
    composite.vignette        = p->vignette;
    composite.aperture_grille = p->aperture_grille;
    composite.chromatic_conv  = p->chromatic_conv;
    composite.gamma           = p->gamma;
    composite.persistence     = p->persistence;
    composite.barrel          = p->barrel;
    composite.hsync_wobble    = p->hsync_wobble;
    composite.ghosting        = p->ghosting;
    composite.ghost_offset    = p->ghost_offset ? p->ghost_offset : 8;
    composite.snow            = p->snow;
    composite.hum_bar         = p->hum_bar;
    composite.black_floor     = p->black_floor;
    composite.hue_deg         = p->hue_deg;
    composite.saturation      = p->saturation;
    composite.brightness      = p->brightness;
    composite.contrast        = p->contrast;
    composite.signal.phase_base       = 0;
    composite.signal.phase_line_adv   = p->phase_line_adv;
    composite.signal.phase_field_adv  = p->phase_field_adv;
    composite.signal.phase_num_fields = p->phase_num_fields;
    composite.signal.demod_rotate     = p->demod_rotate;
    composite.signal.chroma_gain      = p->chroma_gain;
    composite.signal.y_cutoff         = p->y_cutoff;
    composite.signal.c_cutoff         = p->c_cutoff;
    composite.signal.fir_y_n          = p->fir_y_n;
    composite.signal.fir_c_n          = p->fir_c_n;
    composite.signal.comb_filter      = p->comb_filter;
    composite.signal.color_killer     = p->color_killer;
    composite.signal.svideo           = p->svideo;
    comp_redesign_firs(&composite.signal);
    comp_build_gamma_lut(&composite);
    /* Drop any stale vertical persistence state so the next frame
     * doesn't ghost from the OLD preset's snapshot. */
    if (composite.output_prev) {
        memset(composite.output_prev, 0,
               (size_t)(composite.out_w * 240 * composite.rows_per_scanline * 3));
    }
    printf("Composite preset: %s (%s)\n", p->name,
           p->region == COMP_REGION_PAL ? "PAL" : "NTSC");
    fflush(stdout);
}

static void menu_action_preset_clean(void)     { comp_apply_preset(&comp_presets[0]); }
static void menu_action_preset_pvm(void)       { comp_apply_preset(&comp_presets[1]); }
static void menu_action_preset_consumer(void)  { comp_apply_preset(&comp_presets[2]); }
static void menu_action_preset_bad(void)       { comp_apply_preset(&comp_presets[3]); }
static void menu_action_preset_rf(void)        { comp_apply_preset(&comp_presets[4]); }
static void menu_action_preset_trinitron(void) { comp_apply_preset(&comp_presets[5]); }
static void menu_action_preset_arcade(void)    { comp_apply_preset(&comp_presets[6]); }
static void menu_action_preset_vhs(void)       { comp_apply_preset(&comp_presets[7]); }
static void menu_action_preset_svideo(void)    { comp_apply_preset(&comp_presets[8]); }
static void menu_action_preset_cursed(void)    { comp_apply_preset(&comp_presets[9]); }
static void menu_action_preset_mono(void)      { comp_apply_preset(&comp_presets[10]); }
static void menu_action_preset_1702(void)      { comp_apply_preset(&comp_presets[11]); }
static void menu_action_preset_wega(void)      { comp_apply_preset(&comp_presets[12]); }
static void menu_action_preset_pal_tv(void)    { comp_apply_preset(&comp_presets[13]); }
static void menu_action_preset_pal_pvm(void)   { comp_apply_preset(&comp_presets[14]); }
static void menu_action_preset_pal_trin(void)  { comp_apply_preset(&comp_presets[15]); }
static void menu_action_preset_crt_match(void) { comp_apply_preset(&comp_presets[COMP_PRESET_CRT_MATCH]); }

/* --- Menu tables ---
 * composite is a file-scope static, so its fields are valid address
 * constants for static initializers. */

static const MenuItem menu_phase[] = {
    { "Line advance",   MI_INT,        &composite.signal.phase_line_adv,   1.0f, -12.0f, 12.0f, NULL, NULL, 0, NULL, "%+d" },
    { "Field advance",  MI_INT,        &composite.signal.phase_field_adv,  1.0f, -12.0f, 12.0f, NULL, NULL, 0, NULL, "%+d" },
    { "Num fields",     MI_INT,        &composite.signal.phase_num_fields, 1.0f,   1.0f, 12.0f, NULL, NULL, 0, NULL, "%d"  },
    { "Base phase",     MI_INT_CYCLIC, &composite.signal.phase_base,       1.0f,   0.0f, 11.0f, NULL, NULL, 0, NULL, "%d / 12" },
    { "Demod rotate",   MI_INT_CYCLIC, &composite.signal.demod_rotate,     1.0f, -12.0f, 12.0f, NULL, NULL, 0, NULL, "%+d / 12" },
    { "Chroma gain",    MI_FLOAT,      &composite.signal.chroma_gain,      0.10f,  0.0f,  5.0f, NULL, NULL, 0, NULL, "%.2f" },
    { "Color killer",   MI_FLOAT,      &composite.signal.color_killer,     0.01f,  0.0f,  0.30f, NULL, NULL, 0, NULL, "%.2f" },
};

static const MenuItem menu_filters[] = {
    { "Y cutoff",    MI_FLOAT,  &composite.signal.y_cutoff,    0.005f, 0.005f, 0.200f, cb_redesign_firs, NULL, 0, NULL, "%.3f" },
    { "Y taps",      MI_INT,    &composite.signal.fir_y_n,     2.0f,   3.0f,  63.0f,  cb_redesign_firs, NULL, 0, NULL, "%d" },
    { "C cutoff",    MI_FLOAT,  &composite.signal.c_cutoff,    0.002f, 0.005f, 0.200f, cb_redesign_firs, NULL, 0, NULL, "%.3f" },
    { "C taps",      MI_INT,    &composite.signal.fir_c_n,     2.0f,   3.0f,  63.0f,  cb_redesign_firs, NULL, 0, NULL, "%d" },
    { "Comb filter", MI_TOGGLE, &composite.signal.comb_filter, 0, 0, 0, NULL, NULL, 0, NULL, NULL },
    { "S-Video mode",MI_TOGGLE, &composite.signal.svideo,      0, 0, 0, NULL, NULL, 0, NULL, NULL },
};

static const MenuItem menu_color[] = {
    { "Hue",        MI_FLOAT, &composite.hue_deg,    2.0f,  -45.0f, 45.0f, NULL, NULL, 0, NULL, "%+.1f" },
    { "Saturation", MI_FLOAT, &composite.saturation, 0.05f,  0.0f,   3.0f, NULL, NULL, 0, NULL, "%.2f"  },
    { "Brightness", MI_FLOAT, &composite.brightness, 0.02f, -1.0f,   1.0f, NULL, NULL, 0, NULL, "%+.2f" },
    { "Contrast",   MI_FLOAT, &composite.contrast,   0.02f,  0.1f,   3.0f, NULL, NULL, 0, NULL, "%.2f"  },
};

static const MenuItem menu_effects[] = {
    { "Luma lowpass",   MI_FLOAT,  &composite.luma_lowpass,    0.05f, 0.0f, 1.0f, NULL, NULL, 0, NULL, "%.2f" },
    { "Chroma lowpass", MI_FLOAT,  &composite.chroma_lowpass,  0.05f, 0.0f, 1.0f, NULL, NULL, 0, NULL, "%.2f" },
    { "Afterglow",      MI_FLOAT,  &composite.luma_afterglow,  0.05f, 0.0f, 1.0f, NULL, NULL, 0, NULL, "%.2f" },
    { "Luma ringing",   MI_FLOAT,  &composite.luma_ringing,    0.05f, 0.0f, 1.0f, NULL, NULL, 0, NULL, "%.2f" },
    { "Chroma ringing", MI_FLOAT,  &composite.chroma_ringing,  0.05f, 0.0f, 1.0f, NULL, NULL, 0, NULL, "%.2f" },
    { "Bloom",          MI_FLOAT,  &composite.bloom,           0.05f, 0.0f, 1.0f, NULL, NULL, 0, NULL, "%.2f" },
    { "Bloom 2D",       MI_FLOAT,  &composite.bloom_2d,        0.05f, 0.0f, 1.0f, NULL, NULL, 0, NULL, "%.2f" },
    { "Vignette",       MI_FLOAT,  &composite.vignette,        0.05f, 0.0f, 1.0f, NULL, NULL, 0, NULL, "%.2f" },
    { "Phosphor mask",  MI_FLOAT,  &composite.aperture_grille, 0.05f, 0.0f, 1.0f, NULL, NULL, 0, NULL, "%.2f" },
    { "Convergence",    MI_FLOAT,  &composite.chromatic_conv,  0.05f, 0.0f, 1.0f, NULL, NULL, 0, NULL, "%.2f" },
    { "Gamma",          MI_FLOAT,  &composite.gamma,           0.02f, 0.5f, 2.5f, cb_rebuild_gamma_lut, NULL, 0, NULL, "%.2f" },
    { "Persistence",    MI_FLOAT,  &composite.persistence,     0.05f, 0.0f, 1.0f, NULL, NULL, 0, NULL, "%.2f" },
    { "Barrel",         MI_FLOAT,  &composite.barrel,          0.05f, 0.0f, 1.0f, NULL, NULL, 0, NULL, "%.2f" },
    { "H-sync wobble",  MI_FLOAT,  &composite.hsync_wobble,    0.05f, 0.0f, 1.0f, NULL, NULL, 0, NULL, "%.2f" },
    { "Ghosting",       MI_FLOAT,  &composite.ghosting,        0.05f, 0.0f, 1.0f, NULL, NULL, 0, NULL, "%.2f" },
    { "Ghost offset",   MI_INT,    &composite.ghost_offset,    1.0f,  1.0f, 30.0f, NULL, NULL, 0, NULL, "%d px" },
    { "Snow",           MI_FLOAT,  &composite.snow,            0.05f, 0.0f, 1.0f, NULL, NULL, 0, NULL, "%.2f" },
    { "Hum bar",        MI_FLOAT,  &composite.hum_bar,         0.05f, 0.0f, 1.0f, NULL, NULL, 0, NULL, "%.2f" },
    { "Black floor",    MI_INT,    &composite.black_floor,     1.0f,  0.0f, 60.0f, NULL, NULL, 0, NULL, "%d" },
    { "VSync",          MI_TOGGLE, &vsync_enabled, 0, 0, 0, cb_apply_vsync, NULL, 0, NULL, NULL },
};

static const MenuItem menu_presets[] = {
    { "Clean (RGB)",     MI_ACTION, NULL, 0,0,0, NULL, NULL, 0, menu_action_preset_clean,     NULL },
    { "PVM",             MI_ACTION, NULL, 0,0,0, NULL, NULL, 0, menu_action_preset_pvm,       NULL },
    { "Consumer TV",     MI_ACTION, NULL, 0,0,0, NULL, NULL, 0, menu_action_preset_consumer,  NULL },
    { "Commodore 1702",  MI_ACTION, NULL, 0,0,0, NULL, NULL, 0, menu_action_preset_1702,      NULL },
    { "Sony Wega",       MI_ACTION, NULL, 0,0,0, NULL, NULL, 0, menu_action_preset_wega,      NULL },
    { "Trinitron PVM",   MI_ACTION, NULL, 0,0,0, NULL, NULL, 0, menu_action_preset_trinitron, NULL },
    { "S-Video Sharp",   MI_ACTION, NULL, 0,0,0, NULL, NULL, 0, menu_action_preset_svideo,    NULL },
    { "Arcade Monitor",  MI_ACTION, NULL, 0,0,0, NULL, NULL, 0, menu_action_preset_arcade,    NULL },
    { "Bad TV",          MI_ACTION, NULL, 0,0,0, NULL, NULL, 0, menu_action_preset_bad,       NULL },
    { "RF Modulator",    MI_ACTION, NULL, 0,0,0, NULL, NULL, 0, menu_action_preset_rf,        NULL },
    { "VHS Deck",        MI_ACTION, NULL, 0,0,0, NULL, NULL, 0, menu_action_preset_vhs,       NULL },
    { "Mono TV",         MI_ACTION, NULL, 0,0,0, NULL, NULL, 0, menu_action_preset_mono,      NULL },
    { "Cursed CRT",      MI_ACTION, NULL, 0,0,0, NULL, NULL, 0, menu_action_preset_cursed,    NULL },
    { "PAL Consumer TV", MI_ACTION, NULL, 0,0,0, NULL, NULL, 0, menu_action_preset_pal_tv,    NULL },
    { "PAL PVM",         MI_ACTION, NULL, 0,0,0, NULL, NULL, 0, menu_action_preset_pal_pvm,   NULL },
    { "PAL Trinitron",   MI_ACTION, NULL, 0,0,0, NULL, NULL, 0, menu_action_preset_pal_trin,  NULL },
};

/* ============================================================================
 * APU (audio) presets and menu wiring
 * ============================================================================
 * Parallel to the NTSC video preset system. Each APUPreset bundles a
 * filter-chain profile (3 corner frequencies) + analog-character values
 * (DAC nonlinearity, saturation, noise floor, hum, DMC bus crosstalk,
 * output gain). Applied via comp_apply_preset-style one-tap callbacks
 * from the SETUP → Audio → Presets submenu. */

typedef struct {
    const char *name;
    /* filter-chain corner frequencies */
    double hp1_hz;
    double hp2_hz;
    double lp_hz;
    /* analog character */
    float dac_nonlinearity;
    float saturation;
    float noise_floor;
    float hum_60hz;
    float dmc_bus_crosstalk;
    float output_gain;
} APUPreset;

static const APUPreset apu_presets[] = {
    /* 0. Famicom — original Japanese 2A03. Warmer, mild DMC bus
     *    crosstalk that some JP releases used for sample tricks. */
    {
        .name = "Famicom",
        .hp1_hz = 37.0, .hp2_hz = 440.0, .lp_hz = 10000.0,
        .dac_nonlinearity  = 0.30f,
        .saturation        = 0.00f,
        .noise_floor       = 0.00f,
        .hum_60hz          = 0.00f,
        .dmc_bus_crosstalk = 0.30f,
        .output_gain       = 1.00f,
    },
    /* 1. NES Front-Loader — US/EU RP2A03, clean NesDev-spec filter. */
    {
        .name = "NES Front-Loader",
        .hp1_hz = 90.0, .hp2_hz = 440.0, .lp_hz = 14000.0,
        .dac_nonlinearity  = 0.20f,
        .saturation        = 0.00f,
        .noise_floor       = 0.00f,
        .hum_60hz          = 0.00f,
        .dmc_bus_crosstalk = 0.00f,
        .output_gain       = 1.00f,
    },
    /* 2. Mono TV — listened to through the TV speaker via RF/A/V.
     *    Narrow bandwidth, mild amp compression, audible hum, noise. */
    {
        .name = "Mono TV",
        .hp1_hz = 150.0, .hp2_hz = 600.0, .lp_hz = 4000.0,
        .dac_nonlinearity  = 0.40f,
        .saturation        = 0.40f,
        .noise_floor       = 0.005f,
        .hum_60hz          = 0.003f,
        .dmc_bus_crosstalk = 0.10f,
        .output_gain       = 1.10f,
    },
    /* 3. Broken speaker — blown tweeter, severe LPF, heavy clipping,
     *    PSU hum, and a healthy noise floor. */
    {
        .name = "Broken Speaker",
        .hp1_hz = 200.0, .hp2_hz = 800.0, .lp_hz = 2000.0,
        .dac_nonlinearity  = 0.80f,
        .saturation        = 0.80f,
        .noise_floor       = 0.015f,
        .hum_60hz          = 0.010f,
        .dmc_bus_crosstalk = 0.20f,
        .output_gain       = 0.90f,
    },
    /* 4. Cartridge Audio — Famiclone with full DMC bus crosstalk
     *    and extra noise from the cart slot contacts. */
    {
        .name = "Cartridge Audio",
        .hp1_hz = 37.0, .hp2_hz = 440.0, .lp_hz = 12000.0,
        .dac_nonlinearity  = 0.30f,
        .saturation        = 0.20f,
        .noise_floor       = 0.008f,
        .hum_60hz          = 0.005f,
        .dmc_bus_crosstalk = 1.00f,
        .output_gain       = 1.00f,
    },
};

static void apu_apply_preset(const APUPreset *p) {
    apu_filter_config_from_corners(&nes.apu.filter_config,
                                    p->hp1_hz, p->hp2_hz, p->lp_hz,
                                    (double)APU_SAMPLE_RATE);
    nes.apu.analog.filter            = nes.apu.filter_config;
    nes.apu.analog.dac_nonlinearity  = p->dac_nonlinearity;
    nes.apu.analog.saturation        = p->saturation;
    nes.apu.analog.noise_floor       = p->noise_floor;
    nes.apu.analog.hum_60hz          = p->hum_60hz;
    nes.apu.analog.dmc_bus_crosstalk = p->dmc_bus_crosstalk;
    nes.apu.analog.output_gain       = p->output_gain;
    apu_build_dac_tables(&nes.apu);    /* rebuild LUTs with new nonlinearity */
    printf("APU preset: %s\n", p->name);
    fflush(stdout);
}

static void apu_action_preset_famicom(void)  { apu_apply_preset(&apu_presets[0]); }
static void apu_action_preset_nes(void)      { apu_apply_preset(&apu_presets[1]); }
static void apu_action_preset_tv(void)       { apu_apply_preset(&apu_presets[2]); }
static void apu_action_preset_broken(void)   { apu_apply_preset(&apu_presets[3]); }
static void apu_action_preset_cart(void)     { apu_apply_preset(&apu_presets[4]); }

static const MenuItem menu_audio_presets[] = {
    { "Famicom",          MI_ACTION, NULL, 0,0,0, NULL, NULL, 0, apu_action_preset_famicom, NULL },
    { "NES Front-Loader", MI_ACTION, NULL, 0,0,0, NULL, NULL, 0, apu_action_preset_nes,     NULL },
    { "Mono TV",          MI_ACTION, NULL, 0,0,0, NULL, NULL, 0, apu_action_preset_tv,      NULL },
    { "Broken Speaker",   MI_ACTION, NULL, 0,0,0, NULL, NULL, 0, apu_action_preset_broken,  NULL },
    { "Cartridge Audio",  MI_ACTION, NULL, 0,0,0, NULL, NULL, 0, apu_action_preset_cart,    NULL },
};

static const MenuItem menu_audio[] = {
    { "Presets",       MI_SUBMENU, NULL, 0,0,0, NULL, menu_audio_presets, 5, NULL, NULL },
    { "DAC nonlinear", MI_FLOAT,   &nes.apu.analog.dac_nonlinearity,  0.05f, 0.0f, 1.0f, NULL, NULL, 0, NULL, "%.2f" },
    { "Saturation",    MI_FLOAT,   &nes.apu.analog.saturation,        0.05f, 0.0f, 1.0f, NULL, NULL, 0, NULL, "%.2f" },
    { "Noise floor",   MI_FLOAT,   &nes.apu.analog.noise_floor,       0.001f, 0.0f, 0.03f, NULL, NULL, 0, NULL, "%.3f" },
    { "60 Hz hum",     MI_FLOAT,   &nes.apu.analog.hum_60hz,          0.001f, 0.0f, 0.03f, NULL, NULL, 0, NULL, "%.3f" },
    { "DMC crosstalk", MI_FLOAT,   &nes.apu.analog.dmc_bus_crosstalk, 0.05f, 0.0f, 1.0f, NULL, NULL, 0, NULL, "%.2f" },
    { "Output gain",   MI_FLOAT,   &nes.apu.analog.output_gain,       0.05f, 0.0f, 3.0f, NULL, NULL, 0, NULL, "%.2f" },
};

#define MENU_COUNT(tbl) (int)(sizeof(tbl) / sizeof((tbl)[0]))

/* All composite video-related controls live under one top-level Video
 * submenu — keeps the root menu compact and makes it obvious which
 * settings affect the image vs. the sound vs. the OSD itself.
 *
 * "Region" is a 2-way toggle that uses the MI_TOGGLE "OFF/ON" format
 * override: format "NTSC/PAL" displays the field value as NTSC at 0
 * and PAL at 1. The cb_apply_region hook rebuilds the signal table,
 * FIR coefficients, and output color matrix on flip. */
static const MenuItem menu_video[] = {
    { "Region",        MI_TOGGLE,  &composite.signal.region,        0,0,0, cb_apply_region, NULL, 0, NULL, "NTSC/PAL" },
    { "Presets",       MI_SUBMENU, NULL, 0,0,0, NULL, menu_presets, MENU_COUNT(menu_presets), NULL, NULL },
    { "Phase",         MI_SUBMENU, NULL, 0,0,0, NULL, menu_phase,   MENU_COUNT(menu_phase),   NULL, NULL },
    { "Filters",       MI_SUBMENU, NULL, 0,0,0, NULL, menu_filters, MENU_COUNT(menu_filters), NULL, NULL },
    { "Color",         MI_SUBMENU, NULL, 0,0,0, NULL, menu_color,   MENU_COUNT(menu_color),   NULL, NULL },
    { "Effects",       MI_SUBMENU, NULL, 0,0,0, NULL, menu_effects, MENU_COUNT(menu_effects), NULL, NULL },
};

/* OSD chrome toggles (menu appearance + perf overlay). */
static const MenuItem menu_osd[] = {
    { "BG panel",      MI_TOGGLE,  &menu_bg_solid, 0,0,0, NULL, NULL, 0, NULL, NULL },
    { "Perf overlay",  MI_TOGGLE,  &perf_overlay,  0,0,0, NULL, NULL, 0, NULL, NULL },
};

static const MenuItem menu_root[] = {
    { "Video",          MI_SUBMENU, NULL, 0,0,0, NULL, menu_video, MENU_COUNT(menu_video), NULL, NULL },
    { "Audio",          MI_SUBMENU, NULL, 0,0,0, NULL, menu_audio, MENU_COUNT(menu_audio), NULL, NULL },
    { "OSD",            MI_SUBMENU, NULL, 0,0,0, NULL, menu_osd,   MENU_COUNT(menu_osd),   NULL, NULL },
    { "Reset defaults", MI_ACTION,  NULL, 0,0,0, NULL, NULL, 0, menu_action_reset, NULL },
};

/* --- Menu navigation --- */

static void menu_push(const MenuItem *items, int count, const char *title) {
    if (menu_depth >= MENU_STACK_MAX) return;
    menu_stack[menu_depth].items    = items;
    menu_stack[menu_depth].count    = count;
    menu_stack[menu_depth].selected = 0;
    menu_stack[menu_depth].title    = title;
    menu_depth++;
}

static void menu_open_root(void) {
    menu_depth = 0;
    menu_push(menu_root, MENU_COUNT(menu_root), "SETUP");
    menu_open = true;
}

static void menu_close(void) {
    menu_open = false;
    menu_depth = 0;
}

static void menu_back(void) {
    if (menu_depth > 1) menu_depth--;
    else menu_close();
}

static MenuLevel *menu_current(void) {
    return menu_depth > 0 ? &menu_stack[menu_depth - 1] : NULL;
}

static const MenuItem *menu_current_item(void) {
    MenuLevel *lvl = menu_current();
    if (!lvl) return NULL;
    if (lvl->selected < 0 || lvl->selected >= lvl->count) return NULL;
    return &lvl->items[lvl->selected];
}

static void menu_move(int delta) {
    MenuLevel *lvl = menu_current();
    if (!lvl || lvl->count == 0) return;
    lvl->selected = (lvl->selected + delta + lvl->count) % lvl->count;
}

static void menu_adjust(int dir) {
    const MenuItem *it = menu_current_item();
    if (!it || !it->target) return;
    switch (it->type) {
    case MI_FLOAT: {
        float *p = (float *)it->target;
        *p += (float)dir * it->step;
        if (*p < it->min_val) *p = it->min_val;
        if (*p > it->max_val) *p = it->max_val;
        break;
    }
    case MI_INT: {
        int *p = (int *)it->target;
        int step = (int)it->step; if (step < 1) step = 1;
        *p += dir * step;
        int lo = (int)it->min_val, hi = (int)it->max_val;
        if (*p < lo) *p = lo;
        if (*p > hi) *p = hi;
        break;
    }
    case MI_INT_CYCLIC: {
        int *p = (int *)it->target;
        int step = (int)it->step; if (step < 1) step = 1;
        int lo = (int)it->min_val, hi = (int)it->max_val;
        int range = hi - lo + 1;
        *p += dir * step;
        while (*p < lo) *p += range;
        while (*p > hi) *p -= range;
        break;
    }
    case MI_TOGGLE: {
        int *p = (int *)it->target;
        *p = !*p;
        break;
    }
    default: return;
    }
    if (it->on_change) it->on_change();
}

static void menu_activate(void) {
    const MenuItem *it = menu_current_item();
    if (!it) return;
    switch (it->type) {
    case MI_SUBMENU:
        if (it->submenu && it->submenu_count > 0)
            menu_push(it->submenu, it->submenu_count, it->label);
        break;
    case MI_ACTION:
        if (it->action) it->action();
        break;
    case MI_TOGGLE:
        menu_adjust(1);  /* Enter toggles too */
        break;
    default:
        break;
    }
}

/* --- Menu rendering, NES-framebuffer variant ---
 *
 * The menu is drawn INTO the PPU framebuffer AND the index framebuffer
 * right before comp_process runs. That way the composite pipeline treats
 * the OSD as regular NES content — it gets the full signal-level
 * treatment: dot crawl, chroma bleed, scanlines, vignette, the works.
 *
 * It feels like a real 80s/90s TV menu popping up over the game. */

/* NES-framebuffer drawing target. Writes to both the RGB framebuffer
 * (for the legacy path + non-composite mode) and the index framebuffer
 * (for the waveform path). Uses the currently-selected palette for
 * RGB lookup to stay consistent with the game. */
typedef struct {
    uint8_t  *rgb;     /* 256*240*3 */
    uint16_t *idx;     /* 256*240 */
    const uint8_t (*pal)[3];
} NesFB;

static void nesfb_set(NesFB *t, int x, int y, uint8_t pi) {
    if ((unsigned)x >= 256 || (unsigned)y >= 240) return;
    int p = y * 256 + x;
    pi &= 0x3F;
    t->idx[p] = pi;                         /* emphasis = 0 for OSD */
    t->rgb[p*3 + 0] = t->pal[pi][0];
    t->rgb[p*3 + 1] = t->pal[pi][1];
    t->rgb[p*3 + 2] = t->pal[pi][2];
}

static void nesfb_fill(NesFB *t, int x, int y, int w, int h, uint8_t pi) {
    int x0 = x < 0 ? 0 : x;
    int y0 = y < 0 ? 0 : y;
    int x1 = x + w; if (x1 > 256) x1 = 256;
    int y1 = y + h; if (y1 > 240) y1 = 240;
    for (int yy = y0; yy < y1; yy++)
        for (int xx = x0; xx < x1; xx++)
            nesfb_set(t, xx, yy, pi);
}

/* Dim an NES palette index by `tiers` luminance levels. The NES 64-color
 * palette has 4 luminance tiers at 0x00, 0x10, 0x20, 0x30 offsets; each
 * step down roughly halves brightness. This gives us a cheap, palette-
 * native "transparency" — we can see-through to the game underneath by
 * replacing each pixel with a darker variant of the same hue. */
static uint8_t nesfb_dim_index(uint8_t idx, int tiers) {
    idx &= 0x3F;
    int col = idx & 0x0F;
    int lum = (idx >> 4) & 0x03;
    lum -= tiers;
    if (lum < 0) return 0x0F;    /* black hole — darkest available */
    return (uint8_t)((lum << 4) | col);
}

static void nesfb_dim_rect(NesFB *t, int x, int y, int w, int h, int tiers) {
    int x0 = x < 0 ? 0 : x;
    int y0 = y < 0 ? 0 : y;
    int x1 = x + w; if (x1 > 256) x1 = 256;
    int y1 = y + h; if (y1 > 240) y1 = 240;
    for (int yy = y0; yy < y1; yy++) {
        for (int xx = x0; xx < x1; xx++) {
            int p = yy * 256 + xx;
            uint8_t orig = (uint8_t)(t->idx[p] & 0x3F);
            nesfb_set(t, xx, yy, nesfb_dim_index(orig, tiers));
        }
    }
}

/* Hue-compensate a palette index for the current demod_rotate. In
 * practice the waveform pipeline's demod_rotate IS the compensation:
 * it aligns the decoder reference carrier so palette entries appear
 * as their nominal 2C02 colors. $02 (dark blue), $2C (cyan), $06
 * (red) and so on all render correctly once demod_rotate is tuned
 * for the pipeline's chroma phase. So this helper is a pass-through;
 * it exists as a future hook in case the pipeline ever changes. */
static uint8_t hue_compensated(uint8_t nominal) {
    return nominal;
}

/* Draw a single character using the 5x7 font, scaled by `scale`. */
static void nesfb_char(NesFB *t, int x, int y, char c, uint8_t pi, int scale) {
    if ((unsigned char)c >= 128) return;
    if (c >= 'a' && c <= 'z') c -= 32;
    const uint8_t *glyph = osd_font5x7[(int)(unsigned char)c];
    for (int row = 0; row < 7; row++) {
        uint8_t bits = glyph[row];
        for (int col = 0; col < 5; col++) {
            if (bits & (0x10 >> col)) {
                nesfb_fill(t, x + col*scale, y + row*scale, scale, scale, pi);
            }
        }
    }
}

static void nesfb_text(NesFB *t, int x, int y, const char *s, uint8_t pi, int scale) {
    int cx = x;
    for (const char *p = s; *p; p++) {
        if (*p == '\n') { cx = x; y += (7 + 1) * scale; continue; }
        nesfb_char(t, cx, y, *p, pi, scale);
        cx += (5 + 1) * scale;
    }
}

static int nesfb_text_width(const char *s, int scale) {
    int len = 0;
    for (const char *p = s; *p; p++) len++;
    return len * (5 + 1) * scale;
}

/* Drop-shadow text — draw in shadow color offset by (1,1), then in fg. */
static void nesfb_text_sh(NesFB *t, int x, int y, const char *s,
                           uint8_t fg, uint8_t sh, int scale) {
    nesfb_text(t, x + scale, y + scale, s, sh, scale);
    nesfb_text(t, x, y, s, fg, scale);
}

/* --- Menu helpers (format, render) --- */

static void menu_format_value(const MenuItem *it, char *buf, size_t cap) {
    if (!it || !buf || cap == 0) return;
    if (!it->target) { buf[0] = '\0'; return; }
    switch (it->type) {
    case MI_FLOAT:
        snprintf(buf, cap, it->format ? it->format : "%.2f",
                 *(float *)it->target);
        break;
    case MI_INT:
    case MI_INT_CYCLIC:
        snprintf(buf, cap, it->format ? it->format : "%d",
                 *(int *)it->target);
        break;
    case MI_TOGGLE: {
        int v = *(int *)it->target;
        /* If `format` is present and looks like "OFF/ON", use those as
         * the display labels instead of the default ON/OFF. Lets a
         * toggle present as e.g. "NTSC"/"PAL" without introducing a
         * whole new MI_ENUM type. */
        if (it->format) {
            const char *sep = strchr(it->format, '/');
            if (sep) {
                int off_len = (int)(sep - it->format);
                if (v) {
                    snprintf(buf, cap, "%s", sep + 1);
                } else {
                    int n = off_len < (int)(cap - 1) ? off_len : (int)(cap - 1);
                    memcpy(buf, it->format, (size_t)n);
                    buf[n] = '\0';
                }
                break;
            }
        }
        snprintf(buf, cap, v ? "ON" : "OFF");
        break;
    }
    default:
        buf[0] = '\0';
        break;
    }
}

/* Draw the active menu level in the style of a Sony PVM broadcast
 * monitor's on-screen menu. Characteristic PVM aesthetic:
 *   - Dark navy panel (or transparent over a dimmed game)
 *   - Thin cyan border (PVM signature accent color)
 *   - Centered uppercase title at the top
 *   - Clean WHITE/CYAN/GREY palette only — no gold or warm colors
 *   - Selection indicated by a `>` cursor and brighter text
 *     (no solid highlight bar — PVMs rarely did that)
 *   - Horizontal separators above and below the items
 *   - Centered hint line at the bottom
 *
 * Panel is anchored top-left. overscan_x now defaults to 0 (wider
 * viewport), so PX=14 is just a comfortable inset from the edge. */
static void menu_render_nes(uint8_t *rgb_fb, uint16_t *idx_fb,
                             const uint8_t (*pal)[3]) {
    if (!menu_open) return;

    NesFB t;
    t.rgb = rgb_fb;
    t.idx = idx_fb;
    t.pal = pal;

    MenuLevel *lvl = menu_current();
    if (!lvl) return;

    /* PVM palette — white/cyan/grey on dark blue or transparent.
     * Colored picks (BG/border/cursor/value-selected) are hue-compensated
     * so they render as the intended hue regardless of demod_rotate.
     * Greys (title/labels/separators/hint) pass through unchanged. */
    const uint8_t COL_BG        = hue_compensated(0x02); /* dark navy blue */
    const uint8_t COL_BORDER    = hue_compensated(0x2C); /* cyan PVM accent */
    const uint8_t COL_SEP       = 0x10;  /* grey separator */
    const uint8_t COL_TITLE     = 0x30;  /* bright white title */
    const uint8_t COL_LABEL     = 0x30;  /* white label (selected) */
    const uint8_t COL_LABEL_DIM = 0x10;  /* grey label (unselected) */
    const uint8_t COL_VALUE     = hue_compensated(0x2C); /* cyan value (sel) */
    const uint8_t COL_VALUE_DIM = 0x10;  /* grey value (unselected) */
    const uint8_t COL_CURSOR    = hue_compensated(0x2C); /* cyan cursor */
    const uint8_t COL_HINT      = 0x00;  /* dim grey hint */

    /* Compact corner panel. PX=14/PY=14 is a comfortable inset from
     * the edge. Width accommodates the longest label ("CHROMA LOWPASS"
     * = 14 chars × 6 = 84 px) plus a ~6-char value and some margin. */
    const int PX = 14, PY = 14, PW = 166, PH = 136;

    /* Background: solid navy panel, OR fully transparent (no draw at
     * all — the text/border float over the unmodified game pixels). */
    if (menu_bg_solid) {
        nesfb_fill(&t, PX, PY, PW, PH, COL_BG);
    }

    /* Single-pixel cyan border — the PVM signature. */
    nesfb_fill(&t, PX,          PY,          PW, 1, COL_BORDER);
    nesfb_fill(&t, PX,          PY + PH - 1, PW, 1, COL_BORDER);
    nesfb_fill(&t, PX,          PY,          1,  PH, COL_BORDER);
    nesfb_fill(&t, PX + PW - 1, PY,          1,  PH, COL_BORDER);

    /* Centered title — the current menu level's name. */
    const char *title = (menu_depth > 0 && menu_stack[menu_depth - 1].title)
                         ? menu_stack[menu_depth - 1].title : "MENU";
    int title_w = nesfb_text_width(title, 1);
    nesfb_text(&t, PX + (PW - title_w) / 2, PY + 4, title, COL_TITLE, 1);

    /* Separator under the title */
    nesfb_fill(&t, PX + 3, PY + 13, PW - 6, 1, COL_SEP);

    /* Items area */
    int item_y0 = PY + 17;
    int row_h   = 9;
    int label_x = PX + 11;               /* after the cursor column */
    int value_col_right = PX + PW - 5;

    int avail = (PY + PH - 12) - item_y0;
    int max_rows = avail / row_h;
    if (max_rows < 1) max_rows = 1;

    /* Scroll so the selected item is always visible. */
    int first = 0;
    if (lvl->count > max_rows) {
        first = lvl->selected - max_rows / 2;
        if (first < 0) first = 0;
        if (first + max_rows > lvl->count) first = lvl->count - max_rows;
    }
    int last = first + max_rows;
    if (last > lvl->count) last = lvl->count;

    for (int i = first; i < last; i++) {
        int y = item_y0 + (i - first) * row_h;
        bool sel = (i == lvl->selected);
        const MenuItem *it = &lvl->items[i];

        /* Selection cursor in cyan */
        if (sel) {
            nesfb_text(&t, PX + 4, y, ">", COL_CURSOR, 1);
        }

        uint8_t label_col = sel ? COL_LABEL : COL_LABEL_DIM;
        nesfb_text(&t, label_x, y, it->label, label_col, 1);

        /* Value column — right-aligned, cyan when selected, grey otherwise. */
        char vbuf[48];
        if (it->type == MI_SUBMENU) {
            snprintf(vbuf, sizeof(vbuf), ">");
        } else if (it->type == MI_ACTION) {
            vbuf[0] = '\0';
        } else {
            menu_format_value(it, vbuf, sizeof(vbuf));
        }

        int vw = nesfb_text_width(vbuf, 1);
        nesfb_text(&t, value_col_right - vw, y, vbuf,
                    sel ? COL_VALUE : COL_VALUE_DIM, 1);
    }

    /* Separator above the footer */
    nesfb_fill(&t, PX + 3, PY + PH - 12, PW - 6, 1, COL_SEP);

    /* Footer hint line — centered */
    const char *hint = "M EXIT   ESC BACK";
    int hint_w = nesfb_text_width(hint, 1);
    nesfb_text(&t, PX + (PW - hint_w) / 2, PY + PH - 9, hint, COL_HINT, 1);
}

/* Draw the performance overlay into the NES framebuffer. Anchored
 * bottom-center so it doesn't overlap the menu (which is top-left).
 * Gets the NTSC treatment like the menu. */
static void perf_draw_overlay(uint8_t *rgb_fb, uint16_t *idx_fb,
                               const uint8_t (*pal)[3]) {
    if (!perf_overlay || perf_text[0] == '\0') return;

    NesFB t;
    t.rgb = rgb_fb;
    t.idx = idx_fb;
    t.pal = pal;

    int tw = nesfb_text_width(perf_text, 1);
    int PW = tw + 8;
    int PH = 11;
    /* Bottom-center anchor — clears overscan on both sides. */
    int PX = (256 - PW) / 2;
    int PY = 240 - PH - 6;

    /* Semi-transparent backing — dim the game behind the text so it
     * reads regardless of what's underneath. */
    nesfb_dim_rect(&t, PX, PY, PW, PH, 2);

    /* Thin cyan border */
    const uint8_t COL_BORDER = 0x2C;
    nesfb_fill(&t, PX,          PY,          PW, 1, COL_BORDER);
    nesfb_fill(&t, PX,          PY + PH - 1, PW, 1, COL_BORDER);
    nesfb_fill(&t, PX,          PY,          1,  PH, COL_BORDER);
    nesfb_fill(&t, PX + PW - 1, PY,          1,  PH, COL_BORDER);

    /* White text */
    nesfb_text(&t, PX + 4, PY + 2, perf_text, 0x30, 1);
}

/* ============================================================================
 * Palette System
 * ============================================================================ */

#define MAX_PALETTES 32

typedef struct {
    char name[128];
    char path[512];
    uint8_t colors[64][3];
} PaletteEntry;

static PaletteEntry palettes[MAX_PALETTES];
static int palette_count = 0;
static int current_palette = 0;  /* 0 = built-in */

static void palette_add_builtin(void) {
    strcpy(palettes[0].name, "2C02 NTSC (built-in)");
    palettes[0].path[0] = '\0';
    extern const uint8_t ppu_palette_2c02[64][3];
    memcpy(palettes[0].colors, ppu_palette_2c02, sizeof(palettes[0].colors));

    strcpy(palettes[1].name, "2C07 PAL (built-in)");
    palettes[1].path[0] = '\0';
    extern const uint8_t ppu_palette_2c07[64][3];
    memcpy(palettes[1].colors, ppu_palette_2c07, sizeof(palettes[1].colors));

    /* Derived-from-composite PAL palette — filled in at runtime by
     * palette_refresh_composite_derived() after comp_set_region runs.
     * This is the palette you want to use as the "wave-off" fallback
     * to keep the non-composite colors matching what the waveform
     * pipeline produces (composite effects stripped). */
    strcpy(palettes[2].name, "2C07 PAL (composite derived)");
    palettes[2].path[0] = '\0';
    memset(palettes[2].colors, 0, sizeof(palettes[2].colors));

    palette_count = 3;
}

/* Recompute palettes[2] ("2C07 PAL composite derived") from the current
 * composite decoder state. Mirrors the per-scanline decode math — signal
 * table → Y/V/U demod → YUV matrix — but without the per-pixel composite
 * artifacts (no comb filter, no 1H averaging, no FIR band-limiting, no
 * bloom/ringing/vignette). The result is the "clean" base RGB for each
 * palette index, which is exactly what the wave-off path should use so
 * toggling waveform on/off doesn't shift hues.
 *
 * Must be called AFTER comp_set_region (or whenever demod_rotate /
 * chroma_gain / color_matrix changes) to keep the LUT in sync. */
static void palette_refresh_composite_derived(void) {
    const CompositeSignal *s = &composite.signal;
    const int   demod_rot = s->demod_rotate;
    const float chroma_gain = s->chroma_gain;

    for (int pal_idx = 0; pal_idx < 64; pal_idx++) {
        /* Signal waveform for this palette index (emphasis = 0).
         * signal_table[entry][0..11] holds the 12 phase slots. */
        int entry = pal_idx;  /* emph = 0 */
        const float *sig = s->signal_table[entry];

        /* Y = DC component = average of the 12 slots. */
        float Y = 0.0f;
        for (int k = 0; k < 12; k++) Y += sig[k];
        Y /= 12.0f;

        /* V (cos-correlation) and U (sin-correlation) with the
         * composite's demod_rotate applied. The (1/N) scale matches
         * what the composite's sample-by-sample product + Y FIR
         * lowpass produces for a stationary 12-slot periodic signal:
         * (1/2) × fundamental_amplitude × cos(θ + ref). */
        float V = 0.0f, U = 0.0f;
        for (int k = 0; k < 12; k++) {
            int demod_slot = ((k + demod_rot) % 12 + 120) % 12;
            V += sig[k] * s->demod_cos[demod_slot];
            U += sig[k] * s->demod_sin[demod_slot];
        }
        V = (V / 12.0f) * chroma_gain;
        U = (U / 12.0f) * chroma_gain;

        /* YUV (or YIQ on NTSC) → RGB via the composite's matrix. The
         * color_matrix already has the ×255 scale, warm tint, and bias
         * folded in, so the output is directly in 0..255 range. */
        const float (*m)[3] = (const float (*)[3])composite.color_matrix;
        const float *bs = composite.color_bias;
        float rf = m[0][0] * Y + m[0][1] * V + m[0][2] * U + bs[0];
        float gf = m[1][0] * Y + m[1][1] * V + m[1][2] * U + bs[1];
        float bf = m[2][0] * Y + m[2][1] * V + m[2][2] * U + bs[2];

        if (rf < 0.0f) rf = 0.0f; if (rf > 255.0f) rf = 255.0f;
        if (gf < 0.0f) gf = 0.0f; if (gf > 255.0f) gf = 255.0f;
        if (bf < 0.0f) bf = 0.0f; if (bf > 255.0f) bf = 255.0f;

        palettes[2].colors[pal_idx][0] = (uint8_t)rf;
        palettes[2].colors[pal_idx][1] = (uint8_t)gf;
        palettes[2].colors[pal_idx][2] = (uint8_t)bf;
    }

    /* If the derived palette is currently selected, re-point the PPU
     * so the next frame picks up the new values. */
    if (current_palette == 2) {
        nes.ppu.color_palette = palettes[2].colors;
    }
}

static bool palette_load_file(const char *path, uint8_t colors[64][3]) {
    FILE *fp = fopen(path, "rb");
    if (!fp) return false;

    uint8_t data[192];
    size_t n = fread(data, 1, 192, fp);
    fclose(fp);
    if (n != 192) return false;

    for (int i = 0; i < 64; i++) {
        colors[i][0] = data[i * 3 + 0];
        colors[i][1] = data[i * 3 + 1];
        colors[i][2] = data[i * 3 + 2];
    }
    return true;
}

/* Load a 256×240-byte palette-index file produced by
 * tools/image_to_nes_indices.py. Returns a heap buffer on success, NULL
 * on failure. Caller owns the buffer. */
static uint8_t *load_static_frame_bin(const char *path) {
    FILE *fp = fopen(path, "rb");
    if (!fp) {
        fprintf(stderr, "Failed to open static frame file: %s\n", path);
        return NULL;
    }
    size_t sz = (size_t)PPU_WIDTH * (size_t)PPU_HEIGHT;
    uint8_t *buf = (uint8_t *)malloc(sz);
    if (!buf) { fclose(fp); return NULL; }
    size_t n = fread(buf, 1, sz, fp);
    fclose(fp);
    if (n != sz) {
        fprintf(stderr, "Static frame wrong size: %zu (expected %zu)\n", n, sz);
        free(buf);
        return NULL;
    }
    return buf;
}

/* Paint the static frame into the PPU framebuffers so the rest of the
 * pipeline (menu overlay, composite, display) runs unchanged. Indices
 * are clamped to 0..63 and emphasis is forced off. */
static void apply_static_frame_to_ppu(void) {
    if (!static_frame_idx) return;
    const uint8_t (*pal)[3] =
        nes.ppu.color_palette ? nes.ppu.color_palette : ppu_palette_2c02;
    for (int i = 0; i < PPU_WIDTH * PPU_HEIGHT; i++) {
        uint8_t idx = static_frame_idx[i] & 0x3F;
        nes.ppu.index_framebuffer[i] = (uint16_t)idx;
        nes.ppu.framebuffer[i * 3 + 0] = pal[idx][0];
        nes.ppu.framebuffer[i * 3 + 1] = pal[idx][1];
        nes.ppu.framebuffer[i * 3 + 2] = pal[idx][2];
    }
}

static void palette_scan_directory(const char *dir) {
    DIR *d = opendir(dir);
    if (!d) return;

    struct dirent *ent;
    while ((ent = readdir(d)) != NULL && palette_count < MAX_PALETTES) {
        size_t len = strlen(ent->d_name);
        if (len < 5) continue;
        if (strcmp(ent->d_name + len - 4, ".pal") != 0) continue;

        /* Deduplicate by name (same .pal found via different paths) */
        char name[128];
        size_t name_len = len - 4;
        if (name_len >= sizeof(name)) name_len = sizeof(name) - 1;
        memcpy(name, ent->d_name, name_len);
        name[name_len] = '\0';

        bool duplicate = false;
        for (int i = 0; i < palette_count; i++) {
            if (strcmp(palettes[i].name, name) == 0) {
                duplicate = true;
                break;
            }
        }
        if (duplicate) continue;

        char path[512];
        snprintf(path, sizeof(path), "%s/%s", dir, ent->d_name);

        PaletteEntry *pe = &palettes[palette_count];
        if (palette_load_file(path, pe->colors)) {
            strncpy(pe->name, name, sizeof(pe->name) - 1);
            strncpy(pe->path, path, sizeof(pe->path) - 1);
            palette_count++;
        }
    }
    closedir(d);
}

/* Pick a region-appropriate default palette by name substring. Returns
 * the index, or -1 if no match. */
static int palette_find_by_name(const char *substr) {
    for (int i = 0; i < palette_count; i++) {
        if (strstr(palettes[i].name, substr)) return i;
    }
    return -1;
}

static void palette_init(const char *exe_path) {
    palette_add_builtin();

    /* Resolve the real path of the executable so palette loading works
       regardless of the working directory the user launches from. */
    /* realpath allocates: glibc's fortified realpath aborts when handed a
     * buffer smaller than PATH_MAX. */
    char *real_exe = exe_path ? realpath(exe_path, NULL) : NULL;

    /* Try CWD-relative first */
    palette_scan_directory("palettes");

    /* Try paths relative to the executable:
       bin/mynes -> ../palettes, ../../palettes */
    if (real_exe) {
        const char *last_slash = strrchr(real_exe, '/');
        if (last_slash) {
            char dir[1024];
            int prefix_len = (int)(last_slash - real_exe);
            snprintf(dir, sizeof(dir), "%.*s/../palettes", prefix_len, real_exe);
            palette_scan_directory(dir);
            snprintf(dir, sizeof(dir), "%.*s/../../palettes", prefix_len, real_exe);
            palette_scan_directory(dir);
        }
    }
    free(real_exe);

    /* Don't auto-select a default here — the region detection code in
     * main() picks the right palette after the ROM region is known
     * (2C07 for PAL, Digital Prime for NTSC). Leave current_palette at
     * its default (0 = 2C02 built-in) so the region-pick logic can
     * tell we haven't chosen anything yet. */

    printf("Loaded %d palette(s)\n", palette_count);
    for (int i = 0; i < palette_count; i++) {
        printf("  [%d] %s\n", i, palettes[i].name);
    }
}

/* ============================================================================
 * NES Emulator State
 * ============================================================================ */

static SDL_Window *window = NULL;
static SDL_Renderer *renderer = NULL;
static SDL_Texture *texture = NULL;
static SDL_Texture *composite_texture = NULL;
static SDL_AudioDeviceID audio_device = 0;

static bool running = true;
static bool paused = false;
static bool fast_mode = false;
static int screenshot_after_frames = 0;  /* 0 = disabled */
static int frame_counter = 0;
static int screenshot_num = 0;

/* Hot-plugged game controllers: the first to arrive is player 1, the next
 * player 2, and a removed pad frees its slot. Same mapping as the GPU
 * frontend (RetroArch convention: EAST = A, SOUTH = B, 0.5 stick deadzone). */
static SDL_GameController *controllers[2];

static void controller_added(int device_index) {
    for (int i = 0; i < 2; i++) {
        if (controllers[i]) continue;
        controllers[i] = SDL_GameControllerOpen(device_index);
        if (controllers[i])
            printf("Game controller connected: %s (P%d)\n",
                   SDL_GameControllerName(controllers[i]), i + 1);
        return;
    }
}

static void controller_removed(SDL_JoystickID instance_id) {
    for (int i = 0; i < 2; i++) {
        if (!controllers[i] || SDL_JoystickInstanceID(
                SDL_GameControllerGetJoystick(controllers[i])) != instance_id) continue;
        printf("Game controller removed (P%d)\n", i + 1);
        SDL_GameControllerClose(controllers[i]);
        controllers[i] = NULL;
    }
}

static uint8_t controller_buttons(int slot) {
    SDL_GameController *c = controllers[slot];
    if (!c) return 0;
    uint8_t b = 0;
    if (SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_B) ||
        SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_Y))          b |= BTN_A;
    if (SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_A) ||
        SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_X))          b |= BTN_B;
    if (SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_BACK))       b |= BTN_SELECT;
    if (SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_START))      b |= BTN_START;
    if (SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_DPAD_UP))    b |= BTN_UP;
    if (SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_DPAD_DOWN))  b |= BTN_DOWN;
    if (SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_DPAD_LEFT))  b |= BTN_LEFT;
    if (SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_DPAD_RIGHT)) b |= BTN_RIGHT;
    int x = SDL_GameControllerGetAxis(c, SDL_CONTROLLER_AXIS_LEFTX);
    int y = SDL_GameControllerGetAxis(c, SDL_CONTROLLER_AXIS_LEFTY);
    if (x < -16384) b |= BTN_LEFT;  else if (x > 16384) b |= BTN_RIGHT;
    if (y < -16384) b |= BTN_UP;    else if (y > 16384) b |= BTN_DOWN;
    return b;
}

void save_screenshot(void) {
    char filename[64];
    snprintf(filename, sizeof(filename), "screenshot_%03d.ppm", screenshot_num++);

    FILE *fp = fopen(filename, "wb");
    if (fp) {
        if (composite_enabled && composite.output) {
            int out_h = 240 * composite.rows_per_scanline;
            fprintf(fp, "P6\n%d %d\n255\n", composite.out_w, out_h);
            fwrite(composite.output, 1, composite.out_w * out_h * 3, fp);
        } else {
            fprintf(fp, "P6\n%d %d\n255\n", PPU_WIDTH, PPU_HEIGHT);
            fwrite(nes.ppu.framebuffer, 1, PPU_WIDTH * PPU_HEIGHT * 3, fp);
        }
        fclose(fp);
        printf("Saved: %s\n", filename);
    }
}

void toggle_composite(void);
void toggle_fullscreen(void);
static void comp_reconfigure_for_window(void);

void cycle_palette(void) {
    if (palette_count <= 1) return;
    current_palette = (current_palette + 1) % palette_count;
    nes.ppu.color_palette = palettes[current_palette].colors;
    printf("Palette: %s\n", palettes[current_palette].name);
}

static void comp_print_tuning(void) {
    printf("--- NTSC tuning ---\n");
    printf("  Phase:   line_adv=%+d field_adv=%+d num_fields=%d base=%d demod_rot=%+d chroma_gain=%.2f\n",
        composite.signal.phase_line_adv, composite.signal.phase_field_adv,
        composite.signal.phase_num_fields, composite.signal.phase_base,
        composite.signal.demod_rotate, composite.signal.chroma_gain);
    printf("  Filters: y_cutoff=%.3f (taps=%d)  c_cutoff=%.3f (taps=%d)\n",
        composite.signal.y_cutoff, composite.signal.fir_y_n,
        composite.signal.c_cutoff, composite.signal.fir_c_n);
    printf("  Color:   hue=%+5.1f° sat=%.2f bright=%+0.2f contrast=%.2f\n",
        composite.hue_deg, composite.saturation, composite.brightness, composite.contrast);
    fflush(stdout);
}

static void comp_reset_tuning(void) {
    composite.signal.phase_base       = 0;
    composite.signal.phase_line_adv   = 6;
    composite.signal.phase_field_adv  = 6;
    composite.signal.phase_num_fields = 4;
    composite.signal.demod_rotate     = 4;
    composite.signal.chroma_gain      = 1.3f;
    composite.signal.color_killer     = 0.05f;
    composite.signal.comb_filter      = 0;
    composite.signal.y_cutoff         = 0.043f;
    composite.signal.c_cutoff         = 0.015f;
    composite.signal.fir_y_n          = 37;
    composite.signal.fir_c_n          = 47;
    composite.luma_afterglow          = 0.25f;
    composite.luma_ringing            = 0.40f;
    composite.chroma_ringing          = 0.0f;
    composite.bloom                   = 0.40f;
    composite.bloom_2d                = 0.0f;
    composite.aperture_grille         = 0.0f;
    composite.chromatic_conv          = 0.0f;
    composite.gamma                   = 1.0f;
    composite.persistence             = 0.0f;
    composite.barrel                  = 0.0f;
    composite.hsync_wobble            = 0.0f;
    composite.ghosting                = 0.0f;
    composite.ghost_offset            = 8;
    composite.snow                    = 0.0f;
    composite.hum_bar                 = 0.0f;
    composite.black_floor             = 0;
    composite.signal.svideo           = 0;
    comp_build_gamma_lut(&composite);
    comp_redesign_firs(&composite.signal);
    composite.hue_deg     = 0.0f;
    composite.saturation  = 0.90f;
    composite.brightness  = 0.0f;
    composite.contrast    = 1.0f;
    printf("NTSC tuning reset to defaults.\n");
    comp_print_tuning();
}

void handle_input(void) {
    SDL_Event event;
    uint8_t buttons = 0;

    while (SDL_PollEvent(&event)) {
        switch (event.type) {
            case SDL_QUIT:
                running = false;
                break;

            case SDL_WINDOWEVENT:
                if (event.window.event == SDL_WINDOWEVENT_SIZE_CHANGED ||
                    event.window.event == SDL_WINDOWEVENT_RESIZED) {
                    comp_reconfigure_for_window();
                }
                break;

            case SDL_CONTROLLERDEVICEADDED:
                controller_added(event.cdevice.which);
                break;
            case SDL_CONTROLLERDEVICEREMOVED:
                controller_removed(event.cdevice.which);
                break;

            case SDL_TEXTINPUT:
                if (browser_active) browser_handle_text(&browser,event.text.text);
                break;

            case SDL_KEYDOWN:
                /* --- Browser mode: owns input until closed. --- */
                if (browser_active) {
                    int bk = sdl_to_browser_key(event.key.keysym.scancode);
                    if (bk < 0) break;
                    BrowserResult r = browser_handle_key(&browser,
                                                         (BrowserKey)bk);
                    if (r == BROWSER_SELECTED) {
                        ROM new_rom;
                        int re = nes_rom_load(&new_rom, browser.chosen_path);
                        if (re == ROM_OK) {
                            mynes_saves_flush(&saves, nes.mapper.prg_ram, false);
                            nes_load_mapper(&nes, new_rom.mapper,
                                new_rom.prg_rom, new_rom.prg_size,
                                new_rom.chr_rom, new_rom.chr_size,
                                new_rom.mirroring);
                            nes_rom_apply_trainer(&new_rom, &nes.mapper);
                            /* The mapper now points into new_rom, so the
                             * old image is no longer referenced. */
                            nes_rom_free(&rom);
                            rom = new_rom;
                            rom_loaded = true;
                            apply_rom_region();
                            nes_reset(&nes);
                            saves_attach(browser.chosen_path);
                            mynes_config_add_recent(&mynes_config,
                                                    browser.chosen_path);
                            mynes_config_save(&mynes_config);
                            printf("Loaded %s\n", browser.chosen_path);
                        } else {
                            fprintf(stderr, "Failed to load %s: %s\n",
                                browser.chosen_path, nes_rom_error_str(re));
                            browser_set_error(&browser,nes_rom_error_str(re));
                            r=BROWSER_BROWSING;
                        }
                    }
                    if (r == BROWSER_CANCELLED && !rom_loaded) {
                        running = false;
                    }
                    if (r != BROWSER_BROWSING) { browser_active = false; SDL_StopTextInput(); }
                    break;
                }

                /* O: reopen the ROM browser mid-session. */
                if (event.key.keysym.sym == SDLK_o) {
                    if (!browser.current_dir[0]) browser_init(&browser, NULL, &mynes_config);
                    else browser_refresh(&browser);
                    browser.can_resume=rom_loaded;
                    browser_active = true;
                    SDL_StartTextInput();
                    break;
                }

                /* --- Menu mode: intercept navigation keys first. --- */
                if (menu_open) {
                    SDL_Keycode k = event.key.keysym.sym;
                    if (k == SDLK_UP)         { menu_move(-1); break; }
                    if (k == SDLK_DOWN)       { menu_move(+1); break; }
                    if (k == SDLK_LEFT)       { menu_adjust(-1); break; }
                    if (k == SDLK_RIGHT)      { menu_adjust(+1); break; }
                    if (k == SDLK_RETURN ||
                        k == SDLK_KP_ENTER)   { menu_activate(); break; }
                    if (k == SDLK_BACKSPACE ||
                        k == SDLK_ESCAPE)     { menu_back(); break; }
                    if (k == SDLK_m)          { menu_close(); break; }
                    /* Fall through: hotkeys still work in menu mode. */
                }

                if (event.key.keysym.sym == SDLK_m) {
                    menu_open_root();
                    break;
                } else if (event.key.keysym.sym == SDLK_ESCAPE) {
                    running = false;
                } else if (event.key.keysym.sym == SDLK_SPACE) {
                    paused = !paused;
                    printf("%s\n", paused ? "PAUSED" : "RESUMED");
                } else if (event.key.keysym.sym == SDLK_s) {
                    save_screenshot();
                } else if (event.key.keysym.sym == SDLK_f) {
                    toggle_fullscreen();
                } else if (event.key.keysym.sym == SDLK_TAB) {
                    fast_mode = !fast_mode;
                    printf("Fast mode: %s\n", fast_mode ? "ON" : "OFF");
                } else if (event.key.keysym.sym == SDLK_p) {
                    cycle_palette();
                } else if (event.key.keysym.sym == SDLK_c) {
                    toggle_composite();
                } else if (event.key.keysym.sym == SDLK_v) {
                    /* Toggle the on-screen perf overlay without
                     * opening the menu. */
                    perf_overlay = !perf_overlay;
                } else if (event.key.keysym.sym == SDLK_y) {
                    /* Dump current NTSC tuning values. */
                    comp_print_tuning();
                    osd_kick();
                } else if (event.key.keysym.sym == SDLK_o) {
                    /* Toggle OSD pinned (always visible). */
                    osd_pinned = !osd_pinned;
                    osd_kick();
                    printf("OSD: %s\n", osd_pinned ? "PINNED" : "AUTO-HIDE");
                } else if (event.key.keysym.sym == SDLK_F12) {
                    /* Reset all NTSC tuning to defaults. */
                    comp_reset_tuning();
                    osd_kick();
                } else {
                    /* NTSC tuning hotkeys — function keys only fire when
                     * composite mode is on, so they don't steal keys from
                     * the game. SHIFT = color controls, no mod = phase
                     * controls. Every change prints the new value. */
                    int shift = (event.key.keysym.mod & KMOD_SHIFT) ? 1 : 0;
                    bool handled = true;
                    switch (event.key.keysym.sym) {
                    /* Phase tuning (no shift) / Color tuning (shift) */
                    case SDLK_F1:
                        if (shift) composite.hue_deg -= 2.0f;
                        else       composite.signal.phase_line_adv -= 1;
                        break;
                    case SDLK_F2:
                        if (shift) composite.hue_deg += 2.0f;
                        else       composite.signal.phase_line_adv += 1;
                        break;
                    case SDLK_F3:
                        if (shift) composite.saturation -= 0.05f;
                        else       composite.signal.phase_field_adv -= 1;
                        break;
                    case SDLK_F4:
                        if (shift) composite.saturation += 0.05f;
                        else       composite.signal.phase_field_adv += 1;
                        break;
                    case SDLK_F5:
                        if (shift) composite.brightness -= 0.02f;
                        else       composite.signal.phase_base =
                                       (composite.signal.phase_base + 11) % 12;
                        break;
                    case SDLK_F6:
                        if (shift) composite.brightness += 0.02f;
                        else       composite.signal.phase_base =
                                       (composite.signal.phase_base + 1) % 12;
                        break;
                    case SDLK_F7:
                        if (shift) composite.contrast -= 0.02f;
                        else       composite.signal.demod_rotate -= 1;
                        break;
                    case SDLK_F8:
                        if (shift) composite.contrast += 0.02f;
                        else       composite.signal.demod_rotate += 1;
                        break;
                    case SDLK_F9:
                        composite.signal.chroma_gain -= 0.1f;
                        if (composite.signal.chroma_gain < 0.0f)
                            composite.signal.chroma_gain = 0.0f;
                        break;
                    case SDLK_F10:
                        composite.signal.chroma_gain += 0.1f;
                        break;
                    case SDLK_COMMA:
                        /* Y lowpass cutoff -: tighter = less dot crawl,
                         * softer luma. Lower = more aggressive. */
                        composite.signal.y_cutoff -= 0.005f;
                        comp_redesign_firs(&composite.signal);
                        break;
                    case SDLK_PERIOD:
                        composite.signal.y_cutoff += 0.005f;
                        comp_redesign_firs(&composite.signal);
                        break;
                    case SDLK_LEFTBRACKET:
                        /* C lowpass cutoff -: tighter chroma BW,
                         * softer color bleed. */
                        composite.signal.c_cutoff -= 0.002f;
                        comp_redesign_firs(&composite.signal);
                        break;
                    case SDLK_RIGHTBRACKET:
                        composite.signal.c_cutoff += 0.002f;
                        comp_redesign_firs(&composite.signal);
                        break;
                    case SDLK_n:
                        /* Cycle num_fields through {1, 2, 3, 4, 6} */
                        {
                            int cur = composite.signal.phase_num_fields;
                            if      (cur <= 1) cur = 2;
                            else if (cur == 2) cur = 3;
                            else if (cur == 3) cur = 4;
                            else if (cur == 4) cur = 6;
                            else               cur = 1;
                            composite.signal.phase_num_fields = cur;
                        }
                        break;
                    default:
                        handled = false;
                        break;
                    }
                    if (handled) { comp_print_tuning(); osd_kick(); }
                }
                break;
        }
    }

    /* Don't feed keyboard state to the game controller while the menu
     * is open — arrow keys belong to the menu. */
    if (menu_open) {
        nes_set_controller(&nes, 0, 0);
        nes_set_controller(&nes, 1, 0);
        return;
    }

    const Uint8 *keys = SDL_GetKeyboardState(NULL);

    if (keys[SDL_SCANCODE_X])      buttons |= BTN_A;
    if (keys[SDL_SCANCODE_Z])      buttons |= BTN_B;
    if (keys[SDL_SCANCODE_RSHIFT] || keys[SDL_SCANCODE_LSHIFT]) buttons |= BTN_SELECT;
    if (keys[SDL_SCANCODE_RETURN]) buttons |= BTN_START;
    if (keys[SDL_SCANCODE_UP])     buttons |= BTN_UP;
    if (keys[SDL_SCANCODE_DOWN])   buttons |= BTN_DOWN;
    if (keys[SDL_SCANCODE_LEFT])   buttons |= BTN_LEFT;
    if (keys[SDL_SCANCODE_RIGHT])  buttons |= BTN_RIGHT;

    /* Auto-press Start periodically while in auto-start window. Cycle
     * of 12 frames: 2 pressed, 10 released. */
    if (auto_start_until > 0 && frame_counter < auto_start_until) {
        if ((frame_counter % 12) < 2) buttons |= BTN_START;
    }

    nes_set_controller(&nes, 0, buttons | controller_buttons(0));
    nes_set_controller(&nes, 1, controller_buttons(1));
}

void toggle_composite(void) {
    composite_enabled = !composite_enabled;
    /* Both modes use the same 8:7 TV aspect now, so no window resize
     * is needed when toggling — the image stays the same size. */
    printf("NTSC Composite: %s\n", composite_enabled ? "ON" : "OFF");
}

/* Visible NES pixel dimensions after overscan cropping. Composite and
 * non-composite modes apply the same crop so toggling composite keeps
 * the image aspect identical. Used by every place that computes the
 * destination rectangle's aspect ratio. */
static inline void comp_visible_pixels(int *vw, int *vh) {
    int ox = composite.overscan_x;
    int oy = composite.overscan_y;
    if (ox < 0) ox = 0; if (ox * 2 >= PPU_WIDTH)  ox = PPU_WIDTH / 2 - 1;
    if (oy < 0) oy = 0; if (oy * 2 >= PPU_HEIGHT) oy = PPU_HEIGHT / 2 - 1;
    *vw = PPU_WIDTH  - 2 * ox;
    *vh = PPU_HEIGHT - 2 * oy;
}

static inline float comp_visible_src_aspect(void) {
    int vw, vh;
    comp_visible_pixels(&vw, &vh);
    /* NES pixels are 8:7, so physical aspect is (vw × 8/7) : vh. */
    return (float)vw * (8.0f / 7.0f) / (float)vh;
}

/* Pick the largest rows_per_scanline that makes the composite
 * texture exactly fit (or slightly letterbox inside) the current
 * window's drawable area. Prevents moiré banding from the SDL
 * nearest-neighbor upscaler chunking a low-rps texture across many
 * display pixels — instead the scanline curve's brightness gradient
 * is baked at the display's native vertical resolution. */
static void comp_reconfigure_for_window(void) {
    if (!renderer || !composite_enabled) return;

    int win_w, win_h;
    SDL_GetRendererOutputSize(renderer, &win_w, &win_h);
    if (win_w <= 0 || win_h <= 0) return;

    /* Aspect accounts for overscan cropping so the destination rect
     * preserves the NES 8:7 pixel aspect for the VISIBLE area. */
    float src_aspect = comp_visible_src_aspect();
    float win_aspect = (float)win_w / (float)win_h;
    int dst_h;
    if (win_aspect > src_aspect)
        dst_h = win_h;
    else
        dst_h = (int)((float)win_w / src_aspect);

    int new_rps = dst_h / 240;
    if (new_rps < 1) new_rps = 1;
    if (new_rps > 12) new_rps = 12;

    if (new_rps != composite.rows_per_scanline) {
        int comp_w = PPU_WIDTH * 4 * 8 / 7;
        comp_resize(&composite, comp_w, new_rps);

        if (composite_texture) SDL_DestroyTexture(composite_texture);
        composite_texture = SDL_CreateTexture(
            renderer,
            SDL_PIXELFORMAT_RGB24,
            SDL_TEXTUREACCESS_STREAMING,
            comp_w, 240 * new_rps
        );
    }
}

/* Match the display refresh rate and vsync configuration to the NES
 * region's native frame rate. Target is 60 Hz for NTSC, 50 Hz for PAL.
 *
 * Strategy (best-effort, quiet fallback):
 *
 *   1. In exclusive fullscreen (SDL_WINDOW_FULLSCREEN), query the
 *      display's supported modes and pick one matching the target
 *      refresh rate via SDL_GetClosestDisplayMode + SDL_SetWindowDisplayMode.
 *      This is the ideal case — each emulator frame gets one display
 *      frame, vsync stays clean, no tearing, no jitter.
 *
 *   2. In desktop fullscreen (SDL_WINDOW_FULLSCREEN_DESKTOP) or
 *      windowed mode, the display refresh rate isn't under our control.
 *      We auto-manage vsync instead: enable it only if the display
 *      rate matches (or is an integer multiple of) the target. On a
 *      mismatch we disable vsync and let the main-loop timer pace
 *      frames — that produces clean 50 Hz/60 Hz motion with occasional
 *      tearing, which beats the 5:1 stutter of vsynced-mismatched
 *      presentation.
 *
 *   3. Called at startup after NES region detection, and again when
 *      the user toggles fullscreen. NOT called by the composite
 *      region OSD toggle — that switch is purely visual (2C02 ↔ 2C07
 *      signal model) and doesn't affect game timing.
 */
static void apply_display_for_nes_region(int nes_region) {
    nes_region_current = nes_region;
    if (!window) return;

    const int target_hz = (nes_region == NES_REGION_PAL) ? 50 : 60;
    int disp_idx = SDL_GetWindowDisplayIndex(window);
    if (disp_idx < 0) disp_idx = 0;

    SDL_DisplayMode current;
    if (SDL_GetCurrentDisplayMode(disp_idx, &current) != 0) {
        printf("Display mode query failed: %s\n", SDL_GetError());
        return;
    }

    Uint32 flags = SDL_GetWindowFlags(window);
    int is_exclusive = (flags & SDL_WINDOW_FULLSCREEN) != 0 &&
                       (flags & SDL_WINDOW_FULLSCREEN_DESKTOP) !=
                       SDL_WINDOW_FULLSCREEN_DESKTOP;

    /* If we're in exclusive fullscreen, try to switch the display
     * mode to the target refresh rate. Don't fight the OS if no
     * matching mode exists — fall through to vsync management. */
    if (is_exclusive) {
        SDL_DisplayMode desired = current;
        desired.refresh_rate = target_hz;
        SDL_DisplayMode closest;
        if (SDL_GetClosestDisplayMode(disp_idx, &desired, &closest)
            && closest.refresh_rate == target_hz) {
            SDL_SetWindowDisplayMode(window, &closest);
            current = closest;
            printf("Display: exclusive %dx%d @ %d Hz (matched %s)\n",
                   current.w, current.h, current.refresh_rate,
                   nes_region == NES_REGION_PAL ? "PAL" : "NTSC");
        }
    }

    /* Auto-manage vsync. A match (or 2×/3× multiple) keeps vsync
     * enabled; otherwise disable it so the timer can pace frames
     * cleanly without getting pinned to the display's vblank. */
    int dhz = current.refresh_rate;
    int match = (dhz == target_hz) ||
                (dhz == target_hz * 2) ||
                (dhz == target_hz * 3);
    int desired_vsync = match ? 1 : 0;
    if (vsync_enabled != desired_vsync) {
        vsync_enabled = desired_vsync;
        cb_apply_vsync();
        printf("Display: %d Hz, target %d Hz — vsync auto-%s\n",
               dhz, target_hz, desired_vsync ? "ENABLED" : "DISABLED");
    } else {
        printf("Display: %d Hz, target %d Hz — vsync %s\n",
               dhz, target_hz, vsync_enabled ? "enabled" : "disabled");
    }
}

/* Put the console, the composite pipeline and the display rate in the
 * loaded ROM's region (or PAL under --pal). Called at startup and for each
 * ROM the browser loads, before nes_reset. Without a ROM the browser runs
 * with NTSC defaults. Returns the region. */
static int apply_rom_region(void) {
    int region = NES_REGION_NTSC;
    if (force_pal) {
        region = NES_REGION_PAL;
    } else if (rom_loaded && rom.tv_system == NES_TV_PAL) {
        region = NES_REGION_PAL;
        printf("Region: PAL (auto-detected from ROM header)\n");
    }
    nes_set_region(&nes, region);
    if (region == NES_REGION_PAL)
        printf("Region: PAL (312 scanlines, 1.66MHz CPU)\n");
    /* ppu_set_region selects the built-in palette; keep the one in use. */
    if (current_palette >= 0 && current_palette < palette_count)
        nes.ppu.color_palette = palettes[current_palette].colors;

    /* Wire the same region into the composite pipeline so PAL ROMs get
     * 2C07 voltages, YUV decoding, and per-line V-flip. Safe to call
     * after comp_init — comp_set_region rebuilds the signal table, FIR
     * coefficients, and the output color matrix to match. */
    comp_set_region(&composite,
                    region == NES_REGION_PAL ? COMP_REGION_PAL
                                             : COMP_REGION_NTSC);

    /* Compute the derived PAL palette from the just-initialized
     * composite decoder state, so palettes[2] holds 2C07 RGB values
     * that are mathematically consistent with what the waveform
     * pipeline will produce. */
    palette_refresh_composite_derived();

    /* Match the display refresh rate / vsync policy to the NES region's
     * native frame rate. PAL on a 60 Hz display is the most visible
     * mismatch — apply_display_for_nes_region tries exclusive display
     * mode switching first, then falls back to disabling vsync so the
     * main-loop timer paces frames without stutter. */
    apply_display_for_nes_region(region);
    return region;
}

void toggle_fullscreen(void) {
    Uint32 flags = SDL_GetWindowFlags(window);
    if (flags & SDL_WINDOW_FULLSCREEN_DESKTOP)
        SDL_SetWindowFullscreen(window, 0);
    else
        SDL_SetWindowFullscreen(window, SDL_WINDOW_FULLSCREEN_DESKTOP);
    /* Recreate the composite texture so its vertical resolution
     * matches the new display size — prevents moiré scanline banding. */
    comp_reconfigure_for_window();
    /* Re-apply vsync policy for the new fullscreen/windowed state.
     * The display-mode switch may have happened (exclusive FS) or we
     * may need to disable vsync (windowed on a mismatched display). */
    apply_display_for_nes_region(nes_region_current);
}

void render_frame(void) {
    int win_w, win_h;
    SDL_GetRendererOutputSize(renderer, &win_w, &win_h);

    /* Compute destination rect preserving aspect ratio.
     * Both composite and non-composite use the same 8:7 NTSC TV aspect
     * AND the same overscan crop so proportions stay consistent when
     * toggling composite. The visible region is (PPU_WIDTH − 2·overscan_x)
     * NES pixels wide, each 8:7 on screen. */
    const float src_aspect = comp_visible_src_aspect();
    float win_aspect = (float)win_w / (float)win_h;

    SDL_Rect dst;
    if (win_aspect > src_aspect) {
        dst.h = win_h;
        dst.w = (int)(win_h * src_aspect);
        dst.x = (win_w - dst.w) / 2;
        dst.y = 0;
    } else {
        dst.w = win_w;
        dst.h = (int)(win_w / src_aspect);
        dst.x = 0;
        dst.y = (win_h - dst.h) / 2;
    }

    /* Force integer scaling between the composite texture and the
     * display — SDL's nearest-neighbor upscaler produces visible moiré
     * banding on the baked-in scanline gradient when the ratio isn't
     * exact. Pick the largest integer multiple of tex_h that fits in
     * the available window height, then recompute dst.w to preserve
     * the 8:7 NTSC aspect. Letterbox whatever's left. */
    if (composite_enabled && composite.rows_per_scanline > 0) {
        int tex_h = 240 * composite.rows_per_scanline;
        if (tex_h > 0 && dst.h >= tex_h) {
            int k = dst.h / tex_h;
            if (k < 1) k = 1;
            dst.h = k * tex_h;
            dst.w = (int)((float)dst.h * src_aspect + 0.5f);
            dst.x = (win_w - dst.w) / 2;
            dst.y = (win_h - dst.h) / 2;
        }
    }

    SDL_RenderClear(renderer);

    /* Draw the OSD menu and perf overlay into the PPU framebuffers
     * BEFORE the NTSC pipeline runs, so they get the full composite
     * treatment (dot crawl, chroma bleed, scanlines, vignette) — like
     * a real 80s/90s TV's built-in OSD popping up over the game. */
    {
        const uint8_t (*pal)[3] =
            nes.ppu.color_palette ? nes.ppu.color_palette : ppu_palette_2c02;
        if (menu_open) menu_render_nes(nes.ppu.framebuffer, nes.ppu.index_framebuffer, pal);
        perf_draw_overlay(nes.ppu.framebuffer, nes.ppu.index_framebuffer, pal);
    }

    if (composite_enabled) {
        comp_process(&composite, nes.ppu.framebuffer, nes.ppu.index_framebuffer);
        SDL_UpdateTexture(composite_texture, NULL, composite.output, composite.out_w * 3);

        /* Crop overscan from the composite texture (hides filter edge
         * artifacts and matches real CRT viewing experience where TVs
         * cropped ~5-10% of each edge). */
        int tex_h = 240 * composite.rows_per_scanline;
        int ox_px = composite.overscan_x * composite.out_w / 256;
        int oy_px = composite.overscan_y * composite.rows_per_scanline;
        SDL_Rect src = {
            ox_px, oy_px,
            composite.out_w - 2 * ox_px,
            tex_h - 2 * oy_px
        };
        SDL_RenderCopy(renderer, composite_texture, &src, &dst);

        /* Scanlines AND aperture grille are baked into the composite
         * output (see comp_emit_output_row), so no GPU overlay needed. */
    } else {
        /* Non-composite path: apply the same overscan crop so the
         * visible image and pixel aspect match composite mode exactly. */
        SDL_UpdateTexture(texture, NULL, nes.ppu.framebuffer, PPU_WIDTH * 3);
        int vw, vh;
        comp_visible_pixels(&vw, &vh);
        SDL_Rect src_non = {
            composite.overscan_x, composite.overscan_y,
            vw, vh
        };
        SDL_RenderCopy(renderer, texture, &src_non, &dst);
    }

    SDL_RenderPresent(renderer);
}

/* ============================================================================
 * Dynamic Audio Rate Adjustment
 *
 * Nudge the APU's sample rate up or down based on how full the ring buffer
 * is.  This keeps the buffer near 50% full and prevents both underruns
 * (clicks) and overruns (dropped samples / pitch glitches).
 *
 * The adjustment is tiny — at most ±2% — so it's inaudible.
 * ============================================================================ */

static void audio_adjust_rate(void) {
    int fill = audio_ring_available();
    int error = fill - AUDIO_TARGET_FILL;

    /* Dead band: ignore small errors so the controller doesn't
     * chase transient noise in the fill level. 200 samples ≈ 5 ms
     * at 44.1 kHz — below audible pitch-tracking threshold. */
    const int DEAD_BAND = 200;
    int effective = 0;
    if (error > DEAD_BAND)        effective = error - DEAD_BAND;
    else if (error < -DEAD_BAND)  effective = error + DEAD_BAND;

    /* Integral accumulator with slow leak. The leak keeps the
     * integral from winding up indefinitely in pathological cases.
     * Called once per video frame (~60 Hz), so `effective` is in
     * "samples per frame" units; integrating over ~1 second gives
     * a total of ~60 * effective sample-frames. */
    const double LEAK = 0.02;            /* 2% leak per frame → τ ≈ 50 frames */
    const double INTEGRAL_GAIN = 0.015;  /* tiny — slow response */
    audio_ctrl.integral += (double)effective;
    audio_ctrl.integral *= (1.0 - LEAK);

    int adjust = (int)(audio_ctrl.integral * INTEGRAL_GAIN);
    if (adjust >  AUDIO_RATE_ADJUST_MAX) adjust =  AUDIO_RATE_ADJUST_MAX;
    if (adjust < -AUDIO_RATE_ADJUST_MAX) adjust = -AUDIO_RATE_ADJUST_MAX;

    /* Overfull → lower rate → fewer samples emitted per second.
     * Underfull → higher rate → more samples emitted. */
    nes.apu.sample_rate = APU_SAMPLE_RATE - adjust;
    audio_ctrl.last_fill = fill;
}

int main(int argc, char *argv[]) {
    /* No ROM argument is fine — we'll open the fullscreen ROM browser
     * once SDL is initialised. To keep the existing arg-parsing happy,
     * detect that case here and skip the help-print. */
    bool crt_usb_requested = false;
    bool show_help_and_exit = false;
    if (argc < 2) {
        /* Bare invocation → start the browser. */
    } else if (strcmp(argv[1], "-h") == 0 || strcmp(argv[1], "--help") == 0) {
        show_help_and_exit = true;
    }
    if (show_help_and_exit) {
        printf("Interactive NES Emulator (MyNES)\n");
        printf("Usage: %s [rom_file] [options]\n\n", argv[0]);
        printf("If no ROM is given, the fullscreen ROM browser opens.\n\n");
        printf("Options:\n");
        printf("  --crt-usb                Stream PPU frames to a CRT Tang Bridge\n");
        printf("  -f, --fast               Start in fast mode (no FPS limit)\n");
        printf("  --pal                    PAL region (312 scanlines, 50Hz)\n");
        printf("  --composite              Start with NTSC composite simulation on\n");
        printf("  --scale <n>              Window scale factor (1-8, default: 3)\n");
        printf("  --palette <file.pal>     Load custom palette file (192 bytes)\n");
        printf("  --simulate-frame <bin>   Replace emulation with a fixed\n");
        printf("                           256x240 palette-index frame (uint8)\n\n");
        printf("Controls (see README for the full list):\n");
        printf("  Arrow keys - D-pad   X=A   Z=B   Shift=Select   Enter=Start\n");
        printf("  M=menu  P=palette  C=composite  S=screenshot  O=open ROM browser\n");
        printf("  F=fullscreen  Tab=fast  Space=pause  Esc=quit\n");
        return 0;
    }

    const char *rom_path = NULL;
    const char *palette_path = NULL;

    /* First positional argument that isn't a recognised option is the ROM. */
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--crt-usb") == 0) { crt_usb_requested = true; continue; }
        if (argv[i][0] != '-' && !rom_path) { rom_path = argv[i]; continue; }
        if (strcmp(argv[i], "-f") == 0 || strcmp(argv[i], "--fast") == 0) {
            fast_mode = true;
        } else if (strcmp(argv[i], "--composite") == 0) {
            composite_enabled = true;
        } else if (strcmp(argv[i], "--no-composite") == 0) {
            composite_enabled = false;
        } else if (strcmp(argv[i], "--pal") == 0) {
            force_pal = true;
        } else if (strcmp(argv[i], "--scale") == 0 && i + 1 < argc) {
            char *endptr = NULL;
            errno = 0;
            long s = strtol(argv[++i], &endptr, 10);
            if (errno == 0 && endptr != argv[i] && s >= 1 && s <= 8) {
                scale = (int)s;
            } else {
                fprintf(stderr, "Invalid scale '%s', using default %d\n", argv[i], scale);
            }
        } else if (strcmp(argv[i], "--palette") == 0 && i + 1 < argc) {
            palette_path = argv[++i];
        } else if (strcmp(argv[i], "--screenshot-after") == 0 && i + 1 < argc) {
            screenshot_after_frames = atoi(argv[++i]);
            fast_mode = true;
        } else if (strcmp(argv[i], "--simulate-frame") == 0 && i + 1 < argc) {
            static_frame_idx = load_static_frame_bin(argv[++i]);
            if (!static_frame_idx) return 1;
            printf("Static-frame mode: emulation disabled, using %s\n",
                   argv[i]);
        } else if (strcmp(argv[i], "--auto-start") == 0 && i + 1 < argc) {
            auto_start_until = atoi(argv[++i]);
            printf("Auto-start: pulse BTN_START until frame %d\n", auto_start_until);
        } else if (strcmp(argv[i], "--poke") == 0 && i + 1 < argc) {
            /* Format: "addr=val@frame" in hex, e.g. --poke 30=7@600
             * Sets ram[$30]=$07 starting at frame 600 and every frame
             * thereafter (to counter the game writing over it on
             * subsequent logic updates). */
            const char *s = argv[++i];
            if (ram_pokes_n < POKES_MAX) {
                unsigned a, v; int f;
                if (sscanf(s, "%x=%x@%d", &a, &v, &f) == 3) {
                    ram_pokes[ram_pokes_n].addr = (uint16_t)a;
                    ram_pokes[ram_pokes_n].val  = (uint8_t)v;
                    ram_pokes[ram_pokes_n].trigger_frame = f;
                    ram_pokes[ram_pokes_n].done = 0;
                    ram_pokes_n++;
                    printf("RAM poke scheduled: $%04X=$%02X @ frame %d\n", a, v, f);
                } else {
                    fprintf(stderr, "Invalid --poke format: %s "
                            "(want addr=val@frame in hex)\n", s);
                }
            }
        }
    }

    const char *crt_usb_enabled = getenv("MYNES_CRT_USB");
    crt_usb_requested = crt_usb_requested ||
        (crt_usb_enabled && strcmp(crt_usb_enabled, "1") == 0);
#ifndef MYNES_CRT_CAPTURE
    if (crt_usb_requested) {
        fprintf(stderr, "CRT USB support is not built; configure with -DNES_CRT_USB=ON\n");
        return 1;
    }
#endif

    if (fast_mode) {
        printf("Fast mode enabled (no FPS limit)\n");
    }

    /* If the user gave a ROM, load it now. Otherwise the browser
     * (initialised after audio/video setup below) will pick one. */
    if (rom_path) {
        printf("Loading %s...\n", rom_path);
        int err = nes_rom_load(&rom, rom_path);
        if (err != ROM_OK) {
            printf("Error loading ROM: %s\n", nes_rom_error_str(err));
            return 1;
        }
        nes_rom_print_info(&rom);
        rom_loaded = true;
    }

    /* Initialize palettes */
    palette_init(argv[0]);

    /* Load --palette from command line (overrides default) */
    if (palette_path) {
        uint8_t custom_colors[64][3];
        if (palette_load_file(palette_path, custom_colors)) {
            /* Add as first entry after built-in so it becomes current */
            const char *slash = strrchr(palette_path, '/');
            const char *name = slash ? slash + 1 : palette_path;

            /* palette_init may have filled the table from the palettes
             * directory; the one asked for by name takes the last slot. */
            if (palette_count >= MAX_PALETTES) {
                palette_count = MAX_PALETTES - 1;
                fprintf(stderr, "Palette table full; %s replaces %s\n",
                        name, palettes[palette_count].name);
            }
            PaletteEntry *pe = &palettes[palette_count];
            strncpy(pe->name, name, sizeof(pe->name) - 1);
            memcpy(pe->colors, custom_colors, sizeof(pe->colors));
            current_palette = palette_count;
            palette_count++;
            printf("Using custom palette: %s\n", palette_path);
        } else {
            fprintf(stderr, "Failed to load palette: %s (must be 192 bytes)\n", palette_path);
        }
    }

    /* Initialize SDL (video + audio) */
    /* Nearest-neighbor scaling: avoids linear-filtering moire when
     * stretching the 480-row composite texture to window height. */
    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "0");
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_GAMECONTROLLER) < 0) {
        printf("SDL init failed: %s\n", SDL_GetError());
        return 1;
    }

    /* Always derive width from height using the 8:7 NTSC TV aspect,
     * regardless of composite/non-composite mode. NES pixels are
     * non-square on actual hardware (8:7), so the "square pixel" look
     * of non-composite was technically wrong. Using the TV aspect in
     * both modes makes the image proportions consistent when toggling
     * composite at runtime.
     *
     * At startup, composite has not yet been initialized so we use the
     * default overscan (0 per side, matching comp_init). After
     * comp_init runs later, comp_reconfigure_for_window reconciles
     * the actual overscan and snaps dst sizes accordingly. */
    const int default_overscan_x = 0;
    const int default_overscan_y = 0;
    int visible_startup_w = PPU_WIDTH  - 2 * default_overscan_x;
    int visible_startup_h = PPU_HEIGHT - 2 * default_overscan_y;
    int win_h = visible_startup_h * scale;
    const float src_aspect_tv =
        (float)visible_startup_w * (8.0f / 7.0f) / (float)visible_startup_h;
    int win_w = (int)((float)win_h * src_aspect_tv + 0.5f);
    window = SDL_CreateWindow(
        "MyNES",
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        win_w, win_h,
        SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI
    );
    if (!window) {
        printf("Window creation failed: %s\n", SDL_GetError());
        SDL_Quit();
        return 1;
    }
    /* The pointer stays hidden over the picture, windowed or fullscreen,
     * while MyNES is the active app. */
    SDL_ShowCursor(SDL_DISABLE);

    /*
     * Vsync ON by default — kills horizontal tearing on content with
     * moving backgrounds. Frame pacing is still driven by the audio
     * ring buffer's ~2% rate-adjust window, which easily absorbs the
     * 0.16% mismatch between NES 60.0988 Hz and display 60 Hz (or
     * integer-division on 120/144/165 Hz displays). Togglable via the
     * SETUP → Effects menu at runtime using SDL_RenderSetVSync.
     */
    Uint32 renderer_flags = SDL_RENDERER_ACCELERATED;
    if (vsync_enabled) renderer_flags |= SDL_RENDERER_PRESENTVSYNC;
    renderer = SDL_CreateRenderer(window, -1, renderer_flags);
    if (!renderer) {
        printf("Renderer creation failed: %s\n", SDL_GetError());
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 1;
    }

    texture = SDL_CreateTexture(
        renderer,
        SDL_PIXELFORMAT_RGB24,
        SDL_TEXTUREACCESS_STREAMING,
        PPU_WIDTH, PPU_HEIGHT
    );
    if (!texture) {
        printf("Texture creation failed: %s\n", SDL_GetError());
        SDL_DestroyRenderer(renderer);
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 1;
    }

    /* NTSC composite: width 4x with 8:7 aspect for crisp horizontal
     * filter resolution. Vertical resolution matches the window's scale
     * factor so the scanline pattern maps 1:1 to window pixels — avoids
     * moire from non-integer scaling. */
    {
        int comp_w = PPU_WIDTH * 4 * 8 / 7;
        int rps = scale;  /* rows per scanline = display scale factor */
        if (rps < 1) rps = 1;
        if (rps > 4) rps = 4;
        comp_init(&composite, comp_w, rps);
        composite_texture = SDL_CreateTexture(
            renderer,
            SDL_PIXELFORMAT_RGB24,
            SDL_TEXTUREACCESS_STREAMING,
            comp_w, 240 * rps
        );
    }

    /* OSD overlay texture — RGBA so we can alpha-blend over the game. */
    osd_texture = SDL_CreateTexture(
        renderer,
        SDL_PIXELFORMAT_RGBA32,
        SDL_TEXTUREACCESS_STREAMING,
        OSD_W, OSD_H
    );
    if (osd_texture) {
        SDL_SetTextureBlendMode(osd_texture, SDL_BLENDMODE_BLEND);
    }

    /* Initialize audio */
    audio_ring_init();

    SDL_AudioSpec want, have;
    memset(&want, 0, sizeof(want));
    want.freq = 44100;
    want.format = AUDIO_F32SYS;
    want.channels = 1;
    want.samples = 512;   /* smaller buffer → lower latency */
    want.callback = sdl_audio_callback;

    audio_device = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
    if (audio_device == 0) {
        printf("Warning: Audio device failed: %s (continuing without audio)\n", SDL_GetError());
    } else {
        printf("Audio: %dHz, %d-sample buffer\n", have.freq, have.samples);
        SDL_PauseAudioDevice(audio_device, 0);
    }

    /* Initialize NES */
    nes_init(&nes);
    if (rom_loaded) {
        nes_load_mapper(&nes, rom.mapper,
                        rom.prg_rom, rom.prg_size,
                        rom.chr_rom, rom.chr_size,
                        rom.mirroring);
        nes_rom_apply_trainer(&rom, &nes.mapper);
        saves_attach(rom_path);
    }

    apu_set_audio_callback(&nes.apu, apu_sample_callback, NULL);

    /* Region — auto-detected from the ROM header, or forced by --pal. */
    int region = apply_rom_region();

#ifdef MYNES_CRT_CAPTURE
    const char *crt_capture_path = getenv("MYNES_CRT_CAPTURE");
    if (crt_capture_path && *crt_capture_path) {
        crt_capture_file = fopen(crt_capture_path, "wb");
        if (!crt_capture_file) { perror("CRT capture"); return 1; }
    }
#endif

#ifdef MYNES_CRT_CAPTURE
    if (crt_usb_requested) {
        char error[256];
        crt_usb = crt_live_open(error, sizeof(error));
        if (!crt_usb) {
            fprintf(stderr, "CRT USB: %s\n", error);
            if (crt_capture_file) fclose(crt_capture_file);
            return 1;
        }
    }
#endif

    /* Apply the region-appropriate default palette to the PPU, unless
     * the user passed --palette on the command line (in which case
     * current_palette was already set during argument parsing and we
     * respect that choice).
     *
     * PAL ROMs: "2C07 PAL (built-in)" — palettes[1], the canonical
     *           hardcoded PAL RGB values. The composite signal table
     *           is synthesized to decode to exactly these values
     *           (see comp_precompute_signal_table_pal), so wave-on
     *           and wave-off show matching hues, with composite
     *           effects layered on top.
     * NTSC ROMs: Digital Prime (FBX) if loaded, else 2C02 built-in. */
    if (current_palette == 0) {
        int picked = -1;
        if (region == NES_REGION_PAL) {
            picked = palette_find_by_name("2C07 PAL (built-in)");
        } else {
            picked = palette_find_by_name("Digital Prime");
        }
        if (picked >= 0) current_palette = picked;
    }
    if (current_palette >= 0 && current_palette < palette_count) {
        nes.ppu.color_palette = palettes[current_palette].colors;
        printf("Palette: %s\n", palettes[current_palette].name);
    }

    if (rom_loaded) nes_reset(&nes);

    /* Static-frame mode auto-applies the CRT Match preset — it was
     * tuned specifically against the IMG_1766 reference photo with
     * this mode in mind. Force composite ON so the preset takes
     * effect even if the user launched with --no-composite. */
    if (static_frame_idx) {
        composite_enabled = true;
        comp_apply_preset(&comp_presets[COMP_PRESET_CRT_MATCH]);
    }

    /* Persistent config + ROM browser. If no ROM was on argv, open the
     * browser at startup; the first matching ROM the user picks is
     * loaded in-place and emulation begins. */
    mynes_config_load(&mynes_config);
    if (rom_loaded) {
        mynes_config_add_recent(&mynes_config, rom_path);
        mynes_config_save(&mynes_config);
    } else {
        browser_init(&browser, NULL, &mynes_config);
        browser_active = true;
        SDL_StartTextInput();
    }

    printf("\nRunning... Press ESC to quit\n");

    /*
     * Main loop — timer-driven with frame skipping.
     *
     * Game logic runs at a fixed rate (60 Hz NTSC / 50 Hz PAL).
     * When rendering (especially NTSC composite) is too slow, we skip
     * rendering frames to keep the game running at the correct speed.
     * Audio stays smooth because game logic always runs on time.
     */
    Uint64 freq = SDL_GetPerformanceFrequency();
    Uint64 frame_ticks = 0;
    Uint64 next_frame = SDL_GetPerformanceCounter();
    int frames_behind = 0;
    const int MAX_SKIP = 4;

    /* Performance counters (printed every 60 frames) */
    Uint64 perf_emu_total = 0, perf_render_total = 0;
    int perf_frames = 0, perf_skips = 0;
    Uint64 perf_last_print = SDL_GetPerformanceCounter();

    while (running) {
        handle_input();
        /* A ROM loaded from the browser can change the region. */
        frame_ticks = freq / (nes_region_current == NES_REGION_PAL ? 50 : 60);
        /* Battery RAM reaches disk within a couple of seconds of a change. */
        if (saves.battery && SDL_GetTicks() >= battery_next_check) {
            battery_next_check = SDL_GetTicks() + 2000;
            mynes_saves_flush(&saves, nes.mapper.prg_ram, false);
        }

        Uint64 now = SDL_GetPerformanceCounter();

        if (!paused && !fast_mode) {
            /* Run game frames to catch up with real time */
            frames_behind = 0;
            Uint64 t0 = SDL_GetPerformanceCounter();
            while (next_frame <= now && frames_behind < MAX_SKIP) {
                if (audio_device && !static_frame_idx)
                    audio_adjust_rate();
                if (browser_active || !rom_loaded) {
                    /* No game frame to run while the ROM browser owns
                     * the screen — the framebuffer is painted below. */
                } else if (static_frame_idx) {
                    apply_static_frame_to_ppu();
                } else {
                    nes_run_frame(&nes);
#ifdef MYNES_CRT_CAPTURE
                    if (crt_usb && crt_live_submit(crt_usb, nes.ppu.index_framebuffer)) {
                        fprintf(stderr, "CRT USB stopped; emulator preview continues\n");
                        crt_live_close(crt_usb);
                        crt_usb = NULL;
                    }
                    if (crt_capture_file && crt_capture_frame(crt_capture_file, nes.ppu.index_framebuffer)) {
                        fprintf(stderr, "CRT capture write failed\n");
                        crt_capture_failed = 1;
                        running = false;
                        break;
                    }
#endif
                    frame_counter++;
                    /* Apply scheduled RAM pokes after the frame runs.
                     * We poke AFTER so our values win against the
                     * game's frame-start initialization. */
                    for (int k = 0; k < ram_pokes_n; k++) {
                        if (frame_counter >= ram_pokes[k].trigger_frame)
                            nes.ram[ram_pokes[k].addr & 0x07FF] = ram_pokes[k].val;
                    }
                }
                next_frame += frame_ticks;
                frames_behind++;
            }

            /* Paint the browser into the PPU framebuffer so render_frame
             * (and the composite pipeline if enabled) treats it like a
             * normal game frame. Same osd_nesfb_* primitives, same path. */
            if (browser_active && current_palette >= 0
                && current_palette < palette_count) {
                browser_render(&browser,
                    nes.ppu.framebuffer, nes.ppu.index_framebuffer,
                    palettes[current_palette].colors);
            }
            Uint64 t1 = SDL_GetPerformanceCounter();
            perf_emu_total += t1 - t0;
            if (frames_behind > 1)
                perf_skips += frames_behind - 1;

            /* If we're still behind, reset the clock (avoid death spiral) */
            if (next_frame <= now) {
                next_frame = now + frame_ticks;
            }

            /* Render only the latest frame */
            Uint64 t2 = SDL_GetPerformanceCounter();
            render_frame();
            Uint64 t3 = SDL_GetPerformanceCounter();
            perf_render_total += t3 - t2;
            perf_frames++;

            /* Update performance stats overlay string once per second.
             * We used to printf this to stdout, but terminal writes
             * cause per-second jitter — the write to pty + scrollback
             * stalls for ~milliseconds. Writing to a static buffer is
             * O(1) and has no IO side-effect. render_frame() picks it
             * up and draws it into the framebuffer. */
            if (t3 - perf_last_print >= freq) {
                double emu_ms = (double)perf_emu_total * 1000.0 / freq / perf_frames;
                double render_ms = (double)perf_render_total * 1000.0 / freq / perf_frames;
                double total_ms = emu_ms + render_ms;
                snprintf(perf_text, sizeof(perf_text),
                         "EMU %.1f REN %.1f TOT %.1f SKP %d",
                         emu_ms, render_ms, total_ms, perf_skips);
                perf_emu_total = perf_render_total = 0;
                perf_frames = 0; perf_skips = 0;
                perf_last_print = t3;
            }

            /* Sleep until next frame is due */
            now = SDL_GetPerformanceCounter();
            if (next_frame > now) {
                Uint32 sleep_ms = (Uint32)((next_frame - now) * 1000 / freq);
                if (sleep_ms > 1)
                    SDL_Delay(sleep_ms - 1);
            }
        } else if (!paused && fast_mode) {
            if (browser_active || !rom_loaded) {
                if (browser_active && current_palette >= 0
                    && current_palette < palette_count) {
                    browser_render(&browser,
                        nes.ppu.framebuffer, nes.ppu.index_framebuffer,
                        palettes[current_palette].colors);
                }
            } else if (static_frame_idx) {
                apply_static_frame_to_ppu();
            } else {
                nes_run_frame(&nes);
#ifdef MYNES_CRT_CAPTURE
                if (crt_usb && crt_live_submit(crt_usb, nes.ppu.index_framebuffer)) {
                    fprintf(stderr, "CRT USB stopped; emulator preview continues\n");
                    crt_live_close(crt_usb);
                    crt_usb = NULL;
                }
                if (crt_capture_file && crt_capture_frame(crt_capture_file, nes.ppu.index_framebuffer)) {
                    fprintf(stderr, "CRT capture write failed\n");
                    crt_capture_failed = 1;
                    running = false;
                    break;
                }
#endif
                frame_counter++;
                for (int k = 0; k < ram_pokes_n; k++) {
                    if (frame_counter >= ram_pokes[k].trigger_frame)
                        nes.ram[ram_pokes[k].addr & 0x07FF] = ram_pokes[k].val;
                }
            }
            render_frame();
            if (screenshot_after_frames > 0) {
                if (frame_counter >= screenshot_after_frames) {
                    save_screenshot();
                    running = false;
                }
            }
        } else {
            render_frame();
            SDL_Delay(16);
        }
    }

#ifdef MYNES_CRT_CAPTURE
    if (crt_capture_file && fclose(crt_capture_file)) crt_capture_failed = 1;
    if (crt_usb) {
        uint64_t sent, replaced;
        crt_live_stats(crt_usb, &sent, &replaced);
        fprintf(stderr, "CRT USB: %llu verified frames, %llu replaced pending frames\n",
                (unsigned long long)sent, (unsigned long long)replaced);
        crt_live_close(crt_usb);
    }
#endif

    printf("\nShutting down...\n");

    if (audio_device) {
        SDL_PauseAudioDevice(audio_device, 1);
        SDL_CloseAudioDevice(audio_device);
    }

    comp_destroy(&composite);
    for (int i = 0; i < 2; i++)
        if (controllers[i]) SDL_GameControllerClose(controllers[i]);
    if (composite_texture) SDL_DestroyTexture(composite_texture);
    SDL_DestroyTexture(texture);
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();

    if (saves.battery) mynes_saves_flush(&saves, nes.mapper.prg_ram, false);
    nes_rom_free(&rom);

#ifdef MYNES_CRT_CAPTURE
    return crt_capture_failed ? 1 : 0;
#else
    return 0;
#endif
}
