#ifndef FRAME_CAPTURE_H
#define FRAME_CAPTURE_H

#include <stdbool.h>
#include <stdint.h>

/* Packed RGBA16F linear or RGBA/BGRA8 sRGB pixels, top row first. */
typedef struct {
    const void *pixels;
    int width, height;
    bool hdr, bgra;
    float white_level;
} FrameCaptureImage;

typedef struct FrameCaptureJob FrameCaptureJob;

/* PPM clips at SDR white; the companion PFM retains linear HDR values. */
bool frame_capture_write(const FrameCaptureImage *image, const char *path);
/* The PPM's pixels without the file: top row first, 3 bytes per pixel into
 * rgb (width * height * 3 bytes). False only when nothing can be converted. */
bool frame_capture_rgb24(const FrameCaptureImage *image, uint8_t *rgb);
/* Copies pixels and path before returning. Own at most one job at a time. */
FrameCaptureJob *frame_capture_start(const FrameCaptureImage *image, const char *path);
/* Joins, reports write failure, and releases the job. NULL is a no-op. */
bool frame_capture_finish(FrameCaptureJob *job);

#endif
