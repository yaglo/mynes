/*
 * NES Hardware Abstraction Layer (HAL)
 *
 * Platform-agnostic interface for frontends. Each frontend implements
 * these callbacks for video, audio, input, and timing.
 *
 * Usage:
 *   NES_HAL hal;
 *   hal.video_present = my_present;
 *   hal.audio_push = my_audio;
 *   hal.input_poll = my_poll;
 *   ...
 *   nes_hal_run(&hal, &nes);
 */

#ifndef NES_HAL_H
#define NES_HAL_H

#include <stdint.h>
#include <stdbool.h>

/* ============================================================================
 * Callback Types
 * ============================================================================ */

/* Present a completed frame (256x240 RGB888 pixels) */
typedef void (*hal_video_present_fn)(const uint8_t *framebuffer, void *user_data);

/* Push one audio sample (mono float, -1.0 to 1.0) */
typedef void (*hal_audio_push_fn)(float sample, void *user_data);

/* Poll controller input — returns button state bitmask */
typedef uint8_t (*hal_input_poll_fn)(int controller, void *user_data);

/* Check if the frontend wants to quit */
typedef bool (*hal_should_quit_fn)(void *user_data);

/* Frame sync — called after each frame, frontend controls timing */
typedef void (*hal_frame_sync_fn)(void *user_data);

/* ============================================================================
 * HAL Structure
 * ============================================================================ */

typedef struct {
    /* Required callbacks */
    hal_video_present_fn  video_present;
    hal_input_poll_fn     input_poll;
    hal_should_quit_fn    should_quit;

    /* Optional callbacks (NULL = disabled) */
    hal_audio_push_fn     audio_push;
    hal_frame_sync_fn     frame_sync;

    /* User data passed to all callbacks */
    void *user_data;
} NES_HAL;

#endif /* NES_HAL_H */
