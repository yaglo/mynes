/* Queue control in INPUT mono frames, independent of device format/rate.
 * Sample production remains tied to emulated time. SDL makes the small
 * clock correction when converting for the playback device. */
#ifndef AUDIO_SYNC_H
#define AUDIO_SYNC_H
#include <stdbool.h>
#include <math.h>
#include "audio_chain.h"
#define AUDIO_QUEUE_TARGET (AUDIO_STREAM_RATE / 100)  /* 10 ms before a frame */
#define AUDIO_QUEUE_LIMIT (AUDIO_STREAM_RATE * 80 / 1000)
typedef struct { double integral; float ratio; } AudioRateCtrl;
static inline float audio_sync_ratio(AudioRateCtrl *c, int queued, double dt) {
    double error = (double)(queued - AUDIO_QUEUE_TARGET) / AUDIO_STREAM_RATE;
    c->integral = fmax(-0.003, fmin(0.003, c->integral + error * dt * 0.02));
    double target = 1.0 + fmax(-0.005, fmin(0.005, error * 0.1 + c->integral));
    if (c->ratio == 0) c->ratio = 1;
    c->ratio += (float)((target - c->ratio) * fmin(dt * 4, 1));
    return c->ratio;
}
static inline bool audio_sync_stale(int queued, int incoming) {
    return queued > 0 && queued + incoming > AUDIO_QUEUE_LIMIT;
}
#endif
