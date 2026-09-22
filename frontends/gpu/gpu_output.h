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
/* Keep macOS from moving the window into a Space of its own (Globe+F, the
 * green button): that fullscreen runs at the scaled desktop size, which the
 * compositor resamples. Call before SDL_Init. */
void gpu_output_disable_desktop_spaces(void);
/* Globe (fn) chords. SDL reports no modifier for the Globe key and the text
 * system still types the letter, so Globe+F would reach text fields as "f".
 * After SDL_Init, the filter drops text typed with Globe held and marks the
 * key event; gpu_output_globe_key() tells whether a key-down was a chord. */
void gpu_output_watch_globe(void);
bool gpu_output_globe_key(const SDL_KeyboardEvent *key);
#endif
