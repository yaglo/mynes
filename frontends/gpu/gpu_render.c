/*
 * gpu_render.c -- GPU texture management, rendering, and color conversion
 */
#include "gpu_render.h"
#include "frame_capture.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

/* What the mask/glass bypass and the subpixel lab's reference half leave
 * out: the mask (and with glass, the faceplate) while phosphor colour,
 * beam, geometry, gain and shoulder stay. */
static void strip_mask(GPUDisplayParams *p, bool glass) {
    p->mask_strength = 0; p->damper_wires = 0; p->panel_subpixels = 0;
    if (!glass) return;
    p->halation_strength = 0; p->glass_reflection = 0; p->glass_glare = 0;
    p->antiglare_blur = 0; p->ambient_light = 0; p->vignette = 0;
    p->phosphor_grain = 0; p->glass_tint = 1;
}

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
             * too bright. Use the piecewise sRGB EOTF, including its toe.
             *
             * Build a 256-entry LUT once per upload so we only call powf
             * 256 times, not 256×240 times. Uses __fp16 (native on ARM64,
             * supported by clang everywhere else we build). */
            _Float16 lut[256];
            for (int i = 0; i < 256; i++) {
                float v=i/255.0f;
                lut[i] = (_Float16)(v<=.04045f ? v/12.92f : powf((v+.055f)/1.055f,2.4f));
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
     * Load rises faster than it recovers; geometry response has an explicit sign. */
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

/* Hidden playback renders into a target of its own, in the swapchain's
 * format unless main chose one (an HDR recording needs half floats). */
static SDL_GPUTextureFormat final_format(const GPURenderCtx *ctx) {
    if (ctx->offscreen_w && ctx->offscreen_format != SDL_GPU_TEXTUREFORMAT_INVALID)
        return ctx->offscreen_format;
    return SDL_GetGPUSwapchainTextureFormat(ctx->gpu, ctx->window);
}

/* Opt-in capture of our final CRT render, independent of OS screen-recording
 * permissions. PPM is an SDR preview: EDR values above reference white clip.
 * A NULL path hands the image to the capture sink only. */
static bool capture_display(GPURenderCtx *ctx, const GPUDisplayParams *params,
                            int w, int h, const SDL_GPUViewport *viewport, const char *path) {
    // At most one owned image/writer: a slow disk cannot grow a queue.
    if (!frame_capture_finish(ctx->capture_job)) ctx->capture_failed=true;
    ctx->capture_job=NULL;
    bool saved = false;
    SDL_GPUTextureCreateInfo ci = {0};
    ci.type=SDL_GPU_TEXTURETYPE_2D;
    ci.format=final_format(ctx);
    ci.usage=SDL_GPU_TEXTUREUSAGE_COLOR_TARGET;
    ci.width=w; ci.height=h; ci.layer_count_or_depth=1; ci.num_levels=1;
    /* Hidden playback already owns its final target. Read that exact frame
     * instead of rendering the glass/mask a second time just for capture. */
    bool owns_target=ctx->offscreen_target==NULL;
    SDL_GPUTexture *target=owns_target ? SDL_CreateGPUTexture(ctx->gpu,&ci) : ctx->offscreen_target;
    int bpp=ctx->hdr_enabled ? 8 : 4;
    SDL_GPUTransferBufferCreateInfo bi={.usage=SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD,.size=(Uint32)(w*h*bpp)};
    SDL_GPUTransferBuffer *tb=SDL_CreateGPUTransferBuffer(ctx->gpu,&bi);
    if(target && tb) {
        SDL_GPUCommandBuffer *cmd=SDL_AcquireGPUCommandBuffer(ctx->gpu);
        if (!cmd) goto done;
        /* Files are sRGB; only the window's layer takes P3. */
        GPUDisplayParams file_params=*params; file_params.output_p3=0;
        if (owns_target)
            gpu_display_render(ctx->gpu_disp,ctx->gpu,cmd,ctx->display_tex,
                ctx->display_tex_w,ctx->display_tex_h,target,w,h,&file_params,viewport);
        SDL_GPUCopyPass *copy=SDL_BeginGPUCopyPass(cmd);
        if (!copy) { SDL_CancelGPUCommandBuffer(cmd); goto done; }
        SDL_GPUTextureRegion region={.texture=target,.w=w,.h=h,.d=1};
        SDL_GPUTextureTransferInfo dst={.transfer_buffer=tb,.pixels_per_row=w,.rows_per_layer=h};
        SDL_DownloadFromGPUTexture(copy,&region,&dst); SDL_EndGPUCopyPass(copy);
        SDL_GPUFence *fence=SDL_SubmitGPUCommandBufferAndAcquireFence(cmd);
        bool ready=fence && SDL_WaitForGPUFences(ctx->gpu,true,&fence,1);
        if(fence) SDL_ReleaseGPUFence(ctx->gpu,fence);
        const void *data=ready ? SDL_MapGPUTransferBuffer(ctx->gpu,tb,false) : NULL;
        if (data) {
            FrameCaptureImage image = {.pixels=data, .width=w, .height=h,
                .hdr=ctx->hdr_enabled, .bgra=ci.format==SDL_GPU_TEXTUREFORMAT_B8G8R8A8_UNORM,
                .white_level=params->sdr_white_level};
            /* The sink reads the mapped download directly; a file writer
             * copies it, so both can take the same frame. */
            bool delivered = !ctx->capture_sink || ctx->capture_sink(ctx->capture_sink_user, &image);
            if (path) {
                if (ctx->capture_async) {
                    ctx->capture_job=frame_capture_start(&image,path);
                    saved=ctx->capture_job!=NULL;
                }
                // Allocation/thread failure falls back to a synchronous write.
                if (!saved) saved=frame_capture_write(&image,path);
            } else saved=true;
            saved=saved && delivered;
        }
        if(data) SDL_UnmapGPUTransferBuffer(ctx->gpu,tb);
    }
done:
    if(tb) SDL_ReleaseGPUTransferBuffer(ctx->gpu,tb);
    if(target && owns_target) SDL_ReleaseGPUTexture(ctx->gpu,target);
    return saved;
}

float gpu_render_headroom(const GPURenderCtx *ctx) {
    if (!ctx->hdr_enabled) return 1;
    if (ctx->offscreen_w) return ctx->offscreen_headroom;
    return fmaxf(1, SDL_GetFloatProperty(SDL_GetWindowProperties(ctx->window),
        SDL_PROP_WINDOW_HDR_HEADROOM_FLOAT, 1));
}

void gpu_render_presentation_update(GPURenderCtx *ctx, float source_hz) {
    SDL_DisplayID display = SDL_GetDisplayForWindow(ctx->window);
    const SDL_DisplayMode *mode = SDL_GetCurrentDisplayMode(display);
    float hz = mode ? mode->refresh_rate : 0;
    if (ctx->presentation_last_mode != ctx->presentation_mode ||
        ctx->presentation_display != display || ctx->presentation_hz != hz ||
        ctx->presentation_source_hz != source_hz) {
        ctx->presentation_blocked = false;
        ctx->presentation_slot = ctx->cadence_samples = 0;
        ctx->cadence_start_ns = 0;
        ctx->pacing_deadline_ns = 0;
        ctx->presentation_epoch++;
        ctx->presentation_last_mode = ctx->presentation_mode;
        ctx->presentation_display = display;
        ctx->presentation_hz = hz;
        ctx->presentation_source_hz = source_hz;
        if (ctx->presentation_mode == GPU_PRESENT_BFI)
            fprintf(stderr, "BFI: display %.3f Hz, source %.4f Hz, %d refreshes/frame%s\n",
                hz, source_hz, gpu_presentation_slots(hz, source_hz),
                ctx->offscreen_w ? " (disabled offscreen)" : "");
    }
    ctx->presentation_slots = ctx->presentation_mode == GPU_PRESENT_BFI && !ctx->offscreen_w &&
        !ctx->presentation_blocked && ctx->gpu_display_enabled && ctx->crt_shader_enabled &&
        !ctx->split_mode ? gpu_presentation_slots(hz, source_hz) : 1;
    if (ctx->presentation_slots == 1) ctx->presentation_slot = 0;
    ctx->vsync_paced = !ctx->offscreen_w &&
        gpu_presentation_vsync_paced(ctx->presentation_mode, hz, source_hz);
#ifdef MYNES_BUNDLED_SDL3
    /* The bundled Metal backend schedules scanout, rather than delaying CPU
     * submission until a deadline the GPU can then miss. */
    ctx->scheduled_present = !ctx->offscreen_w && !ctx->vsync_paced &&
        ctx->presentation_mode != GPU_PRESENT_BFI && source_hz > 0 &&
        strcmp(SDL_GetGPUDeviceDriver(ctx->gpu), "metal") == 0;
    uint64_t interval = source_hz > 0 ? gpu_presentation_period_ns(ctx->presentation_mode,
        (uint64_t)llround(1e9 / source_hz)) : 0;
    SDL_SetNumberProperty(SDL_GetWindowProperties(ctx->window),
        "mynes.gpu.metal.present_interval_ns",
        ctx->scheduled_present ? interval : 0);
    SDL_SetNumberProperty(SDL_GetWindowProperties(ctx->window),
        "mynes.gpu.metal.present_epoch", ctx->presentation_epoch);
#endif
    /* SDL flushes the queue here: never reconfigure while holding an acquired
     * drawable waiting for a source picture. Apply at the next free boundary. */
    int in_flight = ctx->vsync_paced || ctx->presentation_slots > 1 ||
        (ctx->presentation_mode == GPU_PRESENT_60HZ && !ctx->scheduled_present) ? 1 : 2;
    if (!ctx->present_cmd && ctx->frames_in_flight != in_flight &&
        SDL_SetGPUAllowedFramesInFlight(ctx->gpu, in_flight))
        ctx->frames_in_flight = in_flight;
}

/* Polled from the main loop, never waited for. A fence still pending when
 * its successor is submitted has already cost at least the elapsed time, so
 * that lower bound is recorded rather than losing the overloaded sample. */
static void poll_frame_fence(GPURenderCtx *ctx, bool replace) {
    if (!ctx->frame_fence) return;
    if (!replace && !SDL_QueryGPUFence(ctx->gpu, ctx->frame_fence)) return;
    ctx->gpu_frame_total_ns += SDL_GetTicksNS() - ctx->frame_fence_submit_ns;
    ctx->gpu_frame_samples++;
    SDL_ReleaseGPUFence(ctx->gpu, ctx->frame_fence);
    ctx->frame_fence = NULL;
}

void gpu_render_poll_frame_fence(GPURenderCtx *ctx) {
    poll_frame_fence(ctx, false);
}

bool gpu_render_prepare(GPURenderCtx *ctx) {
    poll_frame_fence(ctx, false);
    if(ctx->present_cmd) return true;
    ctx->swap_wait_ns=0;
    if(ctx->offscreen_w) {
        Uint64 start=SDL_GetTicksNS();
        if(ctx->offscreen_fence) {
            bool ready=SDL_WaitForGPUFences(ctx->gpu,true,&ctx->offscreen_fence,1);
            SDL_ReleaseGPUFence(ctx->gpu,ctx->offscreen_fence);ctx->offscreen_fence=NULL;
            if(!ready) return false;
        }
        ctx->swap_wait_ns=SDL_GetTicksNS()-start;
        if(!ctx->offscreen_target) {
            SDL_GPUTextureCreateInfo ti={.type=SDL_GPU_TEXTURETYPE_2D,
                .format=final_format(ctx),
                .usage=SDL_GPU_TEXTUREUSAGE_COLOR_TARGET,.width=ctx->offscreen_w,
                .height=ctx->offscreen_h,.layer_count_or_depth=1,.num_levels=1};
            ctx->offscreen_target=SDL_CreateGPUTexture(ctx->gpu,&ti);
            if(!ctx->offscreen_target) return false;
        }
        ctx->present_cmd=SDL_AcquireGPUCommandBuffer(ctx->gpu);
        ctx->present_texture=ctx->offscreen_target;
        ctx->present_w=ctx->offscreen_w;ctx->present_h=ctx->offscreen_h;
        return ctx->present_cmd!=NULL;
    }
    SDL_GPUCommandBuffer *cmd=SDL_AcquireGPUCommandBuffer(ctx->gpu);
    if(!cmd) return false;
    Uint64 wait_start=SDL_GetTicksNS();
    /* Poll frame capacity so a slow fence cannot trap the event loop in
     * waitUntilCompleted. Cancel empty attempts to keep command buffers bounded.
     * The platform's drawable acquisition may still wait for vblank. */
    bool acquired=SDL_AcquireGPUSwapchainTexture(cmd,ctx->window,
        &ctx->present_texture,&ctx->present_w,&ctx->present_h);
    ctx->swap_wait_ns=SDL_GetTicksNS()-wait_start;
    if(!acquired || !ctx->present_texture) {
        SDL_CancelGPUCommandBuffer(cmd);
        return false;
    }
    ctx->present_cmd=cmd;
    return true;
}

bool gpu_render_release_pending(GPURenderCtx *ctx) {
    if (!frame_capture_finish(ctx->capture_job)) ctx->capture_failed=true;
    ctx->capture_job=NULL;
    // SDL forbids cancelling a command buffer after acquiring a drawable.
    if(ctx->present_cmd) SDL_SubmitGPUCommandBuffer(ctx->present_cmd);
    ctx->present_cmd=NULL;ctx->present_texture=NULL;
    if(ctx->offscreen_fence) {
        SDL_WaitForGPUFences(ctx->gpu,true,&ctx->offscreen_fence,1);
        SDL_ReleaseGPUFence(ctx->gpu,ctx->offscreen_fence);ctx->offscreen_fence=NULL;
    }
    poll_frame_fence(ctx, true);
    if(ctx->offscreen_target) SDL_ReleaseGPUTexture(ctx->gpu,ctx->offscreen_target);
    ctx->offscreen_target=NULL;
    return !ctx->capture_failed;
}

void gpu_render_frame(GPURenderCtx *ctx, const VideoChain *chain) {
    ctx->capture_accepted = false;
    ctx->submit_ns=ctx->capture_ns=0;
    if(!gpu_render_prepare(ctx)) return;
    SDL_GPUCommandBuffer *cmd=ctx->present_cmd;
    SDL_GPUTexture *swapchain_tex=ctx->present_texture;
    Uint32 sw=ctx->present_w,sh=ctx->present_h;
    ctx->present_cmd=NULL;ctx->present_texture=NULL;
    GPUDisplayParams capture_params = {0};
    bool can_capture = false;

    /* Split view: upload raw PPU frame to raw_tex so we can blit it later. */
    if (ctx->split_mode && ctx->raw_ppu_rgb) {
        ensure_raw_tex(ctx);
        if (ctx->raw_tex)
            upload_raw_ppu_to_tex(ctx, cmd, ctx->raw_tex, ctx->raw_ppu_rgb);
    }

    /* Fit the tube face: 4:3 TV or 16:10 FW900 with internal 4:3 scaling,
     * pillarboxed or letterboxed. Fullscreen fits it below a notched panel's
     * camera housing. Integral viewport edges avoid fractional raster
     * resampling. The shader places the mask by gl_FragCoord, so a viewport
     * moved down by the inset keeps the mask on the same output pixels. */
    const float target_aspect = ctx->crt_shader_enabled && chain->tv.monitor_model==1 ? 16.0f/10.0f : 4.0f/3.0f;
    SDL_Rect safe=ctx->offscreen_w ? (SDL_Rect){0,0,(int)sw,(int)sh}
                                   : gpu_output_safe_area(ctx->window,(int)sw,(int)sh);
    SDL_FRect picture=gpu_output_fit_picture(safe,target_aspect);
    float vp_x=picture.x, vp_y=picture.y, vp_w=picture.w, vp_h=picture.h;
    bool size_changed=ctx->drawable_w!=(int)sw || ctx->drawable_h!=(int)sh ||
        memcmp(&ctx->safe_area,&safe,sizeof(safe))!=0;
    ctx->drawable_w=sw;ctx->drawable_h=sh;ctx->safe_area=safe;
    if(ctx->offscreen_w) {
        ctx->output_geometry=(GPUOutputGeometry){.scale_x=1,.scale_y=1,
            .native_w=(int)sw,.native_h=(int)sh,.native_known=true};
    } else gpu_output_geometry(ctx->window,&ctx->output_geometry);
    if(size_changed) fprintf(stderr,"Output: drawable %ux%u, safe area %dx%d at %d,%d, picture %.0fx%.0f at %.0f,%.0f, "
        "panel %dx%d%s, panel/drawable %.6fx%.6f, mask %s\n",
        sw,sh,safe.w,safe.h,safe.x,safe.y,vp_w,vp_h,vp_x,vp_y,
        ctx->output_geometry.native_w,ctx->output_geometry.native_h,
        ctx->output_geometry.native_known ? "" : " (unknown)",
        ctx->output_geometry.scale_x,ctx->output_geometry.scale_y,
        ctx->mask_alignment ? "physical CRT pitch" : "integer panel periods");

    if (ctx->display_tex && ctx->gpu_display_enabled && (ctx->crt_shader_enabled || !ctx->owns_display_tex)) {
        /* Beam light, glass scattering and final host-display adaptation. */
        GPUDisplayParams disp_params = {0};
        gpu_display_params_from_tv(&disp_params, &chain->tv,
                                   ctx->display_tex_w, ctx->display_tex_h,
                                   (int)vp_w, (int)vp_h);
        /* Gate external room light without changing the preset or the tube's
         * internal emission/scatter. Also applies to captures and dark slots. */
        if (!ctx->room_reflections_enabled) {
            disp_params.glass_glare = 0;
            disp_params.ambient_light = 0;
        }
        const GPUOutputGeometry *panel=&ctx->output_geometry;
        gpu_display_fit_mask(&disp_params,ctx->mask_alignment==0,ctx->panel_subpixels,
            panel->scale_x,panel->scale_y,panel->origin_x,panel->origin_y);
        ctx->effective_panel_subpixels=disp_params.panel_subpixels;
        ctx->effective_mask_triads=vp_w*panel->scale_x/(3*disp_params.mask_pitch_px);
        if (!ctx->crt_shader_enabled) {
            memset(&disp_params, 0, sizeof(disp_params));
            disp_params.glass_tint = 1;
            disp_params.hdr_gain = 1;
        }
        disp_params.pulse_enabled = ctx->presentation_slots > 1;
        disp_params.pulse_gain = gpu_presentation_gain(ctx->presentation_slots,
            ctx->presentation_slot, ctx->dark_frame_level);
        disp_params.reuse_halation = ctx->presentation_slot > 0;
        disp_params.frame_brightness = ctx->hv_sag_state;
        disp_params.thermal_r = ctx->thermal_r_state;
        disp_params.thermal_g = ctx->thermal_g_state;
        disp_params.thermal_b = ctx->thermal_b_state;

        disp_params.audio_bass_rms = ctx->audio_bass_rms;
        disp_params.frame_phase = (float)ctx->frame_counter
                                  * (80.0f * 2.0f * 3.14159265f / 60.0f);
        disp_params.input_gamma = (!ctx->owns_display_tex || ctx->hdr_enabled) ? 0.0f : chain->tv.gamma;
        disp_params.output_hdr = ctx->hdr_enabled;
        SDL_PropertiesID props = SDL_GetWindowProperties(ctx->window);
        disp_params.hdr_headroom = gpu_render_headroom(ctx);
        /* Auto HDR gain puts the brightest phosphor of a full-white field,
         * a stripe centre on a scanline centre, at 95% of the display's peak
         * and starts the output shoulder there, so white keeps its line and
         * stripe shape at the most light the panel has. Squeezing brighter
         * peaks into the panel flattens them: lines measure taller and the
         * gaps fill. The gain may fall below 1; a CRT's white is dimmer
         * than the panel's. At most 4x. Offscreen HDR captures fit the
         * headroom they are given, so they stay reproducible. */
        if (ctx->crt_shader_enabled && ctx->hdr_enabled && ctx->hdr_gain_mode == 0) {
            disp_params.shoulder_knee = 0.95f;
            float fit = disp_params.shoulder_knee * disp_params.hdr_headroom
                      / gpu_display_white_peak(&disp_params, chain->tv.beam_fwhm_max);
            /* Auto boost is the user's choice to spend the shoulder on
             * brightness: above 1 the stripe centres pass the knee. */
            disp_params.hdr_gain = fminf(fit * fmaxf(ctx->hdr_boost, 1.0f), 4.0f);
        }
        /* Mask/glass bypass is an A/B against the same picture: the gain is
         * fitted with the mask on so only the mask and the glass go away;
         * phosphor colour, beam and geometry stay. */
        disp_params.output_p3 = ctx->output_p3;
        if (ctx->display_bypass) {
            strip_mask(&disp_params, true);
            ctx->effective_panel_subpixels = 0;
        }
        ctx->effective_hdr_gain = ctx->hdr_enabled ? (disp_params.hdr_gain > 0 ? disp_params.hdr_gain : 1) : 1;
        disp_params.sdr_white_level = ctx->hdr_enabled
            ? SDL_GetFloatProperty(props, SDL_PROP_WINDOW_SDR_WHITE_LEVEL_FLOAT, 1.0f) : 1.0f;
        if(ctx->offscreen_w) {
            disp_params.sdr_white_level=1;
        }
        capture_params = disp_params;
        can_capture = true;
        SDL_GPUViewport viewport = {0};
        viewport.x = vp_x;
        viewport.y = vp_y;
        viewport.w = vp_w;
        viewport.h = vp_h;
        viewport.min_depth = 0.0f;
        viewport.max_depth = 1.0f;
        /* Subpixel lab: the reference half may show the picture without
         * the grille, or without mask and glass, at the same gain. */
        bool lab = ctx->lab_split && !ctx->offscreen_w;
        GPUDisplayParams base = disp_params;
        if (lab && ctx->lab_reference) strip_mask(&base, ctx->lab_reference == 2);
        gpu_display_render(ctx->gpu_disp, ctx->gpu, cmd,
                           ctx->display_tex, ctx->display_tex_w, ctx->display_tex_h,
                           swapchain_tex, (int)sw, (int)sh,
                           &base, &viewport);
        /* Subpixel lab: one half drawn again with the lab's grille, at the
         * same gain, so the two can be judged side by side. */
        if (lab) {
            GPUDisplayParams lab = disp_params;
            for (int i = 0; i < 3; i++) { lab.lab_gap[i] = ctx->lab_gap[i]; lab.lab_gain[i] = ctx->lab_gain[i]; }
            lab.lab_fill = ctx->lab_fill;
            /* The lab half may take the gain its own grille allows. */
            if (ctx->lab_fit && ctx->hdr_enabled && ctx->hdr_gain_mode == 0)
                lab.hdr_gain = fminf(lab.shoulder_knee * lab.hdr_headroom
                    / gpu_display_white_peak(&lab, chain->tv.beam_fwhm_max) * fmaxf(ctx->hdr_boost, 1.0f), 4.0f);
            lab.lab_scissor_x = (int)(ctx->lab_split == 1 ? vp_x + vp_w / 2 : vp_x);
            lab.lab_scissor_y = (int)vp_y; lab.lab_scissor_w = (int)(vp_w / 2); lab.lab_scissor_h = (int)vp_h;
            gpu_display_render(ctx->gpu_disp, ctx->gpu, cmd,
                               ctx->display_tex, ctx->display_tex_w, ctx->display_tex_h,
                               swapchain_tex, (int)sw, (int)sh, &lab, &viewport);
        }
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

    if (ctx->presentation_mode == GPU_PRESENT_60HZ && !ctx->scheduled_present && !ctx->vsync_paced) {
        uint64_t now = SDL_GetTicksNS();
        /* Start one interval ahead so source wakeup/processing jitter does not
         * immediately move the presentation deadline. Only this mode pays
         * the extra frame of startup latency. */
        if (!ctx->pacing_deadline_ns || now > ctx->pacing_deadline_ns + 3 * GPU_PRESENT_60HZ_PERIOD_NS)
            ctx->pacing_deadline_ns = gpu_presentation_next_ns(0, now);
        if (ctx->pacing_deadline_ns > now)
            SDL_DelayPrecise(ctx->pacing_deadline_ns - now);
    }
#ifdef MYNES_BUNDLED_SDL3
    SDL_SetNumberProperty(SDL_GetWindowProperties(ctx->window),
        "mynes.gpu.metal.source_frame", (Sint64)ctx->frame_counter);
#endif
    if(ctx->offscreen_w) {
        ctx->offscreen_fence=SDL_SubmitGPUCommandBufferAndAcquireFence(cmd);
        if(ctx->offscreen_fence) ctx->submit_ns=SDL_GetTicksNS();
    } else {
        poll_frame_fence(ctx, true);
        ctx->frame_fence=SDL_SubmitGPUCommandBufferAndAcquireFence(cmd);
        if(ctx->frame_fence) ctx->submit_ns=ctx->frame_fence_submit_ns=SDL_GetTicksNS();
    }
    if (ctx->submit_ns && ctx->presentation_mode == GPU_PRESENT_60HZ)
        ctx->pacing_deadline_ns = gpu_presentation_next_ns(ctx->pacing_deadline_ns, ctx->submit_ns);
    if (ctx->submit_ns && ctx->presentation_trace)
        fprintf(ctx->presentation_trace, "%llu,%llu,%d,%d,%.3f,%d,%d\n",
            (unsigned long long)ctx->submit_ns, (unsigned long long)ctx->frame_counter,
            ctx->presentation_slot, ctx->presentation_slots, ctx->presentation_hz,
            ctx->source_phase, ctx->presentation_mode);
    if (ctx->submit_ns && ctx->presentation_slots > 1) {
        if (!ctx->cadence_start_ns) ctx->cadence_start_ns = ctx->submit_ns;
        else if (++ctx->cadence_samples >= 60) {
            double elapsed = (ctx->submit_ns - ctx->cadence_start_ns) * 1e-9;
            double measured = ctx->cadence_samples / elapsed;
            /* Mode metadata is not evidence of actual ProMotion cadence.
             * Submission timing can reject slow pacing, not prove scanout. */
            if (measured < ctx->presentation_hz * .85) {
                ctx->presentation_blocked = true;
                fprintf(stderr, "BFI suspended: %.1f submissions/s below %.1f Hz display; using hold\n",
                    measured, ctx->presentation_hz);
            }
            ctx->cadence_samples = 0;
            ctx->cadence_start_ns = ctx->submit_ns;
        }
    }
    const char *capture_path = ctx->capture_path ? ctx->capture_path : SDL_getenv("MYNES_CAPTURE_PATH");
    const char *capture_frame_env = SDL_getenv("MYNES_CAPTURE_FRAME");
    unsigned capture_frame = capture_frame_env ? (unsigned)atoi(capture_frame_env) : 180;
    static bool captured = false;
    bool want_file = capture_path && (ctx->capture_path || (!captured && ctx->frame_counter >= capture_frame));
    if(can_capture && (want_file || ctx->capture_sink)) {
        SDL_GPUViewport viewport = {.x=vp_x,.y=vp_y,.w=vp_w,.h=vp_h,.min_depth=0,.max_depth=1};
        Uint64 start=SDL_GetTicksNS();
        ctx->capture_accepted = capture_display(ctx,&capture_params,sw,sh,&viewport,want_file ? capture_path : NULL);
        ctx->capture_ns=SDL_GetTicksNS()-start;
        if (!ctx->capture_accepted) ctx->capture_failed=true;
        if (want_file) captured = ctx->capture_accepted;
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
