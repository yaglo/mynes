/*
 * GPU Display Pipeline — CRT Display-Domain Render Shaders
 * ==========================================================
 *
 * Consumes linear beam/phosphor history from the GPU signal chain.
 * Optional area reduction and separable glass scatter precede the mask/glass pass.
 * The last pass writes SDR sRGB or extended linear sRGB to the swapchain.
 * It also supports offscreen targets for numerical and visual validation.
 */

#ifndef GPU_DISPLAY_H
#define GPU_DISPLAY_H

#include <SDL3/SDL.h>
#include <stdbool.h>
#include "video_chain.h"

/* Display pipeline state (opaque to callers). */
typedef struct {
    /* Shader pipelines. */
    SDL_GPUGraphicsPipeline *pipe_halation_reduce; /* area integration before reduction */
    SDL_GPUGraphicsPipeline *pipe_halation;   /* shared H+V blur pipeline */
    SDL_GPUGraphicsPipeline *pipe_crt;        /* CRT display composite */

    /* Halation FBOs (quarter resolution). */
    SDL_GPUTexture *tex_halation_a;    /* H blur output */
    SDL_GPUTexture *tex_halation_b;    /* reduced input, then V blur output */
    int halation_w, halation_h;

    /* Sampler for linear filtering. */
    SDL_GPUSampler *sampler_linear;

    SDL_GPUTexture *mask_tiles[2]; /* shadow dots, staggered slots; linear coverage + mips */
    SDL_GPUSampler *sampler_mask;

    bool initialized;
} GPUDisplay;

/* CRT display uniforms (matches crt_display.frag.glsl UBO layout). */
typedef struct {
    float src_w, src_h;             /* composite texture dimensions */
    int monitor_model;             /* 1: physical FW900 variable-pitch grille */
    float out_w, out_h;             /* display dimensions */
    float barrel;                   /* horizontal barrel distortion */
    float barrel_v;                 /* vertical barrel distortion (0=same as barrel) */
    float convergence_static;       /* legacy UBO slot, beam path handles convergence */
    float convergence_dynamic;      /* legacy UBO slot, beam path handles convergence */
    float mask_strength;            /* phosphor mask strength (0-1) */
    int   mask_type;                /* 0=shadow, 1=aperture_grille, 2=slot */
    float mask_pitch_px;            /* one phosphor cell, in mask-coordinate pixels */
    float mask_row_pitch;           /* 0 = physical aspect; otherwise fitted row spacing */
    float mask_scale_x, mask_scale_y, mask_origin_x, mask_origin_y;
    int   panel_subpixels;          /* host panel: 0 = sample pixel centres, 1 = RGB stripe, 2 = BGR */
    int   damper_wires;             /* aperture-grille damper wires, 0-2 */
    float damper_y[2];              /* wire heights, fraction of the face from the top */
    float damper_width;             /* wire shadow height, fraction of the face height */
    float halation_strength;        /* halation blend intensity */
    float halation_sigma;           /* scatter sigma / picture height; 0 = legacy kernel */
    float halation_tint_r;          /* halation bloom per-channel tint */
    float halation_tint_g;
    float halation_tint_b;
    float vignette;                 /* corner darkening (0-0.3) */
    float gamma;                    /* display gamma (2.0-2.5) */
    float black_floor;              /* minimum black level (0-1) */
    float ambient_light;            /* room light reflection (0-0.2) */
    float glass_tint;               /* glass attenuation (0.6-1.0) */
    float input_gamma;             /* 0 = input already linear */
    float hdr_headroom;            /* current display peak / SDR white */
    float sdr_white_level;         /* scRGB white scale */
    int output_hdr;
    bool pulse_enabled;            /* host BFI, not a television parameter */
    float pulse_gain;              /* zero is a genuinely dark refresh */
    bool reuse_halation;           /* same source texture as previous presentation */
    float hdr_gain;                 /* output multiplier (1.0=normal) */
    int   subpixel_layout;          /* 0=none, 1=RGB, 2=BGR */
    float overscan;                 /* bezel crop fraction per edge (0-0.08) */
    float keystone;                 /* trapezoidal distortion (-0.1 to +0.1) */
    float rotation;                 /* image rotation in radians (-0.05 to +0.05) */
    float skew_x;                   /* horizontal shear (-0.1 to +0.1) */
    float skew_y;                   /* vertical shear (-0.1 to +0.1) */
    float hv_sag;                   /* HV supply sag intensity (0-0.3) */
    float frame_brightness;         /* avg frame luma, computed per-frame by CPU */
    float h_pos;                    /* horizontal raster shift (-0.2 to +0.2) */
    float v_pos;                    /* vertical raster shift (-0.2 to +0.2) */
    float h_size;                   /* horizontal raster size (0.5 - 1.5) */
    float v_size;                   /* vertical raster size (0.5 - 1.5) */

    /* Reference §3.6 / §4.8 / §5.6 / §5.9 / §6.1 / §5.1 / §5.3. */
    float phosphor_gamma_offset_r;
    float phosphor_gamma_offset_g;
    float phosphor_gamma_offset_b;
    float secondary_scatter;       /* cross-phosphor desat (§4.8) */
    float glass_reflection;        /* additional spatial glass-scatter fraction / .08 */
    float antiglare_blur;          /* matte screen sub-pixel scatter (§5.6) */
    float emi_gradient;            /* horizontal deflection brightness (§5.9) */
    float degauss_tint;            /* residual corner color offset (§6.1) */
    float phosphor_grain;          /* fixed-pattern high-freq noise (§5.1) */
    float cathode_center_dim;      /* center dimmer than rim (§5.3) */
    float cathode_gain_r;          /* per-gun aging (§5.3) */
    float cathode_gain_g;
    float cathode_gain_b;

    /* §5.2 thermal-mask doming — amplitude + per-channel slow-EMA of
     * color load (set by gpu_render, consumed in shader). */
    float thermal_dome_amount;
    float thermal_r;
    float thermal_g;
    float thermal_b;
    /* §5.4 phosphor chromaticity drive shift. */
    int phosphor_gamut;
    float chromaticity_drive_shift;
    /* §6.2 microphonic wobble amplitude + current bass RMS
     * (per-frame updated from gpu_render). */
    float microphonic_amount;
    float audio_bass_rms;
    float frame_phase;            /* radians, advances by bass freq/60 each frame */
    /* §6.3 glass-face specular/diffuse glare. */
    float glass_glare;
    float glass_glare_light_x;
    float glass_glare_light_y;
    float glass_glare_size;
    float glass_glare_temp_k;
} GPUDisplayParams;

/* Initialize the display pipeline. Loads SPIR-V shaders from shader_dir.
 * Returns true on success. */
bool gpu_display_init(GPUDisplay *d, SDL_GPUDevice *gpu,
                       SDL_Window *window, const char *shader_dir);

/* Render to an explicitly formatted target, including offscreen validation. */
bool gpu_display_init_target(GPUDisplay *d, SDL_GPUDevice *gpu,
    SDL_GPUTextureFormat format, int width, int height, const char *shader_dir);

/* Destroy the display pipeline. */
void gpu_display_destroy(GPUDisplay *d, SDL_GPUDevice *gpu);

/* Resize halation FBOs when window size changes. */
void gpu_display_resize(GPUDisplay *d, SDL_GPUDevice *gpu, int win_w, int win_h);

/* Render a frame: optional area reduction + H/V scatter, then CRT display.
 * composite_tex: beam/history light, or an explicitly gamma-encoded fallback.
 * params: CRT display uniforms for the current preset.
 * swapchain_tex: acquired from SDL_AcquireGPUSwapchainTexture.
 * cmd: the command buffer to record into.
 * viewport: if non-NULL, restricts the final CRT pass to this viewport
 *           (for 4:3 aspect ratio with letterboxing). Pass NULL for fullscreen.
 *
 * Caller must acquire the command buffer and swapchain texture before calling.
 * Caller submits the command buffer after calling. */
void gpu_display_render(GPUDisplay *d, SDL_GPUDevice *gpu,
                         SDL_GPUCommandBuffer *cmd,
                         SDL_GPUTexture *composite_tex, int comp_w, int comp_h,
                         SDL_GPUTexture *swapchain_tex, int sw, int sh,
                         const GPUDisplayParams *params,
                         const SDL_GPUViewport *viewport);

/* Fill GPUDisplayParams from a VideoChain's TVDisplayParams. */
void gpu_display_params_from_tv(GPUDisplayParams *out, const TVDisplayParams *tv,
                                 int comp_w, int comp_h, int win_w, int win_h);

/* A full-white field's scanline centres sit above its average light by
 * this factor: Gaussian lines of the given FWHM, one line apart. It is 1
 * when neighbouring lines merge into a flat field. */
float gpu_display_scanline_peak(float fwhm_lines);

/* Fit the mask to the host panel, independently of the stored CRT preset.
 * Integer RGB-triad/row periods in pixel mode trade exact CRT pitch for stability.
 * With the panel's subpixel order known (1 RGB, 2 BGR), each colour is drawn at
 * its own subpixel and a triad may be as small as one pixel; otherwise three. */
void gpu_display_fit_mask(GPUDisplayParams *p, bool pixel_aligned, int panel_subpixels,
                         float scale_x, float scale_y, float origin_x, float origin_y);

#endif /* GPU_DISPLAY_H */
