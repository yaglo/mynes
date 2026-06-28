/*
 * browser.c — Fullscreen ROM browser. See browser.h for the contract.
 *
 * Reuses osd_nesfb_* primitives from src/nes/osd.h so the styling
 * matches the in-game setup menu.
 */
#include "browser.h"

#include <ctype.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "nes/osd.h"

/* Tunables */
#define RECENT_DISPLAY_MAX  6     /* show at most N recent ROMs */
#define LIST_ROW_HEIGHT     9     /* same as OSD setup menu */
#define VISIBLE_ROWS        20    /* approx; recomputed at render */

/* PVM-style palette — identical to the OSD setup menu's, so the two
 * screens visually belong to the same UI family. */
#define COL_BG        0x02   /* dark navy blue background */
#define COL_BORDER    0x2C   /* cyan PVM accent — single-pixel frame */
#define COL_SEP       0x10   /* grey horizontal separators */
#define COL_TITLE     0x30   /* bright white title */
#define COL_HEADER    0x2C   /* cyan section headers ("RECENT", "FILES") */
#define COL_PATH      0x10   /* dim grey breadcrumb */
#define COL_LABEL     0x30   /* white label text (selected row) */
#define COL_LABEL_DIM 0x10   /* grey label text (unselected) */
#define COL_DIR_DIM   0x2C   /* cyan dir name (unselected, like OSD value) */
#define COL_CURSOR    0x2C   /* cyan ">" cursor */
#define COL_HINT      0x00   /* dim grey footer */

/* ---------------------------------------------------------------------------
 * Filesystem listing
 * ------------------------------------------------------------------------- */

static bool has_extension(const char *name, const char *ext) {
    size_t nl = strlen(name), el = strlen(ext);
    if (nl <= el) return false;
    const char *suffix = name + nl - el;
    for (size_t i = 0; i < el; i++) {
        if (tolower((unsigned char)suffix[i]) != tolower((unsigned char)ext[i]))
            return false;
    }
    return true;
}

static bool is_rom(const char *name) {
    return has_extension(name, ".nes") || has_extension(name, ".fds");
}

static void load_directory(Browser *b) {
    b->entry_count = 0;

    DIR *d = opendir(b->current_dir);
    if (!d) return;

    /* Always include ".." (parent) so the user can navigate up. */
    strncpy(b->entries[b->entry_count], "..", sizeof(b->entries[0]) - 1);
    b->entry_is_dir[b->entry_count] = true;
    b->entry_count++;

    /* Stash (name, is_dir) pairs so we can sort them. */
    struct { char *name; int is_dir; } pairs[BROWSER_MAX_ENTRIES];
    int n = 0;

    struct dirent *ent;
    while ((ent = readdir(d)) != NULL && n < BROWSER_MAX_ENTRIES) {
        const char *name = ent->d_name;
        if (name[0] == '.') continue;  /* skip hidden + ./.. */

        char full[MYNES_PATH_MAX];
        snprintf(full, sizeof(full), "%s/%s", b->current_dir, name);
        struct stat st;
        if (stat(full, &st) != 0) continue;

        bool is_dir = S_ISDIR(st.st_mode);
        if (!is_dir && !is_rom(name)) continue;  /* hide non-ROM files */

        char *copy = (char *)malloc(strlen(name) + 1);
        if (!copy) continue;
        strcpy(copy, name);
        pairs[n].name = copy;
        pairs[n].is_dir = (int)is_dir;
        n++;
    }
    closedir(d);

    /* Insertion sort — n is small (capped at a few hundred), avoids the
     * qsort comparator complexity above. */
    for (int i = 1; i < n; i++) {
        for (int j = i; j > 0; j--) {
            int swap = 0;
            if (pairs[j].is_dir != pairs[j - 1].is_dir) {
                if (pairs[j].is_dir && !pairs[j - 1].is_dir) swap = 1;
            } else {
                if (strcasecmp(pairs[j].name, pairs[j - 1].name) < 0) swap = 1;
            }
            if (!swap) break;
            char *tn = pairs[j].name; int td = pairs[j].is_dir;
            pairs[j].name = pairs[j - 1].name; pairs[j].is_dir = pairs[j - 1].is_dir;
            pairs[j - 1].name = tn; pairs[j - 1].is_dir = td;
        }
    }

    for (int i = 0; i < n && b->entry_count < BROWSER_MAX_ENTRIES; i++) {
        strncpy(b->entries[b->entry_count], pairs[i].name,
                sizeof(b->entries[0]) - 1);
        b->entries[b->entry_count][sizeof(b->entries[0]) - 1] = '\0';
        b->entry_is_dir[b->entry_count] = pairs[i].is_dir;
        b->entry_count++;
        free(pairs[i].name);
    }

    /* Reset selection/scroll when contents change. */
    b->scroll_offset = 0;
    if (b->selected >= b->entry_count + b->recent_visible_count)
        b->selected = b->recent_visible_count;
}

/* ---------------------------------------------------------------------------
 * API
 * ------------------------------------------------------------------------- */

bool browser_init(Browser *b, const char *start_dir, const MynesConfig *config) {
    if (!b) return false;
    memset(b, 0, sizeof(*b));
    b->config = config;

    if (start_dir && *start_dir) {
        strncpy(b->current_dir, start_dir, sizeof(b->current_dir) - 1);
    } else if (!getcwd(b->current_dir, sizeof(b->current_dir))) {
        strncpy(b->current_dir, ".", sizeof(b->current_dir) - 1);
    }

    /* Compute how many recent items will be shown. */
    int rc = config ? config->recent_count : 0;
    if (rc > RECENT_DISPLAY_MAX) rc = RECENT_DISPLAY_MAX;
    b->recent_visible_count = rc;

    load_directory(b);

    /* Default selection: first recent if any, otherwise first dir entry. */
    b->selected = 0;
    return true;
}

/* Navigate into a directory or up one. Reloads the entry list. */
static void cd_to(Browser *b, const char *path) {
    char abs[MYNES_PATH_MAX];

    if (strcmp(path, "..") == 0) {
        /* Strip trailing component. */
        char tmp[MYNES_PATH_MAX];
        snprintf(tmp, sizeof(tmp), "%s", b->current_dir);
        char *slash = strrchr(tmp, '/');
        if (slash && slash != tmp) *slash = '\0';
        else if (slash == tmp) tmp[1] = '\0';   /* root */
        snprintf(abs, sizeof(abs), "%s", tmp);
    } else if (path[0] == '/') {
        snprintf(abs, sizeof(abs), "%s", path);
    } else {
        snprintf(abs, sizeof(abs), "%s/%s", b->current_dir, path);
    }

    /* Validate it's a directory. */
    struct stat st;
    if (stat(abs, &st) != 0 || !S_ISDIR(st.st_mode)) return;

    snprintf(b->current_dir, sizeof(b->current_dir), "%s", abs);
    load_directory(b);
    /* Place selection on the first entry below "..". */
    b->selected = b->recent_visible_count + (b->entry_count > 1 ? 1 : 0);
}

BrowserResult browser_handle_key(Browser *b, BrowserKey key) {
    if (!b) return BROWSER_BROWSING;

    int total = b->recent_visible_count + b->entry_count;
    if (total <= 0) return BROWSER_BROWSING;

    switch (key) {
    case BROWSER_KEY_UP:
        if (b->selected > 0) b->selected--;
        break;
    case BROWSER_KEY_DOWN:
        if (b->selected + 1 < total) b->selected++;
        break;
    case BROWSER_KEY_LEFT:
    case BROWSER_KEY_PAGEUP:
        b->selected -= VISIBLE_ROWS;
        if (b->selected < 0) b->selected = 0;
        break;
    case BROWSER_KEY_RIGHT:
    case BROWSER_KEY_PAGEDOWN:
        b->selected += VISIBLE_ROWS;
        if (b->selected >= total) b->selected = total - 1;
        break;
    case BROWSER_KEY_BACK:
        cd_to(b, "..");
        break;
    case BROWSER_KEY_ESCAPE:
        return BROWSER_CANCELLED;
    case BROWSER_KEY_ENTER: {
        if (b->selected < b->recent_visible_count) {
            /* Recent ROM. */
            const char *p = b->config->recent_roms[b->selected];
            snprintf(b->chosen_path, sizeof(b->chosen_path), "%s", p);
            return BROWSER_SELECTED;
        }
        int idx = b->selected - b->recent_visible_count;
        if (idx < 0 || idx >= b->entry_count) break;
        if (b->entry_is_dir[idx]) {
            cd_to(b, b->entries[idx]);
        } else {
            snprintf(b->chosen_path, sizeof(b->chosen_path), "%s/%s",
                     b->current_dir, b->entries[idx]);
            return BROWSER_SELECTED;
        }
        break;
    }
    }
    return BROWSER_BROWSING;
}

/* ---------------------------------------------------------------------------
 * Rendering
 * ------------------------------------------------------------------------- */

/* Truncate a string to at most `max_chars`, prefixing with "…" if cut. */
static void truncate_path(char *out, int out_sz, const char *in, int max_chars) {
    int len = (int)strlen(in);
    if (len <= max_chars) {
        snprintf(out, out_sz, "%s", in);
        return;
    }
    int keep = max_chars - 1;  /* room for the leading "…" (3 bytes UTF-8) */
    if (keep < 1) keep = 1;
    snprintf(out, out_sz, "...%s", in + (len - keep));
}

void browser_render(const Browser *b,
                    uint8_t *framebuffer, uint16_t *index_framebuffer,
                    const uint8_t (*palette)[3]) {
    if (!b || !framebuffer || !index_framebuffer || !palette) return;

    OSDNesFB t;
    t.rgb = framebuffer;
    t.idx = index_framebuffer;
    t.pal = palette;

    /* Fullscreen panel that mimics the OSD setup menu's frame: navy fill +
     * single-pixel cyan border + centered title with separator, item rows
     * in the middle, footer hint with separator above. */
    const int PX = 0, PY = 0, PW = 256, PH = 240;

    osd_nesfb_fill(&t, PX, PY, PW, PH, COL_BG);

    osd_nesfb_fill(&t, PX,          PY,          PW, 1, COL_BORDER);
    osd_nesfb_fill(&t, PX,          PY + PH - 1, PW, 1, COL_BORDER);
    osd_nesfb_fill(&t, PX,          PY,          1,  PH, COL_BORDER);
    osd_nesfb_fill(&t, PX + PW - 1, PY,          1,  PH, COL_BORDER);

    /* Title (centered) + separator below. */
    const char *title = "SELECT ROM";
    int title_w = osd_nesfb_text_width(title, 1);
    osd_nesfb_text(&t, PX + (PW - title_w) / 2, PY + 4, title, COL_TITLE, 1);
    osd_nesfb_fill(&t, PX + 3, PY + 13, PW - 6, 1, COL_SEP);

    /* Breadcrumb under title. */
    {
        char trim[60];
        truncate_path(trim, sizeof(trim), b->current_dir, 41);
        osd_nesfb_text(&t, PX + 4, PY + 17, trim, COL_PATH, 1);
    }
    osd_nesfb_fill(&t, PX + 3, PY + 26, PW - 6, 1, COL_SEP);

    /* Items area: starts BELOW the breadcrumb separator (PY+26), ends
     * ABOVE the footer separator (PY+PH-12). Anything we draw outside
     * this band would collide with the breadcrumb or footer. */
    const int label_x       = PX + 11;
    const int items_top     = PY + 30;
    const int footer_sep_y  = PY + PH - 12;

    /* Optional "RECENT" / "FILES" headers. Each takes one row when shown.
     * We position them as anchors at the top of their respective sections
     * so they never overlap the breadcrumb area above. */
    const bool have_recent = (b->recent_visible_count > 0);
    const bool have_files  = (b->entry_count > 0);

    int y = items_top;

    if (have_recent) {
        osd_nesfb_text(&t, label_x, y, "RECENT", COL_HEADER, 1);
        y += LIST_ROW_HEIGHT;
    }

    /* Render up to recent_visible_count recent rows (no scroll inside
     * the recent section — the list is capped). */
    for (int i = 0; i < b->recent_visible_count
                    && y + LIST_ROW_HEIGHT <= footer_sep_y; i++) {
        const char *path = b->config->recent_roms[i];
        const char *base = strrchr(path, '/');
        const char *name = base ? base + 1 : path;
        char display[60];
        truncate_path(display, sizeof(display), name, 38);

        bool sel = (i == b->selected);
        if (sel) osd_nesfb_text(&t, PX + 4, y, ">", COL_CURSOR, 1);
        osd_nesfb_text(&t, label_x, y, display,
                       sel ? COL_LABEL : COL_LABEL_DIM, 1);
        y += LIST_ROW_HEIGHT;
    }

    if (have_files) {
        if (y + LIST_ROW_HEIGHT * 2 > footer_sep_y) {
            /* No vertical room for both header and at least one file row.
             * Skip the "FILES" header rather than overlap the footer. */
        } else if (have_recent) {
            /* Add a thin separator + header between RECENT and FILES. */
            osd_nesfb_fill(&t, PX + 11, y + 2, PW - 22, 1, COL_SEP);
            y += 5;
            osd_nesfb_text(&t, label_x, y, "FILES", COL_HEADER, 1);
            y += LIST_ROW_HEIGHT;
        } else {
            /* No recents, but still label the section so it's clear. */
            osd_nesfb_text(&t, label_x, y, "FILES", COL_HEADER, 1);
            y += LIST_ROW_HEIGHT;
        }
    }

    /* File-list scroll window: rows we have left until the footer line. */
    int avail_rows = (footer_sep_y - y) / LIST_ROW_HEIGHT;
    if (avail_rows < 1) avail_rows = 1;

    /* Scroll so the selected file row stays in view. selected indexes the
     * unified list (recents first, then files). */
    int file_sel = b->selected - b->recent_visible_count;
    Browser *mut = (Browser *)b;
    int scroll = mut->scroll_offset;
    if (file_sel >= 0) {
        if (file_sel < scroll) scroll = file_sel;
        if (file_sel >= scroll + avail_rows) scroll = file_sel - avail_rows + 1;
    }
    if (scroll < 0) scroll = 0;
    if (b->entry_count > avail_rows && scroll > b->entry_count - avail_rows)
        scroll = b->entry_count - avail_rows;
    mut->scroll_offset = scroll;

    int last = scroll + avail_rows;
    if (last > b->entry_count) last = b->entry_count;

    for (int i = scroll; i < last; i++) {
        bool is_dir = b->entry_is_dir[i];
        char display[60];
        if (is_dir) {
            char trim[42];
            truncate_path(trim, sizeof(trim), b->entries[i], 36);
            snprintf(display, sizeof(display), "%s/", trim);
        } else {
            truncate_path(display, sizeof(display), b->entries[i], 38);
        }
        bool sel = (i == file_sel);
        if (sel) osd_nesfb_text(&t, PX + 4, y, ">", COL_CURSOR, 1);
        uint8_t fg = sel ? COL_LABEL : (is_dir ? COL_DIR_DIM : COL_LABEL_DIM);
        osd_nesfb_text(&t, label_x, y, display, fg, 1);
        y += LIST_ROW_HEIGHT;
    }

    /* Scroll indicators in the right-side gutter. */
    if (b->entry_count > avail_rows) {
        int top_row_y = footer_sep_y - avail_rows * LIST_ROW_HEIGHT;
        if (scroll > 0)
            osd_nesfb_text(&t, PX + PW - 8, top_row_y, "^", COL_PATH, 1);
        if (last < b->entry_count)
            osd_nesfb_text(&t, PX + PW - 8, footer_sep_y - LIST_ROW_HEIGHT,
                           "v", COL_PATH, 1);
    }

    /* Footer: separator + hint, identical to the OSD setup menu. */
    osd_nesfb_fill(&t, PX + 3, footer_sep_y, PW - 6, 1, COL_SEP);
    const char *hint = "ENTER OPEN   BKSP UP   O CLOSE   ESC QUIT";
    int hint_w = osd_nesfb_text_width(hint, 1);
    osd_nesfb_text(&t, PX + (PW - hint_w) / 2, PY + PH - 9, hint,
                   COL_HINT, 1);
}
