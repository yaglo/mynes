/*
 * split_view.h -- Where the split view puts the raw palette picture
 * ==================================================================
 *
 * Shift+C draws the right half of the console picture, dots 128 to 255,
 * from the raw PPU palette over the tube's own. The half goes where the
 * displayed texture holds it: on the tube face through the preset's
 * raster when the texture is the beam, on the receiver's unblanked raster
 * as decoded when the GPU has no beam and shows the RGB crop, and over
 * the whole texture when it is the 256x240 palette picture.
 *
 * Pure static inline C, so tests can include it without SDL.
 */
#ifndef SPLIT_VIEW_H
#define SPLIT_VIEW_H

#include "decode_window.h"
#include <math.h>
#include <stdbool.h>
#include <stdint.h>

/* The half's place on the displayed picture: u across and v down, 0 to 1,
 * for picture dots 128 to 256 and picture lines line0 to line1. */
typedef struct { float u0, u1, v0, v1; int line0, line1; } SplitPlace;

/* The face position of raster coordinate r (0 to 1 across the active line
 * or field): the inverse of the deflection map's overscan, size and
 * position (deflection.comp.glsl). Barrel, keystone, rotation and skew are
 * left out. */
static inline float split_face(float r, float size, float pos, float overscan) {
    float zoom = overscan > 0.001f ? fmaxf(1.0f - overscan * 2.0f, 0.2f) : 1.0f;
    size = size > 0.01f ? size : 1.0f;
    return ((r - 0.5f) * size + pos) / zoom + 0.5f;
}

/* The beam: the face shows the active raster through the preset's overscan,
 * size and position, over the lines the receiver unblanks. */
static inline SplitPlace split_place_face(const DecodeWindow *w, float overscan, float h_size, float v_size,
                                          float h_pos, float v_pos) {
    SplitPlace s;
    s.line0 = (int)fmaxf(0.0f, ceilf(-w->picture_top));
    s.line1 = (int)fminf((float)SIGNAL_NES_HEIGHT, w->active_lines - w->picture_top);
    s.u0 = split_face((128.0f + w->picture_left) / w->active_dots, h_size, h_pos, overscan);
    s.u1 = split_face((256.0f + w->picture_left) / w->active_dots, h_size, h_pos, overscan);
    s.v0 = split_face(((float)s.line0 + w->picture_top) / w->active_lines, v_size, v_pos, overscan);
    s.v1 = split_face(((float)s.line1 + w->picture_top) / w->active_lines, v_size, v_pos, overscan);
    return s;
}

/* The RGB crop without a beam (decode_window_trace_crop): the decoded
 * samples as they are, with no deflection. */
static inline SplitPlace split_place_trace(const DecodeWindow *w) {
    int x0, x1, row0, row1;
    decode_window_trace_crop(w, &x0, &x1, &row0, &row1);
    SplitPlace s;
    s.line0 = row0 - w->picture_row > 0 ? row0 - w->picture_row : 0;
    s.line1 = row1 - w->picture_row < SIGNAL_NES_HEIGHT ? row1 - w->picture_row : SIGNAL_NES_HEIGHT;
    s.u0 = (float)(w->picture_x + 128 * w->spp - x0) / (float)(x1 - x0);
    s.u1 = (float)(w->picture_x + 256 * w->spp - x0) / (float)(x1 - x0);
    s.v0 = (float)(w->picture_row + s.line0 - row0) / (float)(row1 - row0);
    s.v1 = (float)(w->picture_row + s.line1 - row0) / (float)(row1 - row0);
    return s;
}

/* The 256x240 palette picture fills the texture. */
static inline SplitPlace split_place_picture(void) {
    SplitPlace s = {0.5f, 1.0f, 0.0f, 1.0f, 0, SIGNAL_NES_HEIGHT};
    return s;
}

/* One axis of the blit: source units [s0, s1) land on the viewport at
 * [f0, f1) pixels. Trim whole source units that fall outside [lo, hi), the
 * part of the picture the viewport shows. False when nothing is left. */
static inline bool split_axis(float f0, float f1, int s0, int s1, float lo, float hi,
                              uint32_t *src, uint32_t *src_n, uint32_t *dst, uint32_t *dst_n) {
    float per = (f1 - f0) / (float)(s1 - s0);
    if (!(per > 0.0f)) return false;
    int a = s0, b = s1;
    if (f0 < lo) a = s0 + (int)ceilf((lo - f0) / per);
    if (f1 > hi) b = s1 - (int)ceilf((f1 - hi) / per);
    if (b <= a) return false;
    float d0 = f0 + (float)(a - s0) * per;
    *src = (uint32_t)a; *src_n = (uint32_t)(b - a);
    *dst = (uint32_t)lroundf(d0); *dst_n = (uint32_t)lroundf((float)(b - a) * per);
    return *dst_n > 0;
}

#endif /* SPLIT_VIEW_H */
