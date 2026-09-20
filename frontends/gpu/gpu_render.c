/*
 * gpu_render.c -- GPU texture management, rendering, and color conversion
 */
#include "gpu_render.h"
#include "gpu_half.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

void gpu_render_ensure_texture(GPURenderCtx *ctx, int w, int h) {
    if (ctx->display_tex && ctx->display_tex_w == w && ctx->display_tex_h == h
        && ctx->owns_display_tex)
        return;

    /* Only release textures we own (not external beam storage textures). */
    if (ctx->display_tex && ctx->owns_display_tex) {
        SDL_ReleaseGPUTexture(ctx->gpu, ctx->display_tex);
    }
    ctx->display_tex = NULL;
    ctx->owns_display_tex = true;

    SDL_GPUTextureCreateInfo ci = {0};
    ci.type = SDL_GPU_TEXTURETYPE_2D;
    ci.format = ctx->hdr_enabled
        ? SDL_GPU_TEXTUREFORMAT_R16G16B16A16_FLOAT
        : SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
    ci.width = w;
    ci.height = h;
    ci.layer_count_or_depth = 1;
    ci.num_levels = 1;
    ci.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER;

    ctx->display_tex = SDL_CreateGPUTexture(ctx->gpu, &ci);
    if (!ctx->display_tex) {
        fprintf(stderr, "Failed to create GPU texture: %s\n", SDL_GetError());
        return;
    }
    ctx->display_tex_w = w;
    ctx->display_tex_h = h;
}

void gpu_render_upload_rgba(GPURenderCtx *ctx, const uint8_t *rgba, int w, int h) {
    /* Caller always provides RGBA8 (4 bytes/pixel). Texture may be RGBA8 or
     * float16x4 depending on HDR mode — we convert on upload. */
    int bpp = ctx->hdr_enabled ? 8 : 4;
    int total_bytes = w * h * bpp;
    SDL_GPUTransferBufferCreateInfo tbci = {0};
    tbci.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
    tbci.size = total_bytes;
    SDL_GPUTransferBuffer *tbuf = SDL_CreateGPUTransferBuffer(ctx->gpu, &tbci);
    if (!tbuf) return;

    void *mapped = SDL_MapGPUTransferBuffer(ctx->gpu, tbuf, false);
    if (mapped) {
        if (ctx->hdr_enabled) {
            /* Convert each RGBA8 pixel to 4× float16, applying sRGB→linear.
             *
             * The palette (PPU framebuffer) stores sRGB-encoded values, but
             * the EDR swapchain expects linear light. Without this gamma
             * step, the passthrough blit (raw-RGB mode) hands sRGB numbers
             * to EDR as if they were linear — mid-grays come out ~2×
             * too bright. Pow 2.2 is a close match to the sRGB EOTF.
             *
             * Build a 256-entry LUT once per upload so we only call powf
             * 256 times, not 256×240 times. Uses __fp16 (native on ARM64,
             * supported by clang everywhere else we build). */
            _Float16 lut[256];
            for (int i = 0; i < 256; i++) {
                lut[i] = (_Float16)powf(i * (1.0f / 255.0f), 2.2f);
            }
            _Float16 *dst = (_Float16 *)mapped;
            int n = w * h;
            for (int i = 0; i < n; i++) {
                dst[i*4 + 0] = lut[rgba[i*4 + 0]];
                dst[i*4 + 1] = lut[rgba[i*4 + 1]];
                dst[i*4 + 2] = lut[rgba[i*4 + 2]];
                dst[i*4 + 3] = (_Float16)1.0f;
            }
        } else {
            memcpy(mapped, rgba, total_bytes);
        }
        SDL_UnmapGPUTransferBuffer(ctx->gpu, tbuf);
    }

    SDL_GPUCommandBuffer *cmd = SDL_AcquireGPUCommandBuffer(ctx->gpu);
    if (cmd) {
        SDL_GPUCopyPass *copy = SDL_BeginGPUCopyPass(cmd);
        if (copy) {
            SDL_GPUTextureTransferInfo si = {0};
            si.transfer_buffer = tbuf;
            SDL_GPUTextureRegion dr = {0};
            dr.texture = ctx->display_tex;
            dr.w = w;
            dr.h = h;
            dr.d = 1;
            SDL_UploadToGPUTexture(copy, &si, &dr, false);
            SDL_EndGPUCopyPass(copy);
        }
        SDL_SubmitGPUCommandBuffer(cmd);
    }

    SDL_ReleaseGPUTransferBuffer(ctx->gpu, tbuf);
}

/* Upload a 256x240 RGB888 (PPU framebuffer) to a GPU texture.
 * Used for the split-view raw palette overlay. */
static void upload_raw_ppu_to_tex(GPURenderCtx *ctx, SDL_GPUCommandBuffer *cmd,
                                   SDL_GPUTexture *tex, const uint8_t *rgb888) {
    const int w = 256, h = 240, bytes = w * h * 4;
    SDL_GPUTransferBufferCreateInfo tbci = {0};
    tbci.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
    tbci.size = bytes;
    SDL_GPUTransferBuffer *tbuf = SDL_CreateGPUTransferBuffer(ctx->gpu, &tbci);
    if (!tbuf) return;
    uint8_t *mapped = (uint8_t *)SDL_MapGPUTransferBuffer(ctx->gpu, tbuf, false);
    if (mapped) {
        for (int i = 0; i < w * h; i++) {
            mapped[i*4+0] = rgb888[i*3+0];
            mapped[i*4+1] = rgb888[i*3+1];
            mapped[i*4+2] = rgb888[i*3+2];
            mapped[i*4+3] = 0xFF;
        }
        SDL_UnmapGPUTransferBuffer(ctx->gpu, tbuf);
    }
    SDL_GPUCopyPass *copy = SDL_BeginGPUCopyPass(cmd);
    if (copy) {
        SDL_GPUTextureTransferInfo si = { .transfer_buffer = tbuf };
        SDL_GPUTextureRegion dr = { .texture = tex, .w = w, .h = h, .d = 1 };
        SDL_UploadToGPUTexture(copy, &si, &dr, false);
        SDL_EndGPUCopyPass(copy);
    }
    SDL_ReleaseGPUTransferBuffer(ctx->gpu, tbuf);
}

/* Lazily create the raw palette texture for split-view. */
static void ensure_raw_tex(GPURenderCtx *ctx) {
    if (ctx->raw_tex) return;
    SDL_GPUTextureCreateInfo ci = {0};
    ci.type = SDL_GPU_TEXTURETYPE_2D;
    ci.format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
    ci.width = 256;
    ci.height = 240;
    ci.layer_count_or_depth = 1;
    ci.num_levels = 1;
    ci.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER;
    ctx->raw_tex = SDL_CreateGPUTexture(ctx->gpu, &ci);
}

void gpu_render_update_dynamic_state(GPURenderCtx *ctx) {
    if (!ctx) return;

    /* HV sag time constant: exponential smoothing of frame brightness.
     * Discharge (expansion) is faster than recharge (recovery). */
    float target = ctx->frame_brightness;
    float k = (target > ctx->hv_sag_state) ? 0.15f : 0.055f;
    ctx->hv_sag_state += (target - ctx->hv_sag_state) * k;

    /* §4.9 APL black-level tracker — slower than the HV rail. */
    const float apl_k = 0.12f;  /* ~80ms @ 60fps */
    ctx->apl_slow_state += (target - ctx->apl_slow_state) * apl_k;

    /* §5.2 thermal-mask tracker — ~15 second EMA per channel. */
    if (ctx->raw_ppu_rgb) {
        float fr, fg, fb;
        gpu_render_compute_rgb_averages(ctx->raw_ppu_rgb, &fr, &fg, &fb);
        const float thermal_k = 1.0f / (60.0f * 15.0f);
        ctx->thermal_r_state += (fr - ctx->thermal_r_state) * thermal_k;
        ctx->thermal_g_state += (fg - ctx->thermal_g_state) * thermal_k;
        ctx->thermal_b_state += (fb - ctx->thermal_b_state) * thermal_k;
    }

    /* Shared frame counter for oscillatory display-domain effects. */
    ctx->frame_counter++;
}

/* Opt-in capture of our final CRT render, independent of OS screen-recording
 * permissions. PPM is an SDR preview: EDR values above reference white clip. */
static void capture_display(GPURenderCtx *ctx, const GPUDisplayParams *params,
                            int w, int h, const char *path) {
    SDL_GPUTextureCreateInfo ci = {0};
    ci.type=SDL_GPU_TEXTURETYPE_2D;
    ci.format=SDL_GetGPUSwapchainTextureFormat(ctx->gpu,ctx->window);
    ci.usage=SDL_GPU_TEXTUREUSAGE_COLOR_TARGET;
    ci.width=w; ci.height=h; ci.layer_count_or_depth=1; ci.num_levels=1;
    SDL_GPUTexture *target=SDL_CreateGPUTexture(ctx->gpu,&ci);
    int bpp=ctx->hdr_enabled ? 8 : 4;
    SDL_GPUTransferBufferCreateInfo bi={.usage=SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD,.size=(Uint32)(w*h*bpp)};
    SDL_GPUTransferBuffer *tb=SDL_CreateGPUTransferBuffer(ctx->gpu,&bi);
    if(target && tb) {
        SDL_GPUCommandBuffer *cmd=SDL_AcquireGPUCommandBuffer(ctx->gpu);
        gpu_display_render(ctx->gpu_disp,ctx->gpu,cmd,ctx->display_tex,
            ctx->display_tex_w,ctx->display_tex_h,target,w,h,params,NULL);
        SDL_GPUCopyPass *copy=SDL_BeginGPUCopyPass(cmd);
        SDL_GPUTextureRegion region={.texture=target,.w=w,.h=h,.d=1};
        SDL_GPUTextureTransferInfo dst={.transfer_buffer=tb,.pixels_per_row=w,.rows_per_layer=h};
        SDL_DownloadFromGPUTexture(copy,&region,&dst); SDL_EndGPUCopyPass(copy);
        SDL_SubmitGPUCommandBuffer(cmd); SDL_WaitForGPUIdle(ctx->gpu);
        const void *data=SDL_MapGPUTransferBuffer(ctx->gpu,tb,false);
        FILE *f=data ? fopen(path,"wb") : NULL;
        if(f) {
            fprintf(f,"P6\n%d %d\n255\n",w,h);
            for(int i=0;i<w*h;i++) {
                uint8_t rgb[3];
                for(int c=0;c<3;c++) {
                    if(ctx->hdr_enabled) {
                        float v=gpu_half_to_float(((const uint16_t *)data)[i*4+c])/params->sdr_white_level;
                        v=fminf(fmaxf(v,0),1);
                        v=v<=0.0031308f ? 12.92f*v : 1.055f*powf(v,1.0f/2.4f)-0.055f;
                        rgb[c]=(uint8_t)lrintf(v*255);
                    } else {
                        int channel=(ci.format==SDL_GPU_TEXTUREFORMAT_B8G8R8A8_UNORM) ? 2-c : c;
                        rgb[c]=((const uint8_t *)data)[i*4+channel];
                    }
                }
                fwrite(rgb,1,3,f);
            }
            fclose(f); fprintf(stderr,"Final CRT capture: %s\n",path);
        }
        if(data) SDL_UnmapGPUTransferBuffer(ctx->gpu,tb);
    }
    if(tb) SDL_ReleaseGPUTransferBuffer(ctx->gpu,tb);
    if(target) SDL_ReleaseGPUTexture(ctx->gpu,target);
}

void gpu_render_frame(GPURenderCtx *ctx, const VideoChain *chain) {
    SDL_GPUCommandBuffer *cmd = SDL_AcquireGPUCommandBuffer(ctx->gpu);
    if (!cmd) return;

    GPUDisplayParams capture_params = {0};
    bool can_capture = false;
    SDL_GPUTexture *swapchain_tex = NULL;
    Uint32 sw, sh;
    if (!SDL_WaitAndAcquireGPUSwapchainTexture(cmd, ctx->window, &swapchain_tex, &sw, &sh)) {
        SDL_SubmitGPUCommandBuffer(cmd);
        return;
    }
    if (!swapchain_tex) {
        SDL_SubmitGPUCommandBuffer(cmd);
        return;
    }

    /* Split view: upload raw PPU frame to raw_tex so we can blit it later. */
    if (ctx->split_mode && ctx->raw_ppu_rgb) {
        ensure_raw_tex(ctx);
        if (ctx->raw_tex)
            upload_raw_ppu_to_tex(ctx, cmd, ctx->raw_tex, ctx->raw_ppu_rgb);
    }

    /* Compute 4:3 letterboxed/pillarboxed viewport within the window. */
    const float target_aspect = 4.0f / 3.0f;
    float win_aspect = (float)sw / (float)sh;
    float vp_x, vp_y, vp_w, vp_h;
    if (win_aspect > target_aspect) {
        /* Window wider than 4:3 — pillarbox (black bars on sides). */
        vp_h = (float)sh;
        vp_w = vp_h * target_aspect;
        vp_x = ((float)sw - vp_w) * 0.5f;
        vp_y = 0.0f;
    } else {
        /* Window taller than 4:3 — letterbox (black bars top/bottom). */
        vp_w = (float)sw;
        vp_h = vp_w / target_aspect;
        vp_x = 0.0f;
        vp_y = ((float)sh - vp_h) * 0.5f;
    }

    if (ctx->display_tex && ctx->gpu_display_enabled && (ctx->crt_shader_enabled || !ctx->owns_display_tex)) {
        /* CRT shader: 3-pass render pipeline. */
        GPUDisplayParams disp_params;
        gpu_display_params_from_tv(&disp_params, &chain->tv,
                                   ctx->display_tex_w, ctx->display_tex_h,
                                   (int)vp_w, (int)vp_h);
        if (!ctx->crt_shader_enabled) {
            memset(&disp_params, 0, sizeof(disp_params));
            disp_params.glass_tint = 1;
            disp_params.hdr_gain = 1;
        }
        disp_params.frame_brightness = ctx->hv_sag_state;
        disp_params.apl_smoothed = ctx->apl_slow_state;
        disp_params.thermal_r = ctx->thermal_r_state;
        disp_params.thermal_g = ctx->thermal_g_state;
        disp_params.thermal_b = ctx->thermal_b_state;

        disp_params.audio_bass_rms = ctx->audio_bass_rms;
        disp_params.frame_phase = (float)ctx->frame_counter
                                  * (80.0f * 2.0f * 3.14159265f / 60.0f);
        disp_params.input_gamma = (!ctx->owns_display_tex || ctx->hdr_enabled) ? 0.0f : chain->tv.gamma;
        disp_params.output_hdr = ctx->hdr_enabled;
        SDL_PropertiesID props = SDL_GetWindowProperties(ctx->window);
        disp_params.hdr_headroom = ctx->hdr_enabled
            ? fmaxf(1.0f, SDL_GetFloatProperty(props, SDL_PROP_WINDOW_HDR_HEADROOM_FLOAT, 1.0f)) : 1.0f;
        disp_params.sdr_white_level = ctx->hdr_enabled
            ? SDL_GetFloatProperty(props, SDL_PROP_WINDOW_SDR_WHITE_LEVEL_FLOAT, 1.0f) : 1.0f;
        capture_params = disp_params;
        can_capture = true;
        SDL_GPUViewport viewport = {0};
        viewport.x = vp_x;
        viewport.y = vp_y;
        viewport.w = vp_w;
        viewport.h = vp_h;
        viewport.min_depth = 0.0f;
        viewport.max_depth = 1.0f;
        gpu_display_render(ctx->gpu_disp, ctx->gpu, cmd,
                           ctx->display_tex, ctx->display_tex_w, ctx->display_tex_h,
                           swapchain_tex, (int)sw, (int)sh,
                           &disp_params, &viewport);
    } else if (ctx->display_tex) {
        /* Passthrough blit with 4:3 aspect ratio.
         * LOADOP_CLEAR fills the entire destination with black before the blit,
         * giving us letterbox/pillarbox bars for free. */
        SDL_GPUBlitInfo blit = {0};
        blit.source.texture = ctx->display_tex;
        blit.source.w = ctx->display_tex_w;
        blit.source.h = ctx->display_tex_h;
        blit.destination.texture = swapchain_tex;
        blit.destination.x = (Uint32)vp_x;
        blit.destination.y = (Uint32)vp_y;
        blit.destination.w = (Uint32)vp_w;
        blit.destination.h = (Uint32)vp_h;
        blit.load_op = SDL_GPU_LOADOP_CLEAR;
        blit.clear_color.r = 0.0f;
        blit.clear_color.g = 0.0f;
        blit.clear_color.b = 0.0f;
        blit.clear_color.a = 1.0f;
        blit.filter = SDL_GPU_FILTER_NEAREST;  /* pixel-perfect, no blur */
        SDL_BlitGPUTexture(cmd, &blit);
    }

    /* Split view: overwrite right half of swapchain with raw PPU palette.
     * This gives an A/B comparison against the CRT-processed left half. */
    if (ctx->split_mode && ctx->raw_tex) {
        SDL_GPUBlitInfo split = {0};
        split.source.texture = ctx->raw_tex;
        /* Source: right half of 256x240 PPU framebuffer. */
        split.source.x = 128;
        split.source.y = 0;
        split.source.w = 128;
        split.source.h = 240;
        /* Destination: right half of swapchain viewport (use 4:3 pillarbox
         * bounds matching the main render so it aligns visually). */
        split.destination.texture = swapchain_tex;
        split.destination.x = (Uint32)(vp_x + vp_w * 0.5f);
        split.destination.y = (Uint32)vp_y;
        split.destination.w = (Uint32)(vp_w * 0.5f);
        split.destination.h = (Uint32)vp_h;
        split.load_op = SDL_GPU_LOADOP_LOAD;  /* don't clear, preserve left half */
        split.filter = SDL_GPU_FILTER_NEAREST;
        SDL_BlitGPUTexture(cmd, &split);
    }

    SDL_SubmitGPUCommandBuffer(cmd);
    const char *capture_path=SDL_getenv("MYNES_CAPTURE_PATH");
    static bool captured=false;
    if(can_capture && !captured && capture_path && ctx->frame_counter>=180) {
        captured=true;
        capture_display(ctx,&capture_params,(int)vp_w,(int)vp_h,capture_path);
    }
}

uint8_t *gpu_render_float_rgb_to_rgba8(const float *rgb, int pixel_count) {
    static uint8_t *buf = NULL;
    static int buf_size = 0;
    int needed = pixel_count * 4;
    if (needed > buf_size) {
        free(buf);
        buf = (uint8_t *)malloc(needed);
        buf_size = needed;
    }
    for (int i = 0; i < pixel_count; i++) {
        int r = (int)(rgb[i*3+0] * 255.0f);
        int g = (int)(rgb[i*3+1] * 255.0f);
        int b = (int)(rgb[i*3+2] * 255.0f);
        if (r < 0) r = 0; if (r > 255) r = 255;
        if (g < 0) g = 0; if (g > 255) g = 255;
        if (b < 0) b = 0; if (b > 255) b = 255;
        buf[i*4+0] = (uint8_t)r;
        buf[i*4+1] = (uint8_t)g;
        buf[i*4+2] = (uint8_t)b;
        buf[i*4+3] = 0xFF;
    }
    return buf;
}

uint8_t *gpu_render_ppu_to_rgba8(const uint8_t *rgb888) {
    static uint8_t rgba[256 * 240 * 4];
    for (int i = 0; i < 256 * 240; i++) {
        rgba[i*4+0] = rgb888[i*3+0];
        rgba[i*4+1] = rgb888[i*3+1];
        rgba[i*4+2] = rgb888[i*3+2];
        rgba[i*4+3] = 0xFF;
    }
    return rgba;
}

/* Compute average luminance of the PPU framebuffer (256×240 RGB888).
 * Sparse sampling (every 16th pixel) keeps this cheap — ~3840 samples per frame.
 * Returns [0, 1]. */
float gpu_render_compute_frame_brightness(const uint8_t *rgb888) {
    if (!rgb888) return 0.0f;
    const int stride = 16;
    unsigned long sum = 0;
    int count = 0;
    for (int i = 0; i < 256 * 240; i += stride) {
        unsigned r = rgb888[i*3+0];
        unsigned g = rgb888[i*3+1];
        unsigned b = rgb888[i*3+2];
        /* BT.601 luma × 256 for integer math. */
        sum += 77u * r + 150u * g + 29u * b;
        count++;
    }
    return (float)sum / (float)(count * 256 * 255);
}

/* Per-channel average intensity for the thermal tracker — sparse
 * sample the PPU buffer to cheaply estimate how hard each gun is
 * driven this frame. Output in [0,1] per channel. */
void gpu_render_compute_rgb_averages(const uint8_t *rgb888,
                                      float *out_r, float *out_g, float *out_b) {
    if (!rgb888 || !out_r || !out_g || !out_b) return;
    const int stride = 16;
    unsigned long sr = 0, sg = 0, sb = 0;
    int count = 0;
    for (int i = 0; i < 256 * 240; i += stride) {
        sr += rgb888[i*3+0];
        sg += rgb888[i*3+1];
        sb += rgb888[i*3+2];
        count++;
    }
    float inv = 1.0f / (float)(count * 255);
    *out_r = (float)sr * inv;
    *out_g = (float)sg * inv;
    *out_b = (float)sb * inv;
}
