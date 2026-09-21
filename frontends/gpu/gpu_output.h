#ifndef GPU_OUTPUT_H
#define GPU_OUTPUT_H
#include <SDL3/SDL.h>
/* Physical panel coordinates of the drawable, before desktop resampling.
 * Unknown platforms fall back to drawable pixels without claiming calibration. */
typedef struct {
    float scale_x, scale_y, origin_x, origin_y;
    int native_w, native_h;
    bool native_known, resampled;
} GPUOutputGeometry;
void gpu_output_geometry(SDL_Window *window, GPUOutputGeometry *out);
bool gpu_output_toggle_fullscreen(SDL_Window *window, bool native);
#endif
