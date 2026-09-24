#ifndef CRT_LIVE_H
#define CRT_LIVE_H
#include <stddef.h>
#include <stdint.h>
#include "crt_protocol.h"
typedef struct crt_live crt_live;
/* Owns its USB connection. Exactly one in-flight and one replaceable pending
 * frame. Audio is queued in a ring and rides inside the next video packet
 * (kind 3, mono), paced by the queue level the box reports, so it costs no
 * extra round trips; when no frames are coming it goes out alone. Opening
 * probes the box for audio support. */
crt_live *crt_live_open(char *error, size_t error_size);
int crt_live_submit(crt_live *stream, const uint16_t pixels[CRT_PIXELS]);
/* Mono 16-bit samples at crt_live_audio_rate(). Returns the number of frames
 * dropped because the queue (about 0.7 s) was full. */
size_t crt_live_submit_audio(crt_live *stream, const int16_t *samples, size_t frames);
/* Sample rate the box consumes, in Hz; 0 when the image has no audio path. */
uint32_t crt_live_audio_rate(crt_live *stream);
void crt_live_stats(crt_live *stream, uint64_t *sent, uint64_t *replaced);
/* Packets that carried audio, the box's queue level in frames, its underrun
 * count and frames dropped on the host side, from the most recent acknowledgement. */
void crt_live_audio_stats(crt_live *stream, uint64_t *packets, unsigned *level, unsigned *underruns, size_t *dropped);
void crt_live_close(crt_live *stream);
#endif
