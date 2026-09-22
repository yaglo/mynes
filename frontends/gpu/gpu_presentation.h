/* Host-display pulse timing. This does not advance the emulated raster. */
#ifndef GPU_PRESENTATION_H
#define GPU_PRESENTATION_H
#include <math.h>
#include <stdint.h>

enum {
    GPU_PRESENT_HOLD,
    GPU_PRESENT_BFI,
    GPU_PRESENT_60HZ
};
#define GPU_PRESENT_60HZ_PERIOD_NS UINT64_C(16666667)

/* Match the frontend's wall clock to 60 Hz without changing emulated cycles
 * or phase. PAL is slower already. Audio resampling compensates this ratio. */
static inline uint64_t gpu_presentation_period_ns(int mode, uint64_t native_period) {
    return mode == GPU_PRESENT_60HZ && native_period < GPU_PRESENT_60HZ_PERIOD_NS
        ? GPU_PRESENT_60HZ_PERIOD_NS : native_period;
}

/* A fixed-refresh panel close to the source rate can pace hold directly by
 * vsync. Do not race an independent Metal timestamp against the same vblank.
 * 50-on-60 and 60-on-120 still need separate cadence handling. */
static inline int gpu_presentation_vsync_paced(int mode, float hz, float source_hz) {
    if (mode == GPU_PRESENT_BFI || !isfinite(hz) || !isfinite(source_hz) || hz <= 0 || source_hz <= 0)
        return 0;
    float target = mode == GPU_PRESENT_60HZ && source_hz > 60 ? 60 : source_hz;
    return fabsf(hz / target - 1) <= .005f;
}

static inline uint64_t gpu_presentation_playback_period(int mode, uint64_t native_period,
                                                       float display_hz, int display_paced) {
    return display_paced && isfinite(display_hz) && display_hz > 0
        ? (uint64_t)llround(1e9 / display_hz)
        : gpu_presentation_period_ns(mode, native_period);
}

/* Absolute deadlines avoid accumulating scheduler overshoot. After a missed
 * whole interval, restart instead of presenting a burst of stale pictures. */
static inline uint64_t gpu_presentation_next_ns(uint64_t deadline, uint64_t submitted) {
    const uint64_t period = GPU_PRESENT_60HZ_PERIOD_NS;
    if (!deadline || submitted > deadline + period) return submitted + period;
    return deadline + period;
}

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
