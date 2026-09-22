/* PQ encoding of the half-float display target for HDR recording. The
 * offscreen target holds extended-linear BT.709 light with 1.0 at SDR white
 * and highlights above it up to the render headroom; negative components
 * are colours outside BT.709. Each pixel becomes BT.2020 SMPTE ST 2084 code
 * values, 16 bits per channel (rgb48 in host byte order). */
#ifndef FRAME_PQ_H
#define FRAME_PQ_H

#include <stdbool.h>
#include <stdint.h>
#include "frame_capture.h"

#define FRAME_PQ_PEAK_NITS 10000.0

/* The ffmpeg rawvideo name of the rgb48 frames frame_pq_convert writes. */
#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
#define FRAME_PQ_PIX_FMT "rgb48be"
#else
#define FRAME_PQ_PIX_FMT "rgb48le"
#endif

typedef struct FramePQ FramePQ;

/* Light of one converted frame, from the largest BT.2020 component of each
 * pixel in nits (the CTA-861.3 MaxCLL/MaxFALL measure). */
typedef struct {
    double max_nits;
    double mean_nits;
} FramePQLight;

/* Tables for SDR white (1.0) at white_nits. NULL when white_nits is not in
 * (0, 10000] or memory is short. */
FramePQ *frame_pq_create(double white_nits);
void     frame_pq_destroy(FramePQ *pq);
/* Converts a half-float image (image->hdr) into rgb48, width * height * 3
 * values top row first. Rows are split across threads for large frames.
 * False when the image is not half-float or has no pixels. */
bool     frame_pq_convert(const FramePQ *pq, const FrameCaptureImage *image,
                          uint16_t *rgb48, FramePQLight *light);

/* --- The steps, for tests --- */

/* BT.709 to BT.2020 primaries, both linear with D65 white (ITU-R BT.2087). */
void   frame_pq_bt2020(const float bt709[3], float bt2020[3]);
/* SMPTE ST 2084 inverse EOTF in double precision: nits (clamped to
 * 0..10000) to a signal of 0..1. */
double frame_pq_encode(double nits);
/* The table lookup frame_pq_convert uses, same input and output. */
float  frame_pq_lookup(const FramePQ *pq, float nits);

#endif /* FRAME_PQ_H */
