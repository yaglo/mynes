/* Picture placement on the host drawable: the safe area in pixels and the
 * tube face fitted into it. */
#include "gpu_output.h"
#include <stdio.h>

static int failures;
#define CHECK(x) do { if (!(x)) { fprintf(stderr,"FAIL line %d: %s\n",__LINE__,#x); failures++; } } while(0)

static bool rect_is(SDL_Rect r, int x, int y, int w, int h) {
    return r.x==x && r.y==y && r.w==w && r.h==h;
}
static bool frect_is(SDL_FRect r, float x, float y, float w, float h) {
    return r.x==x && r.y==y && r.w==w && r.h==h;
}

/* gpu_render.c before the safe area, kept to show that a whole drawable
 * still gets the same viewport to the pixel. */
static SDL_FRect previous_fit(int sw, int sh, float aspect) {
    float vp_w, vp_h;
    if ((float)sw/(float)sh > aspect) { vp_h=(float)sh; vp_w=vp_h*aspect; }
    else { vp_w=(float)sw; vp_h=vp_w/aspect; }
    vp_w=floorf(vp_w); vp_h=floorf(vp_h);
    return (SDL_FRect){floorf(((float)sw-vp_w)*0.5f),floorf(((float)sh-vp_h)*0.5f),vp_w,vp_h};
}

int main(void) {
    /* Native fullscreen on a 2560x1664 notched panel: 1280x832 points at 2x,
     * NSScreen.safeAreaInsets.top 32 points. */
    SDL_Rect safe=gpu_output_safe_pixels((SDL_Rect){0,32,1280,800},1280,832,2560,1664);
    CHECK(rect_is(safe,0,64,2560,1600));
    CHECK(frect_is(gpu_output_fit_picture(safe,4.0f/3.0f),213,64,2133,1600));
    CHECK(frect_is(gpu_output_fit_picture(safe,16.0f/10.0f),0,64,2560,1600));
    /* The whole panel, as before: the top 32 points are under the housing. */
    CHECK(frect_is(gpu_output_fit_picture((SDL_Rect){0,0,2560,1664},4.0f/3.0f),171,0,2218,1664));
    /* The scaled desktop mode (1470x956 points, 2940x1912 drawable). */
    CHECK(rect_is(gpu_output_safe_pixels((SDL_Rect){0,32,1470,924},1470,956,2940,1912),0,64,2940,1848));
    /* A fractional ratio rounds inward on every edge. */
    CHECK(rect_is(gpu_output_safe_pixels((SDL_Rect){3,33,994,667},1000,700,1500,1050),5,50,1490,1000));
    /* No insets, or nothing usable, is the whole drawable. */
    CHECK(rect_is(gpu_output_safe_pixels((SDL_Rect){0,0,1280,832},1280,832,2560,1664),0,0,2560,1664));
    CHECK(rect_is(gpu_output_safe_pixels((SDL_Rect){0,0,0,0},1280,832,2560,1664),0,0,2560,1664));
    CHECK(rect_is(gpu_output_safe_pixels((SDL_Rect){0,32,1280,800},0,0,2560,1664),0,0,2560,1664));
    CHECK(rect_is(gpu_output_safe_pixels((SDL_Rect){0,900,1280,10},1280,832,2560,1664),0,0,2560,1664));
    /* Windows and offscreen targets keep their viewport to the pixel. */
    int mismatches=0;
    for (int w=64; w<=4096; w+=37)
        for (int h=64; h<=4096; h+=41)
            for (int a=0; a<2; a++) {
                float aspect=a ? 16.0f/10.0f : 4.0f/3.0f;
                SDL_FRect now=gpu_output_fit_picture((SDL_Rect){0,0,w,h},aspect);
                SDL_FRect before=previous_fit(w,h,aspect);
                if (!frect_is(now,before.x,before.y,before.w,before.h)) mismatches++;
            }
    CHECK(mismatches==0);
    printf("Output placement: %d failures\n",failures);
    return failures!=0;
}
