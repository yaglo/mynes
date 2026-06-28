/*
 * browser.h — Fullscreen ROM browser drawn into the NES framebuffer.
 *
 * Used by both frontends (mynes, mynes_gpu) when the user launches with
 * no ROM argument or reopens the browser mid-session. Renders into the
 * 256×240 NES framebuffer using the same osd_nesfb_* primitives the
 * setup menu uses, so styling stays consistent.
 *
 * Layout:
 *   ┌─────────────────────────────────────┐
 *   │ MYNES — SELECT ROM                  │  title bar
 *   │ <current path>                      │  breadcrumb
 *   │                                     │
 *   │ RECENT                              │  if any recent ROMs
 *   │   game1.nes                         │
 *   │   ...                               │
 *   │                                     │
 *   │ FILES                               │
 *   │   ../                               │
 *   │   subdir/                           │  directories first
 *   │   game.nes                          │  then .nes files
 *   │   ...                               │
 *   │                                     │
 *   │ ↑↓ navigate  Enter open  Esc quit   │  footer
 *   └─────────────────────────────────────┘
 */
#ifndef MYNES_BROWSER_H
#define MYNES_BROWSER_H

#include <stdbool.h>
#include <stdint.h>
#include "config.h"

typedef enum {
    BROWSER_KEY_UP,
    BROWSER_KEY_DOWN,
    BROWSER_KEY_LEFT,
    BROWSER_KEY_RIGHT,
    BROWSER_KEY_ENTER,
    BROWSER_KEY_BACK,        /* Backspace — go up one directory */
    BROWSER_KEY_ESCAPE,      /* close browser (or quit if at startup) */
    BROWSER_KEY_PAGEUP,
    BROWSER_KEY_PAGEDOWN,
} BrowserKey;

typedef enum {
    BROWSER_BROWSING,        /* still browsing — keep handing it events */
    BROWSER_SELECTED,        /* user picked something; chosen_path is set */
    BROWSER_CANCELLED,       /* user pressed Escape */
} BrowserResult;

#define BROWSER_MAX_ENTRIES 1024

typedef struct {
    /* --- Where we are now --- */
    char current_dir[MYNES_PATH_MAX];

    /* --- Filtered, sorted entries in current_dir --- */
    char entries[BROWSER_MAX_ENTRIES][256];
    bool entry_is_dir[BROWSER_MAX_ENTRIES];
    int  entry_count;

    /* --- Recent ROMs section (passed in at init) --- */
    const MynesConfig *config;   /* read-only; for the recent list */

    /* --- Selection / scroll state ---
     * selected indexes a unified list:
     *   0 .. recent_visible_count-1     → recent ROMs
     *   recent_visible_count .. (+entry_count-1)  → file-system entries */
    int  selected;
    int  scroll_offset;
    int  recent_visible_count;   /* min(config->recent_count, RECENT_DISPLAY_MAX) */

    /* --- Output (valid after BROWSER_SELECTED) --- */
    char chosen_path[MYNES_PATH_MAX];
} Browser;

/* Initialise the browser at `start_dir` (NULL → cwd). The config pointer
 * is borrowed; the browser will read recent_roms[] from it when rendering.
 * Returns true on success. */
bool browser_init(Browser *b, const char *start_dir, const MynesConfig *config);

/* Feed a key event. Returns the browser's state after handling it.
 * `chosen_path` is populated iff the return is BROWSER_SELECTED. */
BrowserResult browser_handle_key(Browser *b, BrowserKey key);

/* Draw the browser into the supplied 256×240 NES framebuffer + index FB.
 * The palette argument is the active PPU palette (64×3 RGB). */
void browser_render(const Browser *b,
                    uint8_t *framebuffer, uint16_t *index_framebuffer,
                    const uint8_t (*palette)[3]);

#endif /* MYNES_BROWSER_H */
