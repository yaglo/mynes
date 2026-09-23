#ifndef GPU_OUTPUT_H
#define GPU_OUTPUT_H
#include <SDL3/SDL.h>
#include <math.h>
/* SDL's safe area (window coordinates) in drawable pixels. Edges round
 * inward, so a pixel the camera housing partly covers never counts as clear. */
static inline SDL_Rect gpu_output_safe_pixels(SDL_Rect safe, int window_w, int window_h,
                                              int drawable_w, int drawable_h) {
    SDL_Rect all={0,0,drawable_w,drawable_h};
    if (window_w<=0 || window_h<=0 || safe.w<=0 || safe.h<=0) return all;
    double sx=(double)drawable_w/window_w, sy=(double)drawable_h/window_h;
    int left=(int)ceil(safe.x*sx), top=(int)ceil(safe.y*sy);
    int right=(int)floor((safe.x+safe.w)*sx), bottom=(int)floor((safe.y+safe.h)*sy);
    if (left<0) left=0;
    if (top<0) top=0;
    if (right>drawable_w) right=drawable_w;
    if (bottom>drawable_h) bottom=drawable_h;
    if (right<=left || bottom<=top) return all;
    return (SDL_Rect){left,top,right-left,bottom-top};
}
/* The tube face: the largest rectangle of `aspect` inside `area`, with
 * whole-pixel edges, centred in it. */
static inline SDL_FRect gpu_output_fit_picture(SDL_Rect area, float aspect) {
    float w=(float)area.w, h=(float)area.h;
    if (w/h>aspect) w=h*aspect;
    else h=w/aspect;
    w=floorf(w); h=floorf(h);
    return (SDL_FRect){area.x+floorf(((float)area.w-w)*0.5f),
                       area.y+floorf(((float)area.h-h)*0.5f),w,h};
}
/* Drawable pixels the picture may use. Fullscreen leaves out what SDL
 * reports as unsafe, on macOS NSScreen.safeAreaInsets: the top 32 points of
 * a notched MacBook panel. A window keeps the whole drawable. */
SDL_Rect gpu_output_safe_area(SDL_Window *window, int drawable_w, int drawable_h);
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

/* Give the window's HDR layer extended linear Display P3, so each channel
 * drives the panel's own primary, or the extended linear sRGB SDL set up.
 * Cheap to call every frame; it only touches the layer when the space
 * differs. Returns whether the layer is P3 now. */
bool gpu_output_apply_colorspace(SDL_Window *window, bool p3);
/* Globe (fn) chords. SDL reports no modifier for the Globe key and the text
 * system still types the letter, so Globe+F would reach text fields as "f".
 * After SDL_Init, the filter drops text typed with Globe held and marks the
 * key event; gpu_output_globe_key() tells whether a key-down was a chord. */
void gpu_output_watch_globe(void);
bool gpu_output_globe_key(const SDL_KeyboardEvent *key);
#endif
