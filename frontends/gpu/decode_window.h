/*
 * decode_window.h -- The part of the raster the decoder turns into RGB
 * =====================================================================
 *
 * The receiver scans the tube face over the standard active line and field
 * (ITU-R BT.470), locked to the console's sync. The console's 256x240
 * picture is a window in that raster; around it the 2C02 draws its border
 * (the backdrop colour on NTSC, blanking on PAL) and the receiver blanks the
 * retrace. The decoder, the RGB amplifiers and the gun stages work on one
 * rectangle of the raster, the decode window, laid out row by row:
 *
 *   window sample x of row r  =  raster line (first_line + r) mod frame_lines,
 *                                raster sample start_dot * spp + x
 *
 * Raster sample 0 is the leading edge of the line's sync (raster_encode),
 * so the console's picture starts at dot picture_dot (SIGNAL_PICTURE_DOT). The picture sits
 * in the window at (picture_x, picture_row), picture_w x picture_h samples.
 * trace_x0..trace_x1 and trace_row0..trace_row1 are the part of the window
 * the receiver leaves unblanked: the active line and the lines that overlap
 * the active field.
 *
 * Pure static inline C, so tests and tools can include it without linking
 * the GPU chain.
 */
#ifndef DECODE_WINDOW_H
#define DECODE_WINDOW_H

#include "signal_format.h"
#include <math.h>
#include <stddef.h>
#include <string.h>

/* Samples kept either side of the unblanked line: the widest horizontal
 * spot halo (h_blur_rgb.comp.glsl) reads 32 samples beyond an output, and
 * the RGB amplifier's 31 taps reach 15. */
#define DECODE_WINDOW_HALO_SAMPLES 32

typedef struct {
    int   spp, dots_per_line, frame_lines;     /* samples per dot; raster dots per line; raster lines */
    int   start_dot, dots, width;              /* decoded raster dots; width = dots * spp, the RGB stride */
    int   first_line, lines;                   /* row r is picture line first_line + r */
    int   picture_dot, picture_x, picture_row; /* raster dot of the picture; its place in the window */
    int   picture_w, picture_h;                /* the console picture: 256 * spp samples, 240 lines */
    float trace_x0, trace_x1;                  /* the receiver's unblanked line, in window samples */
    int   trace_row0, trace_row1;              /* unblanked rows, [row0, row1) */
    float active_dots, picture_left;           /* dots across the active line; from its left edge to the picture */
    float active_lines, picture_top;           /* lines in the active field; from its top to the picture */
} DecodeWindow;

/* Where the console's picture sits on the receiver's active raster, in dots
 * and lines: dots across the active line, dots from its left edge to the
 * picture, lines in the active field, lines from its top to the picture
 * (negative when the picture's first line is in the blanking).
 *
 * ITU-R BT.470: NTSC line 63.556 us with 10.9 us of blanking and a 1.5 us
 * front porch, 21 blanked lines per field; PAL line 64 us with 12 us of
 * blanking, a 1.5 us front porch and 25 blanked lines. The picture starts
 * picture_dot dots after the console's sync, and its vertical sync pulses
 * start at line 245 or 270 of its 262 or 312-line frame
 * (raster_encode.comp.glsl), so the picture's place follows from the dot
 * period alone: NES dots are 8:7. */
static inline void decode_window_geometry(int region, int samples_per_pixel, int picture_dot,
                                          float *active_dots, float *picture_left,
                                          float *active_lines, float *picture_top) {
    int pal = region == SIGNAL_REGION_PAL;
    double dot_us = 1e6 * samples_per_pixel / signal_region_sample_rate_hz(region);
    double line_us = pal ? 64.0 : 1e6 / 15734.264, blanking_us = pal ? 12.0 : 10.9, front_porch_us = 1.5;
    double blanked_lines = pal ? 25.0 : 21.0, frame_lines = pal ? 312.0 : 262.0, vsync_line = pal ? 270.0 : 245.0;
    double sync_to_active_lines = pal ? 22.5 : 18.0;   /* broad pulses start 2.5 or 3 lines into the blanking */
    *active_dots = (float)((line_us - blanking_us) / dot_us);
    *picture_left = (float)((double)picture_dot - (blanking_us - front_porch_us) / dot_us);
    *active_lines = (float)(frame_lines - blanked_lines);
    *picture_top = (float)((frame_lines - vsync_line) - sync_to_active_lines);
}

/* Fill in everything that follows from spp, the raster geometry and the
 * window's extent (start_dot, dots, first_line, lines). */
static inline void decode_window_place(DecodeWindow *w) {
    w->width = w->dots * w->spp;
    w->picture_x = (w->picture_dot - w->start_dot) * w->spp;
    w->picture_row = -w->first_line;
    w->picture_w = SIGNAL_NES_WIDTH * w->spp;
    w->picture_h = SIGNAL_NES_HEIGHT;
    double trace_dot = (double)w->picture_dot - w->picture_left;
    w->trace_x0 = (float)((trace_dot - w->start_dot) * w->spp);
    w->trace_x1 = (float)((trace_dot + w->active_dots - w->start_dot) * w->spp);
    /* Rows overlapping the active field, picture lines -picture_top to
     * active_lines - picture_top. */
    int row0 = (int)floor(-(double)w->picture_top) - w->first_line;
    int row1 = (int)ceil((double)w->active_lines - w->picture_top) - w->first_line;
    w->trace_row0 = row0 < 0 ? 0 : row0 > w->lines ? w->lines : row0;
    w->trace_row1 = row1 < w->trace_row0 ? w->trace_row0 : row1 > w->lines ? w->lines : row1;
}

static inline void decode_window_begin(DecodeWindow *w, int region, int spp, int dots_per_line, int picture_dot) {
    memset(w, 0, sizeof(*w));
    w->spp = spp;
    w->dots_per_line = dots_per_line > 0 ? dots_per_line : 341;
    w->frame_lines = region == SIGNAL_REGION_PAL ? 312 : 262;
    w->picture_dot = picture_dot;
    decode_window_geometry(region, spp, picture_dot, &w->active_dots, &w->picture_left,
                           &w->active_lines, &w->picture_top);
}

/* The whole unblanked raster: the active line with a halo of
 * DECODE_WINDOW_HALO_SAMPLES either side, and every line that overlaps the
 * active field. The window always holds the picture and never runs past the
 * raster line. */
static inline void decode_window_raster(DecodeWindow *w, int region, int spp, int dots_per_line, int picture_dot) {
    decode_window_begin(w, region, spp, dots_per_line, picture_dot);
    double trace0 = (double)picture_dot - w->picture_left, trace1 = trace0 + w->active_dots;
    int margin = (DECODE_WINDOW_HALO_SAMPLES + spp - 1) / spp;
    int start = (int)floor(trace0) - margin, end = (int)ceil(trace1) + margin;
    if (start > picture_dot) start = picture_dot;
    if (end < picture_dot + SIGNAL_NES_WIDTH) end = picture_dot + SIGNAL_NES_WIDTH;
    if (start < 0) start = 0;
    if (end > w->dots_per_line) end = w->dots_per_line;
    int first = (int)floor(-(double)w->picture_top), last = (int)ceil((double)w->active_lines - w->picture_top);
    if (first > 0) first = 0;
    if (last < SIGNAL_NES_HEIGHT) last = SIGNAL_NES_HEIGHT;
    w->start_dot = start;
    w->dots = end - start;
    w->first_line = first;
    w->lines = last - first;
    decode_window_place(w);
}

/* The console picture alone, 256 dots from SIGNAL_PICTURE_DOT and lines 0 to
 * 239: the layout the decoder used before it decoded the border. */
static inline void decode_window_picture(DecodeWindow *w, int region, int spp) {
    decode_window_begin(w, region, spp, 341, SIGNAL_PICTURE_DOT);
    w->start_dot = SIGNAL_PICTURE_DOT;
    w->dots = SIGNAL_NES_WIDTH;
    w->first_line = 0;
    w->lines = SIGNAL_NES_HEIGHT;
    decode_window_place(w);
}

static inline size_t decode_window_samples(const DecodeWindow *w) {
    return (size_t)w->width * (size_t)w->lines;
}

/* Raster line of window row r. */
static inline int decode_window_raster_line(const DecodeWindow *w, int row) {
    return ((w->first_line + row) % w->frame_lines + w->frame_lines) % w->frame_lines;
}

/* Index of the red float of a picture sample in the interleaved RGB. */
static inline size_t decode_window_rgb_index(const DecodeWindow *w, int picture_line, int picture_sample) {
    long long row = picture_line + w->picture_row, sample = w->picture_x + picture_sample;
    return (size_t)((row * w->width + sample) * 3);
}

#endif /* DECODE_WINDOW_H */
