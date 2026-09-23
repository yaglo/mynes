#include "gpu_output.h"
#include <math.h>
#ifndef __APPLE__
bool gpu_output_apply_colorspace(SDL_Window *window, bool p3) { (void)window; (void)p3; return false; }

void gpu_output_geometry(SDL_Window *window, GPUOutputGeometry *out) {
    (void)window;
    *out=(GPUOutputGeometry){.scale_x=1,.scale_y=1};
}
void gpu_output_disable_desktop_spaces(void) {}
void gpu_output_watch_globe(void) {}
bool gpu_output_globe_key(const SDL_KeyboardEvent *key) { (void)key; return false; }
#endif

/* SDL's Cocoa backend sets the insets on entering fullscreen and clears them
 * on leaving; the flag test keeps windows whole on platforms that report
 * insets for windows as well. */
SDL_Rect gpu_output_safe_area(SDL_Window *window, int drawable_w, int drawable_h) {
    SDL_Rect safe;
    int window_w,window_h;
    if (!(SDL_GetWindowFlags(window) & SDL_WINDOW_FULLSCREEN) ||
        !SDL_GetWindowSafeArea(window,&safe) || !SDL_GetWindowSize(window,&window_w,&window_h))
        return (SDL_Rect){0,0,drawable_w,drawable_h};
    return gpu_output_safe_pixels(safe,window_w,window_h,drawable_w,drawable_h);
}

bool gpu_output_toggle_fullscreen(SDL_Window *window, bool native) {
    if (SDL_GetWindowFlags(window) & SDL_WINDOW_FULLSCREEN) {
        return SDL_SetWindowFullscreen(window,false);
    }
    GPUOutputGeometry geometry;
    gpu_output_geometry(window,&geometry);
    SDL_DisplayMode chosen, **modes=NULL;
    bool found=false;
    if (native && geometry.native_known) {
        int count=0;
        modes=SDL_GetFullscreenDisplayModes(SDL_GetDisplayForWindow(window),&count);
        for(int i=0;i<count;i++) {
            const SDL_DisplayMode *mode=modes[i];
            if (lroundf(mode->w*mode->pixel_density)==geometry.native_w &&
                lroundf(mode->h*mode->pixel_density)==geometry.native_h &&
                (!found || mode->pixel_density>chosen.pixel_density ||
                 (mode->pixel_density==chosen.pixel_density && mode->refresh_rate>chosen.refresh_rate))) {
                chosen=*mode; found=true;
            }
        }
        SDL_free(modes);
        if (!found) return SDL_SetError("No native fullscreen mode for this panel");
    }
    return SDL_SetWindowFullscreenMode(window,found ? &chosen : NULL) &&
           SDL_SetWindowFullscreen(window,true);
}
