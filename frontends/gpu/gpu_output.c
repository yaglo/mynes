#include "gpu_output.h"
#include <math.h>
#ifndef __APPLE__
void gpu_output_geometry(SDL_Window *window, GPUOutputGeometry *out) {
    (void)window;
    *out=(GPUOutputGeometry){.scale_x=1,.scale_y=1};
}
#endif

bool gpu_output_toggle_fullscreen(SDL_Window *window, bool native) {
    if (SDL_GetWindowFlags(window) & SDL_WINDOW_FULLSCREEN) {
        if (!SDL_SetWindowFullscreen(window,false)) return false;
        SDL_ShowCursor();
        return true;
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
    if (!SDL_SetWindowFullscreenMode(window,found ? &chosen : NULL) ||
        !SDL_SetWindowFullscreen(window,true)) return false;
    SDL_HideCursor();
    return true;
}
