/* PQ encoding of the half-float display target for HDR recording. The
 * offscreen target holds extended-linear BT.709 light with 1.0 at SDR white
 * and highlights above it up to the render headroom; negative components
 * are colours outside BT.709. Each pixel becomes BT.2020 SMPTE ST 2084
 * R'G'B', then Y'CbCr with the BT.2020 non-constant-luminance matrix in
 * limited range, 16 bits per sample in three planes (yuv444p16 in host byte
 * order). */
#ifndef FRAME_PQ_H
#define FRAME_PQ_H

#include <stdbool.h>
#include <stdint.h>
#include "frame_capture.h"

#define FRAME_PQ_PEAK_NITS 10000.0

/* The ffmpeg rawvideo name of the frames frame_pq_convert writes. */
#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
#define FRAME_PQ_PIX_FMT "yuv444p16be"
#else
#define FRAME_PQ_PIX_FMT "yuv444p16le"
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
/* Converts a half-float image (image->hdr) into three planes of
 * width * height samples, Y' then Cb then Cr, top row first. The codes are
 * the BT.2100 limited-range ones at 16 bits: Y' 4096 to 60160, Cb and Cr
 * 4096 to 61440 around 32768. Rows are split across threads for large
 * frames. False when the image is not half-float or has no pixels. */
bool     frame_pq_convert(const FramePQ *pq, const FrameCaptureImage *image,
                          uint16_t *yuv, FramePQLight *light);

/* --- The steps, for tests --- */

/* BT.709 to BT.2020 primaries, both linear with D65 white (ITU-R BT.2087). */
void   frame_pq_bt2020(const float bt709[3], float bt2020[3]);
/* SMPTE ST 2084 inverse EOTF in double precision: nits (clamped to
 * 0..10000) to a signal of 0..1. */
double frame_pq_encode(double nits);
/* The table lookup frame_pq_convert uses, same input and output. */
float  frame_pq_lookup(const FramePQ *pq, float nits);

#endif /* FRAME_PQ_H */
