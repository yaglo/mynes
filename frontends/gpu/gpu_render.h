/*
 * gpu_render.h -- GPU texture management, rendering, and color conversion
 */
#ifndef GPU_RENDER_H
#define GPU_RENDER_H

#include <SDL3/SDL.h>
#include <stdbool.h>
#include <stdint.h>
#include "gpu_display.h"
#include "gpu_output.h"
#include "video_chain.h"

typedef struct {
    SDL_GPUDevice  *gpu;
    SDL_Window     *window;
    SDL_GPUTexture *display_tex;
    int             display_tex_w, display_tex_h;
    GPUDisplay     *gpu_disp;
    bool            gpu_display_enabled;
    bool            crt_shader_enabled;
    int             mask_alignment;      /* 0 = panel pixels, 1 = physical CRT pitch */
    GPUOutputGeometry output_geometry;
    int             drawable_w, drawable_h;
    float           effective_mask_triads;
    bool            hdr_enabled;         /* RGBA16F textures + HDR swapchain */
    bool            owns_display_tex;    /* false when using external beam texture */
    float           frame_brightness;    /* instantaneous avg luma of current PPU frame */
    float           hv_sag_state;        /* smoothed HV sag level (0-1), updated per frame */
    float           apl_slow_state;      /* §4.9 APL black-level tracker, tau ≈ 120ms */
    /* §5.2 thermal-mask approximation: long-timescale per-channel EMA
     * of frame brightness, ~15 seconds. The three values drift apart
     * during scenes heavy in a given color, mimicking purity drift as
     * the mask heats locally. */
    float           thermal_r_state;
    float           thermal_g_state;
    float           thermal_b_state;
    /* §6.2 microphonic tracker — RMS energy of recent audio samples
     * in the bass band. Updated by the audio pipeline and sampled
     * each frame for the raster-wobble uniform. */
    float           audio_bass_rms;
    uint64_t        frame_counter;
    const char     *capture_path;
    bool            capture_complete;
    uint64_t        swap_wait_ns, submit_ns, capture_ns; /* diagnostics; not photon timestamps */
    SDL_GPUCommandBuffer *present_cmd;
    SDL_GPUTexture *present_texture;
    Uint32 present_w, present_h;
    int offscreen_w, offscreen_h;
    float offscreen_headroom;
    SDL_GPUTexture *offscreen_target;
    SDL_GPUFence *offscreen_fence;
    bool            split_mode;          /* Shift+C: split view (left CRT, right raw palette) */
    SDL_GPUTexture *raw_tex;             /* 256x240 RGBA8 raw PPU frame, used in split mode */
    const uint8_t  *raw_ppu_rgb;         /* pointer to PPU RGB framebuffer for upload */
} GPURenderCtx;

/* Compute avg luminance of an RGB888 PPU framebuffer (256x240).
 * Result in [0,1]. Used for HV-sag CRT effect (bright scenes expand image). */
float gpu_render_compute_frame_brightness(const uint8_t *rgb888);

/* Compute per-channel averages (sparse-sampled) of an RGB888 PPU
 * framebuffer. Used by the §5.2 thermal-mask doming approximation,
 * which tracks each gun's long-timescale load separately. */
void gpu_render_compute_rgb_averages(const uint8_t *rgb888,
                                      float *out_r, float *out_g, float *out_b);

/* Ensure the display texture matches the requested size; recreate if needed. */
void gpu_render_ensure_texture(GPURenderCtx *ctx, int w, int h);

/* Upload an RGBA8 buffer to the display texture via a transfer buffer. */
void gpu_render_upload_rgba(GPURenderCtx *ctx, const uint8_t *rgba, int w, int h);

/* Advance the slow render-domain state once per frame (HV sag, APL
 * tracker, thermal drift, microphonic phase counter). Call after the
 * caller refreshes frame_brightness/raw_ppu_rgb/audio_bass_rms for the
 * current frame and before any stage that consumes those values. */
void gpu_render_update_dynamic_state(GPURenderCtx *ctx);

/* Wait for display capacity before taking a picture from the playback
 * mailbox. Keeping this wait after encoding used to present stale pictures. */
bool gpu_render_prepare(GPURenderCtx *ctx);
void gpu_render_release_pending(GPURenderCtx *ctx);

/* Render the display texture to the swapchain (CRT shader or passthrough blit). */
void gpu_render_frame(GPURenderCtx *ctx, const VideoChain *chain);

/* Convert float RGB (3 floats/pixel, [0,1]) to RGBA8.
 * Returns pointer to a static buffer (reused each call). */
uint8_t *gpu_render_float_rgb_to_rgba8(const float *rgb, int pixel_count);

/* Convert PPU RGB888 framebuffer (256x240) to RGBA8.
 * Returns pointer to a static buffer. */
uint8_t *gpu_render_ppu_to_rgba8(const uint8_t *rgb888);

#endif /* GPU_RENDER_H */
