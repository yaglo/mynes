#ifndef FRAME_CAPTURE_H
#define FRAME_CAPTURE_H

#include <stdbool.h>

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
/* Copies pixels and path before returning. Own at most one job at a time. */
FrameCaptureJob *frame_capture_start(const FrameCaptureImage *image, const char *path);
/* Joins, reports write failure, and releases the job. NULL is a no-op. */
bool frame_capture_finish(FrameCaptureJob *job);

#endif
