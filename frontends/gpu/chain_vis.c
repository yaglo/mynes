/*
 * Chain Visualiser Overlay -- Implementation (Phase A)
 * =====================================================
 *
 * Renders a compact two-column stage list into a 256x240 uint8 overlay
 * buffer. Each byte is a NES palette index so the overlay can be
 * composited onto the PPU framebuffer before the composite pipeline
 * runs (giving it that authentic CRT look) or blitted clean on top.
 *
 * Layout (256 x 240):
 *
 *   Row 0:    padding
 *   Row 2:    "CHAIN: <preset name>"
 *   Row 12:   "VIDEO"               "AUDIO"
 *   Row 20+:  stage rows (9px each) stage rows (9px each)
 *   Bottom:   "Total: X.XXms"
 *
 * Mini font: built-in 4x6 pixel glyphs for printable ASCII 0x20-0x7E.
 * Intentionally tiny so 14 video + 10 audio stages fit on one screen.
 */

#include "chain_vis.h"
#include "preset_apply.h"
#include <SDL3/SDL.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* ============================================================================
 * Constants
 * ============================================================================ */

#define VIS_W   256
#define VIS_H   240

/* NES palette indices used for drawing. */
#define PAL_BG          0x0F    /* black */
#define PAL_TEXT        0x30    /* white */
#define PAL_DIM         0x00    /* dark grey */
#define PAL_HIGHLIGHT   0x21    /* cyan */
#define PAL_BYPASS      0x16    /* red-ish */
#define PAL_HEADER      0x28    /* yellow */
#define PAL_BAR_FILL    0x1A    /* green */
#define PAL_BAR_EMPTY   0x2D    /* dark */
#define PAL_SEPARATOR   0x00    /* dark grey */

/* Layout geometry. */
#define HEADER_Y        3
#define COLUMNS_Y       13
#define STAGE_START_Y   22
#define STAGE_ROW_H     9       /* pixels per stage row */
#define VIDEO_COL_X     2
#define AUDIO_COL_X     130
#define FOOTER_Y        (VIS_H - 12)

/* Column: which chain is selected. */
#define COL_VIDEO  0
#define COL_AUDIO  1

/* Stage counts. */
#define VIDEO_STAGE_COUNT  14
#define AUDIO_STAGE_COUNT  10

/* ============================================================================
 * Mini bitmap font (4x6 pixels, printable ASCII 0x20 .. 0x7E)
 * ============================================================================
 * Each glyph is 6 rows of 4 bits, packed into 3 bytes (6 nibbles).
 * Bit order within each nibble: MSB = leftmost pixel.
 *
 * Encoding: glyph[ch - 0x20][row] has 4 bits, stored in the high nibble
 * of successive nibble pairs. We pack two rows per byte for compactness.
 * Byte 0 = (row0 << 4) | row1, Byte 1 = (row2 << 4) | row3, etc.
 */

/* Simpler encoding: 6 bytes per glyph, each byte's top 4 bits are the
 * pixel row (MSB = leftmost). Only 95 printable chars * 6 = 570 bytes. */
static const uint8_t mini_font[95][6] = {
    /* 0x20 ' ' */ { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 },
    /* 0x21 '!' */ { 0x40, 0x40, 0x40, 0x00, 0x40, 0x00 },
    /* 0x22 '"' */ { 0xA0, 0xA0, 0x00, 0x00, 0x00, 0x00 },
    /* 0x23 '#' */ { 0xA0, 0xF0, 0xA0, 0xF0, 0xA0, 0x00 },
    /* 0x24 '$' */ { 0x60, 0xC0, 0x60, 0x30, 0x60, 0x00 },
    /* 0x25 '%' */ { 0x90, 0x20, 0x40, 0x80, 0x90, 0x00 },
    /* 0x26 '&' */ { 0x40, 0xA0, 0x40, 0xA0, 0x50, 0x00 },
    /* 0x27 ''' */ { 0x40, 0x40, 0x00, 0x00, 0x00, 0x00 },
    /* 0x28 '(' */ { 0x20, 0x40, 0x40, 0x40, 0x20, 0x00 },
    /* 0x29 ')' */ { 0x40, 0x20, 0x20, 0x20, 0x40, 0x00 },
    /* 0x2A '*' */ { 0x00, 0xA0, 0x40, 0xA0, 0x00, 0x00 },
    /* 0x2B '+' */ { 0x00, 0x40, 0xE0, 0x40, 0x00, 0x00 },
    /* 0x2C ',' */ { 0x00, 0x00, 0x00, 0x40, 0x80, 0x00 },
    /* 0x2D '-' */ { 0x00, 0x00, 0xE0, 0x00, 0x00, 0x00 },
    /* 0x2E '.' */ { 0x00, 0x00, 0x00, 0x00, 0x40, 0x00 },
    /* 0x2F '/' */ { 0x10, 0x20, 0x40, 0x80, 0x00, 0x00 },
    /* 0x30 '0' */ { 0x60, 0xB0, 0xD0, 0x90, 0x60, 0x00 },
    /* 0x31 '1' */ { 0x40, 0xC0, 0x40, 0x40, 0xE0, 0x00 },
    /* 0x32 '2' */ { 0x60, 0x90, 0x20, 0x40, 0xF0, 0x00 },
    /* 0x33 '3' */ { 0xE0, 0x20, 0x60, 0x20, 0xE0, 0x00 },
    /* 0x34 '4' */ { 0x20, 0x60, 0xA0, 0xF0, 0x20, 0x00 },
    /* 0x35 '5' */ { 0xF0, 0x80, 0xE0, 0x10, 0xE0, 0x00 },
    /* 0x36 '6' */ { 0x60, 0x80, 0xE0, 0x90, 0x60, 0x00 },
    /* 0x37 '7' */ { 0xF0, 0x10, 0x20, 0x40, 0x40, 0x00 },
    /* 0x38 '8' */ { 0x60, 0x90, 0x60, 0x90, 0x60, 0x00 },
    /* 0x39 '9' */ { 0x60, 0x90, 0x70, 0x10, 0x60, 0x00 },
    /* 0x3A ':' */ { 0x00, 0x40, 0x00, 0x40, 0x00, 0x00 },
    /* 0x3B ';' */ { 0x00, 0x40, 0x00, 0x40, 0x80, 0x00 },
    /* 0x3C '<' */ { 0x20, 0x40, 0x80, 0x40, 0x20, 0x00 },
    /* 0x3D '=' */ { 0x00, 0xE0, 0x00, 0xE0, 0x00, 0x00 },
    /* 0x3E '>' */ { 0x80, 0x40, 0x20, 0x40, 0x80, 0x00 },
    /* 0x3F '?' */ { 0x60, 0x90, 0x20, 0x00, 0x20, 0x00 },
    /* 0x40 '@' */ { 0x60, 0x90, 0xB0, 0x80, 0x70, 0x00 },
    /* 0x41 'A' */ { 0x60, 0x90, 0xF0, 0x90, 0x90, 0x00 },
    /* 0x42 'B' */ { 0xE0, 0x90, 0xE0, 0x90, 0xE0, 0x00 },
    /* 0x43 'C' */ { 0x60, 0x90, 0x80, 0x90, 0x60, 0x00 },
    /* 0x44 'D' */ { 0xE0, 0x90, 0x90, 0x90, 0xE0, 0x00 },
    /* 0x45 'E' */ { 0xF0, 0x80, 0xE0, 0x80, 0xF0, 0x00 },
    /* 0x46 'F' */ { 0xF0, 0x80, 0xE0, 0x80, 0x80, 0x00 },
    /* 0x47 'G' */ { 0x60, 0x80, 0xB0, 0x90, 0x60, 0x00 },
    /* 0x48 'H' */ { 0x90, 0x90, 0xF0, 0x90, 0x90, 0x00 },
    /* 0x49 'I' */ { 0xE0, 0x40, 0x40, 0x40, 0xE0, 0x00 },
    /* 0x4A 'J' */ { 0x70, 0x10, 0x10, 0x90, 0x60, 0x00 },
    /* 0x4B 'K' */ { 0x90, 0xA0, 0xC0, 0xA0, 0x90, 0x00 },
    /* 0x4C 'L' */ { 0x80, 0x80, 0x80, 0x80, 0xF0, 0x00 },
    /* 0x4D 'M' */ { 0x90, 0xF0, 0xF0, 0x90, 0x90, 0x00 },
    /* 0x4E 'N' */ { 0x90, 0xD0, 0xB0, 0x90, 0x90, 0x00 },
    /* 0x4F 'O' */ { 0x60, 0x90, 0x90, 0x90, 0x60, 0x00 },
    /* 0x50 'P' */ { 0xE0, 0x90, 0xE0, 0x80, 0x80, 0x00 },
    /* 0x51 'Q' */ { 0x60, 0x90, 0x90, 0xA0, 0x50, 0x00 },
    /* 0x52 'R' */ { 0xE0, 0x90, 0xE0, 0xA0, 0x90, 0x00 },
    /* 0x53 'S' */ { 0x70, 0x80, 0x60, 0x10, 0xE0, 0x00 },
    /* 0x54 'T' */ { 0xE0, 0x40, 0x40, 0x40, 0x40, 0x00 },
    /* 0x55 'U' */ { 0x90, 0x90, 0x90, 0x90, 0x60, 0x00 },
    /* 0x56 'V' */ { 0x90, 0x90, 0x90, 0x60, 0x60, 0x00 },
    /* 0x57 'W' */ { 0x90, 0x90, 0xF0, 0xF0, 0x90, 0x00 },
    /* 0x58 'X' */ { 0x90, 0x90, 0x60, 0x90, 0x90, 0x00 },
    /* 0x59 'Y' */ { 0x90, 0x90, 0x60, 0x40, 0x40, 0x00 },
    /* 0x5A 'Z' */ { 0xF0, 0x20, 0x40, 0x80, 0xF0, 0x00 },
    /* 0x5B '[' */ { 0x60, 0x40, 0x40, 0x40, 0x60, 0x00 },
    /* 0x5C '\' */ { 0x80, 0x40, 0x20, 0x10, 0x00, 0x00 },
    /* 0x5D ']' */ { 0x60, 0x20, 0x20, 0x20, 0x60, 0x00 },
    /* 0x5E '^' */ { 0x40, 0xA0, 0x00, 0x00, 0x00, 0x00 },
    /* 0x5F '_' */ { 0x00, 0x00, 0x00, 0x00, 0xF0, 0x00 },
    /* 0x60 '`' */ { 0x80, 0x40, 0x00, 0x00, 0x00, 0x00 },
    /* 0x61 'a' */ { 0x00, 0x70, 0x90, 0x90, 0x70, 0x00 },
    /* 0x62 'b' */ { 0x80, 0xE0, 0x90, 0x90, 0xE0, 0x00 },
    /* 0x63 'c' */ { 0x00, 0x70, 0x80, 0x80, 0x70, 0x00 },
    /* 0x64 'd' */ { 0x10, 0x70, 0x90, 0x90, 0x70, 0x00 },
    /* 0x65 'e' */ { 0x00, 0x60, 0xF0, 0x80, 0x70, 0x00 },
    /* 0x66 'f' */ { 0x30, 0x40, 0xE0, 0x40, 0x40, 0x00 },
    /* 0x67 'g' */ { 0x00, 0x70, 0x90, 0x70, 0x10, 0x60 },
    /* 0x68 'h' */ { 0x80, 0xE0, 0x90, 0x90, 0x90, 0x00 },
    /* 0x69 'i' */ { 0x40, 0x00, 0x40, 0x40, 0x40, 0x00 },
    /* 0x6A 'j' */ { 0x20, 0x00, 0x20, 0x20, 0xA0, 0x40 },
    /* 0x6B 'k' */ { 0x80, 0x90, 0xE0, 0xA0, 0x90, 0x00 },
    /* 0x6C 'l' */ { 0xC0, 0x40, 0x40, 0x40, 0xE0, 0x00 },
    /* 0x6D 'm' */ { 0x00, 0xF0, 0xF0, 0x90, 0x90, 0x00 },
    /* 0x6E 'n' */ { 0x00, 0xE0, 0x90, 0x90, 0x90, 0x00 },
    /* 0x6F 'o' */ { 0x00, 0x60, 0x90, 0x90, 0x60, 0x00 },
    /* 0x70 'p' */ { 0x00, 0xE0, 0x90, 0xE0, 0x80, 0x80 },
    /* 0x71 'q' */ { 0x00, 0x70, 0x90, 0x70, 0x10, 0x10 },
    /* 0x72 'r' */ { 0x00, 0xB0, 0xC0, 0x80, 0x80, 0x00 },
    /* 0x73 's' */ { 0x00, 0x70, 0xC0, 0x30, 0xE0, 0x00 },
    /* 0x74 't' */ { 0x40, 0xE0, 0x40, 0x40, 0x30, 0x00 },
    /* 0x75 'u' */ { 0x00, 0x90, 0x90, 0x90, 0x70, 0x00 },
    /* 0x76 'v' */ { 0x00, 0x90, 0x90, 0x60, 0x60, 0x00 },
    /* 0x77 'w' */ { 0x00, 0x90, 0xF0, 0xF0, 0x90, 0x00 },
    /* 0x78 'x' */ { 0x00, 0x90, 0x60, 0x60, 0x90, 0x00 },
    /* 0x79 'y' */ { 0x00, 0x90, 0x90, 0x70, 0x10, 0x60 },
    /* 0x7A 'z' */ { 0x00, 0xF0, 0x20, 0x40, 0xF0, 0x00 },
    /* 0x7B '{' */ { 0x20, 0x40, 0xC0, 0x40, 0x20, 0x00 },
    /* 0x7C '|' */ { 0x40, 0x40, 0x40, 0x40, 0x40, 0x00 },
    /* 0x7D '}' */ { 0x80, 0x40, 0x60, 0x40, 0x80, 0x00 },
    /* 0x7E '~' */ { 0x50, 0xA0, 0x00, 0x00, 0x00, 0x00 },
};

/* ============================================================================
 * Stage name tables
 * ============================================================================ */

static const char *video_stage_names[VIDEO_STAGE_COUNT] = {
    "2C02 DAC",         /*  1 */
    "Console Out",      /*  2 */
    "Cable",            /*  3 */
    "RF Mod/Demod",     /*  4 */
    "TV Input",         /*  5 */
    "Comb Filter",      /*  6 */
    "Chroma Demod",     /*  7 */
    "Luma Process",     /*  8 */
    "Matrix Decode",    /*  9 */
    "Video Amp",        /* 10 */
    "Beam",             /* 11 */
    "Phosphor",         /* 12 */
    "CRT Glass",        /* 13 */
    "Environment",      /* 14 */
};

static const char *audio_stage_names[AUDIO_STAGE_COUNT] = {
    "Coupling Cap",     /*  1 */
    "Feedback HP",      /*  2 */
    "Amp BW LP",        /*  3 */
    "Saturation",       /*  4 */
    "PSU Hum",          /*  5 */
    "Noise Floor",      /*  6 */
    "Cable",            /*  7 */
    "TV Input",         /*  8 */
    "Speaker",          /*  9 */
    "Decimate",         /* 10 */
};

/* ============================================================================
 * ChainVis struct
 * ============================================================================ */

struct ChainVis {
    /* Borrowed references. */
    const VideoChain *vc;
    const AudioChain *ac;

    /* UI state. */
    bool visible;
    int  selected_col;      /* COL_VIDEO or COL_AUDIO */
    int  selected_row;      /* 0-based index into the selected column */

    /* Per-stage bypass flags (independent of chain active state). */
    bool video_bypass[VIDEO_STAGE_COUNT];
    bool audio_bypass[AUDIO_STAGE_COUNT];

    /* Per-stage timing in microseconds (filled from GpuTimingLog later). */
    float video_time_us[VIDEO_STAGE_COUNT];
    float audio_time_us[AUDIO_STAGE_COUNT];

    /* Overlay framebuffer: 256 x 240, palette-indexed. */
    uint8_t buf[VIS_W * VIS_H];
};

/* ============================================================================
 * Drawing primitives
 * ============================================================================ */

static inline void vis_put(uint8_t *buf, int x, int y, uint8_t pal) {
    if (x >= 0 && x < VIS_W && y >= 0 && y < VIS_H)
        buf[y * VIS_W + x] = pal;
}

/* Draw a single glyph. Returns advance width (5 pixels). */
static int vis_glyph(uint8_t *buf, int x, int y, char ch, uint8_t pal) {
    if (ch < 0x20 || ch > 0x7E) return 5;
    const uint8_t *g = mini_font[ch - 0x20];
    for (int row = 0; row < 6; row++) {
        uint8_t bits = g[row];
        for (int col = 0; col < 4; col++) {
            if (bits & (0x80 >> col))
                vis_put(buf, x + col, y + row, pal);
        }
    }
    return 5; /* 4px glyph + 1px spacing */
}

/* Draw a string. Returns the X position after the last character. */
static int vis_text(uint8_t *buf, int x, int y, const char *s, uint8_t pal) {
    while (*s) {
        x += vis_glyph(buf, x, y, *s, pal);
        s++;
    }
    return x;
}

/* Draw a string with strikethrough (horizontal line through the middle). */
static int vis_text_strike(uint8_t *buf, int x, int y, const char *s,
                           uint8_t pal) {
    int x0 = x;
    x = vis_text(buf, x, y, s, pal);
    /* Draw strike line at vertical center (row 2 of 6). */
    int strike_y = y + 3;
    for (int sx = x0; sx < x; sx++)
        vis_put(buf, sx, strike_y, pal);
    return x;
}

/* Draw a filled rectangle. */
static void vis_rect(uint8_t *buf, int x, int y, int w, int h, uint8_t pal) {
    for (int row = y; row < y + h; row++)
        for (int col = x; col < x + w; col++)
            vis_put(buf, col, row, pal);
}

/* Draw a horizontal line. */
static void vis_hline(uint8_t *buf, int x, int y, int w, uint8_t pal) {
    for (int col = x; col < x + w; col++)
        vis_put(buf, col, y, pal);
}

/* Draw a timing bar. max_w = maximum bar width in pixels, frac = 0..1. */
static void vis_bar(uint8_t *buf, int x, int y, int max_w, float frac,
                    uint8_t fill_pal, uint8_t empty_pal) {
    if (frac < 0.0f) frac = 0.0f;
    if (frac > 1.0f) frac = 1.0f;
    int filled = (int)(frac * max_w + 0.5f);
    vis_rect(buf, x, y, filled, 5, fill_pal);
    vis_rect(buf, x + filled, y, max_w - filled, 5, empty_pal);
}

/* Format a float as milliseconds: "X.XX" into a caller buffer. */
static void fmt_ms(char *out, int out_sz, float us) {
    float ms = us / 1000.0f;
    snprintf(out, out_sz, "%.2f", (double)ms);
}

/* ============================================================================
 * Audio chain stage active query
 * ============================================================================
 * Unlike video, the audio chain uses per-stage `enabled` flags directly.
 */

static bool audio_stage_enabled(const AudioChain *ac, int stage) {
    switch (stage) {
    case 0:  return ac->coupling_cap.enabled;
    case 1:  return ac->feedback_network.enabled;
    case 2:  return ac->amp_bandwidth.enabled;
    case 3:  return ac->amp_saturation.enabled;
    case 4:  return ac->psu_hum.enabled;
    case 5:  return ac->noise_floor.enabled;
    case 6:  return ac->cable.enabled;
    case 7:  return ac->tv_input_coupling.enabled;
    case 8:  return ac->speaker.enabled;
    case 9:  return ac->decimation.enabled;
    default: return false;
    }
}

/* ============================================================================
 * Public API
 * ============================================================================ */

ChainVis *chain_vis_create(const VideoChain *vc, const AudioChain *ac) {
    ChainVis *vis = (ChainVis *)calloc(1, sizeof(ChainVis));
    if (!vis) return NULL;
    vis->vc = vc;
    vis->ac = ac;
    vis->visible = false;
    vis->selected_col = COL_VIDEO;
    vis->selected_row = 0;
    return vis;
}

void chain_vis_destroy(ChainVis *vis) {
    free(vis);
}

void chain_vis_toggle(ChainVis *vis) {
    if (vis) vis->visible = !vis->visible;
}

bool chain_vis_is_open(const ChainVis *vis) {
    return vis && vis->visible;
}

/* ---- Rendering ---------------------------------------------------------- */

/* Draw one stage row. Returns nothing.
 *   col_x:    left pixel of the column
 *   y:        top pixel of this row
 *   name:     stage name
 *   active:   true if the connection type enables this stage
 *   bypassed: true if the user has toggled bypass
 *   time_us:  dispatch time in microseconds (0 = no data)
 *   selected: true if this is the cursor row
 *   max_time: maximum time across all stages (for bar scaling)
 */
static void draw_stage_row(uint8_t *buf, int col_x, int y,
                           const char *name, bool active, bool bypassed,
                           float time_us, bool selected, float max_time) {
    /* Selection highlight: draw a 1px-high bar above the text. */
    if (selected) {
        vis_hline(buf, col_x, y, 122, PAL_HIGHLIGHT);
    }

    int text_y = y + 1;
    int x = col_x;

    /* Bypass indicator: [B] or [ ] */
    uint8_t text_pal;
    if (!active) {
        text_pal = PAL_DIM;
    } else if (bypassed) {
        text_pal = PAL_BYPASS;
    } else {
        text_pal = PAL_TEXT;
    }

    if (bypassed) {
        x = vis_text(buf, x, text_y, "[B]", PAL_BYPASS);
    } else {
        x = vis_text(buf, x, text_y, "[ ]", active ? PAL_DIM : PAL_DIM);
    }
    x += 2; /* small gap */

    /* Stage name (with strikethrough if bypassed). */
    if (bypassed && active) {
        x = vis_text_strike(buf, x, text_y, name, PAL_BYPASS);
    } else {
        x = vis_text(buf, x, text_y, name, text_pal);
    }

    /* Timing value (right-aligned in the column). */
    if (time_us > 0.0f) {
        char tbuf[16];
        fmt_ms(tbuf, sizeof(tbuf), time_us);
        /* Position timing at col_x + 88 */
        int tx = col_x + 88;
        vis_text(buf, tx, text_y, tbuf, text_pal);
    }

    /* Timing bar at the end of the row. */
    int bar_x = col_x + 108;
    int bar_w = 14;
    float frac = (max_time > 0.0f) ? (time_us / max_time) : 0.0f;
    if (active && !bypassed) {
        vis_bar(buf, bar_x, text_y, bar_w, frac, PAL_BAR_FILL, PAL_BAR_EMPTY);
    } else {
        vis_bar(buf, bar_x, text_y, bar_w, 0.0f, PAL_BAR_EMPTY, PAL_BAR_EMPTY);
    }
}

void chain_vis_update(ChainVis *vis, int current_preset) {
    if (!vis || !vis->visible) return;

    /* Clear buffer. */
    memset(vis->buf, PAL_BG, sizeof(vis->buf));

    /* ---- Header: preset name (via the scanned preset registry) ---- */
    const char *pname = preset_display_name(current_preset);
    if (pname && *pname) {
        vis_text(vis->buf, 2, HEADER_Y, "CHAIN: ", PAL_DIM);
        vis_text(vis->buf, 37, HEADER_Y, pname, PAL_HEADER);
    } else {
        vis_text(vis->buf, 2, HEADER_Y, "CHAIN VISUALISER", PAL_HEADER);
    }

    /* ---- Column headers ---- */
    vis_text(vis->buf, VIDEO_COL_X, COLUMNS_Y, "VIDEO", PAL_HEADER);
    vis_text(vis->buf, AUDIO_COL_X, COLUMNS_Y, "AUDIO", PAL_HEADER);

    /* Separator line below column headers. */
    vis_hline(vis->buf, VIDEO_COL_X, COLUMNS_Y + 8, 122, PAL_SEPARATOR);
    vis_hline(vis->buf, AUDIO_COL_X, COLUMNS_Y + 8, 122, PAL_SEPARATOR);

    /* ---- Find max timing for bar scaling ---- */
    float max_time = 1.0f; /* minimum 1 us to avoid division by zero */
    for (int i = 0; i < VIDEO_STAGE_COUNT; i++)
        if (vis->video_time_us[i] > max_time) max_time = vis->video_time_us[i];
    for (int i = 0; i < AUDIO_STAGE_COUNT; i++)
        if (vis->audio_time_us[i] > max_time) max_time = vis->audio_time_us[i];

    /* ---- Video stages ---- */
    float video_total_us = 0.0f;
    for (int i = 0; i < VIDEO_STAGE_COUNT; i++) {
        int y = STAGE_START_Y + i * STAGE_ROW_H;
        /* video_chain_stage_active uses 1-based indices. */
        bool active = video_chain_stage_active(vis->vc, i + 1);
        bool selected = (vis->selected_col == COL_VIDEO &&
                         vis->selected_row == i);
        draw_stage_row(vis->buf, VIDEO_COL_X, y,
                       video_stage_names[i], active,
                       vis->video_bypass[i],
                       vis->video_time_us[i], selected, max_time);
        if (active && !vis->video_bypass[i])
            video_total_us += vis->video_time_us[i];
    }

    /* ---- Audio stages ---- */
    float audio_total_us = 0.0f;
    for (int i = 0; i < AUDIO_STAGE_COUNT; i++) {
        int y = STAGE_START_Y + i * STAGE_ROW_H;
        bool active = audio_stage_enabled(vis->ac, i);
        bool selected = (vis->selected_col == COL_AUDIO &&
                         vis->selected_row == i);
        draw_stage_row(vis->buf, AUDIO_COL_X, y,
                       audio_stage_names[i], active,
                       vis->audio_bypass[i],
                       vis->audio_time_us[i], selected, max_time);
        if (active && !vis->audio_bypass[i])
            audio_total_us += vis->audio_time_us[i];
    }

    /* ---- Footer: total timing ---- */
    {
        char tbuf[32];
        fmt_ms(tbuf, sizeof(tbuf), video_total_us);
        int fx = vis_text(vis->buf, VIDEO_COL_X, FOOTER_Y, "Total: ", PAL_DIM);
        fx = vis_text(vis->buf, fx, FOOTER_Y, tbuf, PAL_TEXT);
        vis_text(vis->buf, fx, FOOTER_Y, "ms", PAL_DIM);
    }
    {
        char tbuf[32];
        fmt_ms(tbuf, sizeof(tbuf), audio_total_us);
        int fx = vis_text(vis->buf, AUDIO_COL_X, FOOTER_Y, "Total: ", PAL_DIM);
        fx = vis_text(vis->buf, fx, FOOTER_Y, tbuf, PAL_TEXT);
        vis_text(vis->buf, fx, FOOTER_Y, "ms", PAL_DIM);
    }

    /* ---- Help line at very bottom ---- */
    vis_text(vis->buf, 2, VIS_H - 4, "Arrows:nav  B:bypass  F8:close",
             PAL_DIM);
}

const uint8_t *chain_vis_get_overlay(const ChainVis *vis, int *w, int *h) {
    if (!vis || !vis->visible) {
        if (w) *w = 0;
        if (h) *h = 0;
        return NULL;
    }
    if (w) *w = VIS_W;
    if (h) *h = VIS_H;
    return vis->buf;
}

/* ---- Input handling ----------------------------------------------------- */

bool chain_vis_handle_key(ChainVis *vis, int scancode, bool down) {
    if (!vis || !vis->visible) return false;
    if (!down) return false;  /* Only act on key-down. */

    int max_row = (vis->selected_col == COL_VIDEO)
                  ? VIDEO_STAGE_COUNT : AUDIO_STAGE_COUNT;

    switch (scancode) {
    case SDL_SCANCODE_L:
        vis->visible = false;
        return true;

    case SDL_SCANCODE_UP:
        vis->selected_row--;
        if (vis->selected_row < 0) vis->selected_row = max_row - 1;
        return true;

    case SDL_SCANCODE_DOWN:
        vis->selected_row++;
        if (vis->selected_row >= max_row) vis->selected_row = 0;
        return true;

    case SDL_SCANCODE_LEFT:
        if (vis->selected_col == COL_AUDIO) {
            vis->selected_col = COL_VIDEO;
            if (vis->selected_row >= VIDEO_STAGE_COUNT)
                vis->selected_row = VIDEO_STAGE_COUNT - 1;
        }
        return true;

    case SDL_SCANCODE_RIGHT:
        if (vis->selected_col == COL_VIDEO) {
            vis->selected_col = COL_AUDIO;
            if (vis->selected_row >= AUDIO_STAGE_COUNT)
                vis->selected_row = AUDIO_STAGE_COUNT - 1;
        }
        return true;

    case SDL_SCANCODE_B:
        if (vis->selected_col == COL_VIDEO) {
            vis->video_bypass[vis->selected_row] =
                !vis->video_bypass[vis->selected_row];
        } else {
            vis->audio_bypass[vis->selected_row] =
                !vis->audio_bypass[vis->selected_row];
        }
        return true;

    default:
        return false;
    }
}
