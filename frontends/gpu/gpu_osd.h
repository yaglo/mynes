#ifndef GPU_OSD_H
#define GPU_OSD_H
#include "nes/osd.h"
#include "gpu_render.h"

/* RGB OSD inserted after the receiver, before the tube. Packed R,G,B,A bytes. */
#define GPU_OSD_PIXELS (256 * 240)
void gpu_osd_render(uint32_t *rgba, const OSDMenuLevel *level, bool editing,
                    const char *preset, bool modified, bool pal, const GPURenderCtx *render, float headroom);
void gpu_osd_preset_notice(uint32_t *rgba, const char *name);
void gpu_osd_performance(uint32_t *rgba, const char *stats);
void gpu_osd_blend_rgb(uint8_t *rgb, const uint32_t *rgba);

static inline bool gpu_osd_editable(const OSDMenuItem *item) {
    return item && item->target && item->type <= OSD_MI_TOGGLE;
}

/* Header-local navigation uses the caller's header-local menu stack. */
static inline bool gpu_osd_handle_key(SDL_Scancode key, bool *editing) {
    if (!osd_menu_is_open) return false;
    switch (key) {
    case SDL_SCANCODE_M:
        *editing=false; osd_menu_close(); return true;
    case SDL_SCANCODE_ESCAPE:
    case SDL_SCANCODE_BACKSPACE:
        if (*editing) *editing=false;
        else osd_menu_back();
        return true;
    case SDL_SCANCODE_RETURN:
        if (*editing) *editing=false;
        else if (gpu_osd_editable(osd_menu_current_item())) *editing=true;
        else osd_menu_activate();
        return true;
    case SDL_SCANCODE_LEFT:
    case SDL_SCANCODE_RIGHT:
        if (gpu_osd_editable(osd_menu_current_item())) {
            *editing=true;
            osd_menu_adjust(key==SDL_SCANCODE_LEFT ? -1 : 1);
        }
        return true;
    case SDL_SCANCODE_UP:
    case SDL_SCANCODE_DOWN:
        osd_menu_move(key==SDL_SCANCODE_UP ? -1 : 1);
        /* Keep the compact adjustment panel when moving between settings.
         * A submenu/action returns to the list; navigation never activates it. */
        *editing = *editing && gpu_osd_editable(osd_menu_current_item());
        return true;
    default: return false;
    }
}
#endif
