/*
 * OSD Menu System — Shared Header-Only Module
 * =============================================
 *
 * Reusable OSD engine for NES frontends: bitmap font, NES-framebuffer
 * drawing primitives, and a hierarchical menu state machine. The menu
 * renders INTO the PPU framebuffer BEFORE comp_process() runs, so text
 * goes through the full composite pipeline and looks like a real CRT
 * on-screen display.
 *
 * All functions are static inline so this works as a header-only module
 * (same pattern as composite.h). Each frontend includes this header and
 * defines its own menu item tables.
 *
 * Usage:
 *   #include "nes/osd.h"
 *   // Define MenuItem arrays and a root menu
 *   // Call osd_menu_open_root(items, count, "TITLE") to open
 *   // Call osd_menu_render_nes(rgb_fb, idx_fb, pal) before comp_process
 */

#ifndef OSD_H
#define OSD_H

#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <stdio.h>

/* ============================================================================
 * 5x7 Bitmap Font
 * ============================================================================
 * Uppercase A-Z, digits 0-9, common punctuation. Lowercase folds to
 * uppercase. Each glyph is 7 bytes; bit 4 is the leftmost column. */

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

/* ============================================================================
 * NES Framebuffer Drawing Target
 * ============================================================================
 * Writes to both the RGB framebuffer (for the legacy path + non-composite
 * mode) and the index framebuffer (for the waveform path). Uses the
 * currently-selected palette for RGB lookup. */

typedef struct {
    uint8_t        *rgb;          /* 256*240*3 */
    uint16_t       *idx;          /* 256*240 */
    const uint8_t (*pal)[3];
} OSDNesFB;

static inline void osd_nesfb_set(OSDNesFB *t, int x, int y, uint8_t pi) {
    if ((unsigned)x >= 256 || (unsigned)y >= 240) return;
    int p = y * 256 + x;
    pi &= 0x3F;
    t->idx[p] = pi;
    t->rgb[p*3 + 0] = t->pal[pi][0];
    t->rgb[p*3 + 1] = t->pal[pi][1];
    t->rgb[p*3 + 2] = t->pal[pi][2];
}

static inline void osd_nesfb_fill(OSDNesFB *t, int x, int y, int w, int h,
                                   uint8_t pi) {
    int x0 = x < 0 ? 0 : x;
    int y0 = y < 0 ? 0 : y;
    int x1 = x + w; if (x1 > 256) x1 = 256;
    int y1 = y + h; if (y1 > 240) y1 = 240;
    for (int yy = y0; yy < y1; yy++)
        for (int xx = x0; xx < x1; xx++)
            osd_nesfb_set(t, xx, yy, pi);
}

/* Dim an NES palette index by luminance tiers. The NES 64-color palette
 * has 4 luminance tiers at 0x00/0x10/0x20/0x30; each step down roughly
 * halves brightness. Cheap palette-native "transparency". */
static inline uint8_t osd_nesfb_dim_index(uint8_t idx, int tiers) {
    idx &= 0x3F;
    int col = idx & 0x0F;
    int lum = (idx >> 4) & 0x03;
    lum -= tiers;
    if (lum < 0) return 0x0F;
    return (uint8_t)((lum << 4) | col);
}

static inline void osd_nesfb_dim_rect(OSDNesFB *t, int x, int y, int w,
                                       int h, int tiers) {
    int x0 = x < 0 ? 0 : x;
    int y0 = y < 0 ? 0 : y;
    int x1 = x + w; if (x1 > 256) x1 = 256;
    int y1 = y + h; if (y1 > 240) y1 = 240;
    for (int yy = y0; yy < y1; yy++) {
        for (int xx = x0; xx < x1; xx++) {
            int p = yy * 256 + xx;
            uint8_t orig = (uint8_t)(t->idx[p] & 0x3F);
            osd_nesfb_set(t, xx, yy, osd_nesfb_dim_index(orig, tiers));
        }
    }
}

/* Draw a single character using the 5x7 font, scaled by `scale`. */
static inline void osd_nesfb_char(OSDNesFB *t, int x, int y, char c,
                                   uint8_t pi, int scale) {
    if ((unsigned char)c >= 128) return;
    if (c >= 'a' && c <= 'z') c -= 32;
    const uint8_t *glyph = osd_font5x7[(int)(unsigned char)c];
    for (int row = 0; row < 7; row++) {
        uint8_t bits = glyph[row];
        for (int col = 0; col < 5; col++) {
            if (bits & (0x10 >> col)) {
                osd_nesfb_fill(t, x + col*scale, y + row*scale,
                               scale, scale, pi);
            }
        }
    }
}

static inline void osd_nesfb_text(OSDNesFB *t, int x, int y, const char *s,
                                   uint8_t pi, int scale) {
    int cx = x;
    for (const char *p = s; *p; p++) {
        if (*p == '\n') { cx = x; y += (7 + 1) * scale; continue; }
        osd_nesfb_char(t, cx, y, *p, pi, scale);
        cx += (5 + 1) * scale;
    }
}

static inline int osd_nesfb_text_width(const char *s, int scale) {
    int len = 0;
    for (const char *p = s; *p; p++) len++;
    return len * (5 + 1) * scale;
}

/* Drop-shadow text: draw in shadow color offset by (1,1), then in fg. */
static inline void osd_nesfb_text_sh(OSDNesFB *t, int x, int y,
                                      const char *s, uint8_t fg,
                                      uint8_t sh, int scale) {
    osd_nesfb_text(t, x + scale, y + scale, s, sh, scale);
    osd_nesfb_text(t, x, y, s, fg, scale);
}

/* ============================================================================
 * Menu Item / Menu Level Types
 * ============================================================================ */

typedef enum {
    OSD_MI_FLOAT,       /* adjustable float (clamped to [min, max]) */
    OSD_MI_INT,         /* adjustable int (clamped to [min, max]) */
    OSD_MI_INT_CYCLIC,  /* adjustable int (wraps at boundaries) */
    OSD_MI_TOGGLE,      /* int used as bool; Left/Right toggles */
    OSD_MI_SUBMENU,     /* Enter descends into `submenu` */
    OSD_MI_ACTION,      /* Enter calls `action()` */
} OSDMenuItemType;

typedef struct OSDMenuItem {
    const char          *label;
    OSDMenuItemType      type;
    void                *target;
    float                step;
    float                min_val;
    float                max_val;
    void               (*on_change)(void);
    const struct OSDMenuItem *submenu;
    int                  submenu_count;
    void               (*action)(void);
    const char          *format;    /* printf format for value display */
} OSDMenuItem;

#define OSD_MENU_STACK_MAX 4

typedef struct {
    const OSDMenuItem  *items;
    int                 count;
    int                 selected;
    const char         *title;
} OSDMenuLevel;

/* ============================================================================
 * Menu State
 * ============================================================================
 * Single global menu state. Frontends share one menu at a time. */

static OSDMenuLevel osd_menu_stack[OSD_MENU_STACK_MAX];
static int          osd_menu_depth = 0;
static bool         osd_menu_is_open = false;
static int          osd_menu_bg_solid = 1;

/* ============================================================================
 * Menu Navigation
 * ============================================================================ */

static inline void osd_menu_push(const OSDMenuItem *items, int count,
                                  const char *title) {
    if (osd_menu_depth >= OSD_MENU_STACK_MAX) return;
    osd_menu_stack[osd_menu_depth].items    = items;
    osd_menu_stack[osd_menu_depth].count    = count;
    osd_menu_stack[osd_menu_depth].selected = 0;
    osd_menu_stack[osd_menu_depth].title    = title;
    osd_menu_depth++;
}

static inline void osd_menu_open_root(const OSDMenuItem *root_items,
                                       int root_count, const char *title) {
    osd_menu_depth = 0;
    osd_menu_push(root_items, root_count, title);
    osd_menu_is_open = true;
}

static inline void osd_menu_close(void) {
    osd_menu_is_open = false;
    osd_menu_depth = 0;
}

static inline void osd_menu_back(void) {
    if (osd_menu_depth > 1) osd_menu_depth--;
    else osd_menu_close();
}

static inline OSDMenuLevel *osd_menu_current(void) {
    return osd_menu_depth > 0 ? &osd_menu_stack[osd_menu_depth - 1] : NULL;
}

static inline const OSDMenuItem *osd_menu_current_item(void) {
    OSDMenuLevel *lvl = osd_menu_current();
    if (!lvl) return NULL;
    if (lvl->selected < 0 || lvl->selected >= lvl->count) return NULL;
    return &lvl->items[lvl->selected];
}

static inline void osd_menu_move(int delta) {
    OSDMenuLevel *lvl = osd_menu_current();
    if (!lvl || lvl->count == 0) return;
    lvl->selected = (lvl->selected + delta + lvl->count) % lvl->count;
}

static inline void osd_menu_adjust(int dir) {
    const OSDMenuItem *it = osd_menu_current_item();
    if (!it || !it->target) return;
    switch (it->type) {
    case OSD_MI_FLOAT: {
        float *p = (float *)it->target;
        *p += (float)dir * it->step;
        if (*p < it->min_val) *p = it->min_val;
        if (*p > it->max_val) *p = it->max_val;
        break;
    }
    case OSD_MI_INT: {
        int *p = (int *)it->target;
        int s = (int)it->step; if (s < 1) s = 1;
        *p += dir * s;
        int lo = (int)it->min_val, hi = (int)it->max_val;
        if (*p < lo) *p = lo;
        if (*p > hi) *p = hi;
        break;
    }
    case OSD_MI_INT_CYCLIC: {
        int *p = (int *)it->target;
        int s = (int)it->step; if (s < 1) s = 1;
        int lo = (int)it->min_val, hi = (int)it->max_val;
        int range = hi - lo + 1;
        *p += dir * s;
        while (*p < lo) *p += range;
        while (*p > hi) *p -= range;
        break;
    }
    case OSD_MI_TOGGLE: {
        int *p = (int *)it->target;
        *p = !*p;
        break;
    }
    default: return;
    }
    if (it->on_change) it->on_change();
}

static inline void osd_menu_activate(void) {
    const OSDMenuItem *it = osd_menu_current_item();
    if (!it) return;
    switch (it->type) {
    case OSD_MI_SUBMENU:
        if (it->submenu && it->submenu_count > 0)
            osd_menu_push(it->submenu, it->submenu_count, it->label);
        break;
    case OSD_MI_ACTION:
        if (it->action) it->action();
        break;
    case OSD_MI_TOGGLE:
        osd_menu_adjust(1);
        break;
    default:
        break;
    }
}

/* ============================================================================
 * Value Formatting
 * ============================================================================ */

static inline void osd_menu_format_value(const OSDMenuItem *it, char *buf,
                                          size_t cap) {
    if (!it || !buf || cap == 0) return;
    if (!it->target) { buf[0] = '\0'; return; }
    switch (it->type) {
    case OSD_MI_FLOAT:
        snprintf(buf, cap, it->format ? it->format : "%.2f",
                 *(float *)it->target);
        break;
    case OSD_MI_INT:
    case OSD_MI_INT_CYCLIC: {
        int iv = *(int *)it->target;
        /* If format contains '|', treat as label map: "OFF|RGB|BGR" */
        if (it->format && strchr(it->format, '|')) {
            const char *p = it->format;
            int idx = 0;
            while (idx < iv && *p) {
                if (*p == '|') idx++;
                p++;
            }
            const char *end = strchr(p, '|');
            int len = end ? (int)(end - p) : (int)strlen(p);
            if (len >= (int)cap) len = (int)cap - 1;
            memcpy(buf, p, (size_t)len);
            buf[len] = '\0';
        } else {
            snprintf(buf, cap, it->format ? it->format : "%d", iv);
        }
        break;
    }
    case OSD_MI_TOGGLE: {
        int v = *(int *)it->target;
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

/* ============================================================================
 * Menu Rendering — NES Framebuffer
 * ============================================================================
 * Draws the menu INTO the PPU framebuffer before comp_process runs.
 * Sony PVM broadcast monitor aesthetic: dark navy panel, cyan border,
 * centered uppercase title, clean white/cyan/grey palette. */

static inline void osd_menu_render_nes(uint8_t *rgb_fb, uint16_t *idx_fb,
                                        const uint8_t (*pal)[3]) {
    if (!osd_menu_is_open) return;

    OSDNesFB t;
    t.rgb = rgb_fb;
    t.idx = idx_fb;
    t.pal = pal;

    OSDMenuLevel *lvl = osd_menu_current();
    if (!lvl) return;

    /* PVM palette */
    const uint8_t COL_BG        = 0x02;  /* dark navy blue */
    const uint8_t COL_BORDER    = 0x2C;  /* cyan PVM accent */
    const uint8_t COL_SEP       = 0x10;  /* grey separator */
    const uint8_t COL_TITLE     = 0x30;  /* bright white title */
    const uint8_t COL_LABEL     = 0x30;  /* white label (selected) */
    const uint8_t COL_LABEL_DIM = 0x10;  /* grey label (unselected) */
    const uint8_t COL_VALUE     = 0x2C;  /* cyan value (selected) */
    const uint8_t COL_VALUE_DIM = 0x10;  /* grey value (unselected) */
    const uint8_t COL_CURSOR    = 0x2C;  /* cyan cursor */
    const uint8_t COL_HINT      = 0x00;  /* dim grey hint */

    /* Panel geometry */
    const int PX = 14, PY = 14, PW = 166, PH = 136;

    /* Background */
    if (osd_menu_bg_solid) {
        osd_nesfb_fill(&t, PX, PY, PW, PH, COL_BG);
    }

    /* Single-pixel cyan border */
    osd_nesfb_fill(&t, PX,          PY,          PW, 1, COL_BORDER);
    osd_nesfb_fill(&t, PX,          PY + PH - 1, PW, 1, COL_BORDER);
    osd_nesfb_fill(&t, PX,          PY,          1,  PH, COL_BORDER);
    osd_nesfb_fill(&t, PX + PW - 1, PY,          1,  PH, COL_BORDER);

    /* Centered title */
    const char *title = (osd_menu_depth > 0 &&
                         osd_menu_stack[osd_menu_depth - 1].title)
                         ? osd_menu_stack[osd_menu_depth - 1].title : "MENU";
    int title_w = osd_nesfb_text_width(title, 1);
    osd_nesfb_text(&t, PX + (PW - title_w) / 2, PY + 4, title,
                   COL_TITLE, 1);

    /* Separator under title */
    osd_nesfb_fill(&t, PX + 3, PY + 13, PW - 6, 1, COL_SEP);

    /* Items area */
    int item_y0 = PY + 17;
    int row_h   = 9;
    int label_x = PX + 11;
    int value_col_right = PX + PW - 5;

    int avail = (PY + PH - 12) - item_y0;
    int max_rows = avail / row_h;
    if (max_rows < 1) max_rows = 1;

    /* Scroll so the selected item is always visible */
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
        const OSDMenuItem *it = &lvl->items[i];

        /* Selection cursor */
        if (sel) {
            osd_nesfb_text(&t, PX + 4, y, ">", COL_CURSOR, 1);
        }

        uint8_t label_col = sel ? COL_LABEL : COL_LABEL_DIM;
        osd_nesfb_text(&t, label_x, y, it->label, label_col, 1);

        /* Value column, right-aligned */
        char vbuf[48];
        if (it->type == OSD_MI_SUBMENU) {
            snprintf(vbuf, sizeof(vbuf), ">");
        } else if (it->type == OSD_MI_ACTION) {
            vbuf[0] = '\0';
        } else {
            osd_menu_format_value(it, vbuf, sizeof(vbuf));
        }

        int vw = osd_nesfb_text_width(vbuf, 1);
        osd_nesfb_text(&t, value_col_right - vw, y, vbuf,
                       sel ? COL_VALUE : COL_VALUE_DIM, 1);
    }

    /* Separator above footer */
    osd_nesfb_fill(&t, PX + 3, PY + PH - 12, PW - 6, 1, COL_SEP);

    /* Footer hint line */
    const char *hint = "M EXIT   ESC BACK";
    int hint_w = osd_nesfb_text_width(hint, 1);
    osd_nesfb_text(&t, PX + (PW - hint_w) / 2, PY + PH - 9, hint,
                   COL_HINT, 1);
}

/* ============================================================================
 * Convenience Macro
 * ============================================================================ */

#define OSD_MENU_COUNT(tbl) (int)(sizeof(tbl) / sizeof((tbl)[0]))

#endif /* OSD_H */
