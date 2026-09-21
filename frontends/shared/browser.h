/* ROM browser state and 256x240 UI. The GPU frontend composites RGBA after
 * receiver decoding; the SDL2 frontend retains its palette framebuffer path. */
#ifndef MYNES_BROWSER_H
#define MYNES_BROWSER_H
#include <stdbool.h>
#include <stdint.h>
#include "config.h"

typedef enum {
    BROWSER_KEY_UP, BROWSER_KEY_DOWN, BROWSER_KEY_LEFT, BROWSER_KEY_RIGHT,
    BROWSER_KEY_ENTER, BROWSER_KEY_BACK, BROWSER_KEY_ESCAPE,
    BROWSER_KEY_PAGEUP, BROWSER_KEY_PAGEDOWN, BROWSER_KEY_HOME, BROWSER_KEY_END,
    BROWSER_KEY_TAB,
} BrowserKey;
typedef enum { BROWSER_BROWSING, BROWSER_SELECTED, BROWSER_CANCELLED } BrowserResult;
#define BROWSER_MAX_ENTRIES 1024
#define BROWSER_VISIBLE_ROWS 14

typedef struct {
    char current_dir[MYNES_PATH_MAX];
    char entries[BROWSER_MAX_ENTRIES][256];
    bool entry_is_dir[BROWSER_MAX_ENTRIES];
    int entry_count;
    const MynesConfig *config;
    bool recent_tab;
    int selected, scroll_offset;
    int visible[BROWSER_MAX_ENTRIES], visible_count;
    char filter[64];
    char error[128];
    bool truncated;
    bool can_resume;
    char chosen_path[MYNES_PATH_MAX];
} Browser;

/* NULL start_dir uses the most recent ROM's directory, falling back to cwd. */
bool browser_init(Browser *b, const char *start_dir, const MynesConfig *config);
void browser_refresh(Browser *b);
BrowserResult browser_handle_key(Browser *b, BrowserKey key);
void browser_handle_text(Browser *b, const char *text);
void browser_set_error(Browser *b, const char *message);
void browser_render(const Browser *b, uint8_t *rgb, uint16_t *indices,
                    const uint8_t (*palette)[3]);
/* Opaque, palette-independent R,G,B,A bytes for the post-decoder UI plane. */
void browser_render_rgba(const Browser *b, uint32_t *rgba);
#endif
