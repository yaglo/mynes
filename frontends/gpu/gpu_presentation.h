/* Host-display pulse timing. This does not advance the emulated raster. */
#ifndef GPU_PRESENTATION_H
#define GPU_PRESENTATION_H
#include <math.h>

/* An integer number of host refreshes per source frame is required. Allow
 * the small NTSC 60.0988 vs 120 Hz mismatch, but not 144/60 judder. */
static inline int gpu_presentation_slots(float refresh_hz, float source_hz) {
    if (!isfinite(refresh_hz) || !isfinite(source_hz) || source_hz <= 0)
        return 1;
    float ratio = refresh_hz / source_hz;
    if (ratio < 1.99f || ratio > 8.01f) return 1;
    int slots = (int)roundf(ratio);
    return fabsf(ratio / slots - 1) <= .005f ? slots : 1;
}

/* floor is dark-refresh emission relative to the bright refresh. Conserve
 * time-averaged linear light BEFORE the display's peak-limiting shoulder. */
static inline float gpu_presentation_gain(int slots, int slot, float floor) {
    floor = fminf(1, fmaxf(0, floor));
    if (slots <= 1) return 1;
    return slots * (slot == 0 ? 1 : floor) / (1 + (slots - 1) * floor);
}
#endif
