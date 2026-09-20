/*
 * Signal Chain Runner — Generic GPU Compute Dispatch
 * ====================================================
 *
 * Data-driven stage dispatch with ping-pong buffers and per-stage timing.
 * Both audio and video chains use this same runner with different stage
 * configurations.
 */

#include "signal_chain.h"
#include "gpu_log.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

/* Shader filenames per kernel type. */
static const char *kernel_shader_names[CHAIN_KERNEL_COUNT] = {
    [CHAIN_KERNEL_POINTWISE]  = "pointwise.comp.spv",
    [CHAIN_KERNEL_RC_FILTER]  = "rc_filter.comp.spv",
    [CHAIN_KERNEL_FIR]        = "fir.comp.spv",
    [CHAIN_KERNEL_DELAY]      = "delay.comp.spv",
    [CHAIN_KERNEL_COMB]       = "comb_filter.comp.spv",
    [CHAIN_KERNEL_MODULATOR]  = "modulator.comp.spv",
    [CHAIN_KERNEL_DAC]        = "dac_2c02.comp.spv",
    [CHAIN_KERNEL_MATRIX]     = "matrix_decode.comp.spv",
    [CHAIN_KERNEL_PAL_CHROMA] = "pal_chroma.comp.spv",
    [CHAIN_KERNEL_DEFLECTION] = "deflection.comp.spv",
    [CHAIN_KERNEL_BEAM]       = "beam_profile.comp.spv",
    [CHAIN_KERNEL_RF]         = "rf_mod_demod.comp.spv",
    [CHAIN_KERNEL_VIDEO_AMP]  = "video_amp.comp.spv",
    [CHAIN_KERNEL_H_BLUR_RGB] = "h_blur_rgb.comp.spv",
    [CHAIN_KERNEL_TEMPORAL_BLIT] = "temporal_blit.comp.spv",
    [CHAIN_KERNEL_AGC]           = "agc.comp.spv",
    [CHAIN_KERNEL_RASTER] = "raster_encode.comp.spv",
    [CHAIN_KERNEL_RECEIVER] = "receiver_lock.comp.spv",
    [CHAIN_KERNEL_RECEIVER_DEMOD] = "receiver_demod.comp.spv",
    [CHAIN_KERNEL_YC_ROUTE] = "yc_route.comp.spv",
};

/* Workgroup sizes per kernel type. */
static const int kernel_workgroup_x[CHAIN_KERNEL_COUNT] = {
    [CHAIN_KERNEL_POINTWISE]  = 256,
    [CHAIN_KERNEL_RC_FILTER]  = 256,   /* sequential: 1 thread per scanline */
    [CHAIN_KERNEL_FIR]        = 256,
    [CHAIN_KERNEL_DELAY]      = 256,
    [CHAIN_KERNEL_COMB]       = 256,
    [CHAIN_KERNEL_MODULATOR]  = 256,
    [CHAIN_KERNEL_DAC]        = 256,
    [CHAIN_KERNEL_MATRIX]     = 256,
    [CHAIN_KERNEL_PAL_CHROMA] = 256,
    [CHAIN_KERNEL_DEFLECTION] = 16,   /* 16×16 for 2D dispatch */
    [CHAIN_KERNEL_BEAM]       = 16,   /* 16×16 for 2D dispatch */
    [CHAIN_KERNEL_RF]         = 256,
    [CHAIN_KERNEL_VIDEO_AMP]  = 256,
    [CHAIN_KERNEL_H_BLUR_RGB] = 256,
    [CHAIN_KERNEL_TEMPORAL_BLIT] = 16,  /* 16×16 for 2D dispatch */
    [CHAIN_KERNEL_AGC]           = 256,
    [CHAIN_KERNEL_RASTER] = 256,
    [CHAIN_KERNEL_RECEIVER] = 256,
    [CHAIN_KERNEL_YC_ROUTE] = 256,
    [CHAIN_KERNEL_RECEIVER_DEMOD] = 256, /* sequential: 1 thread per scanline */
};

/* Resource counts per kernel type: {readonly, readwrite, uniform}. */
static const int kernel_resources[CHAIN_KERNEL_COUNT][3] = {
    /* readonly, readwrite, uniform */
    [CHAIN_KERNEL_POINTWISE]  = { 0, 2, 1 },
    [CHAIN_KERNEL_RC_FILTER]  = { 0, 2, 1 },  /* data + carry */
    [CHAIN_KERNEL_FIR]        = { 2, 1, 1 },  /* input + taps → output */
    [CHAIN_KERNEL_DELAY]      = { 2, 1, 1 },  /* input + prev → output */
    [CHAIN_KERNEL_COMB]       = { 1, 2, 1 },  /* signal → Y + C */
    [CHAIN_KERNEL_MODULATOR]  = { 1, 2, 1 },  /* input → out1 + out2 */
    [CHAIN_KERNEL_DAC]        = { 3, 2, 1 },  /* indices + table → waveform */
    [CHAIN_KERNEL_MATRIX]     = { 4, 1, 1 },  /* Y,I,Q → RGB */
    [CHAIN_KERNEL_PAL_CHROMA] = { 2, 2, 1 },  /* V,U raw → V,U corrected */
    [CHAIN_KERNEL_DEFLECTION] = { 0, 2, 1 },  /* params → landing_x + landing_y */
    [CHAIN_KERNEL_BEAM]       = { 3, 1, 1 },  /* RGB + landing maps → RGBA_out */
    [CHAIN_KERNEL_RF]         = { 0, 1, 1 },  /* composite in-place */
    [CHAIN_KERNEL_VIDEO_AMP]  = { 1, 1, 1 },  /* RGB_in → RGB_out */
    [CHAIN_KERNEL_H_BLUR_RGB] = { 0, 2, 1 },  /* RGB_in + RGB_out as readwrite */
    [CHAIN_KERNEL_TEMPORAL_BLIT] = { 2, 1, 1 },  /* cur+prev, recursive history, and output texture */
    [CHAIN_KERNEL_RASTER] = { 2, 2, 1 },
    [CHAIN_KERNEL_RECEIVER] = { 1, 1, 1 },
    [CHAIN_KERNEL_RECEIVER_DEMOD] = { 2, 2, 1 },
    [CHAIN_KERNEL_YC_ROUTE] = { 2, 2, 1 },
    [CHAIN_KERNEL_AGC]           = { 0, 2, 1 },  /* data + carry in-place */
};

/* Build a full path from directory + filename. */
static char *build_shader_path(const char *dir, const char *name) {
    size_t dlen = strlen(dir);
    size_t nlen = strlen(name);
    int need_slash = (dlen > 0 && dir[dlen - 1] != '/') ? 1 : 0;
    char *path = (char *)malloc(dlen + need_slash + nlen + 1);
    if (!path) return NULL;
    memcpy(path, dir, dlen);
    if (need_slash) path[dlen] = '/';
    memcpy(path + dlen + need_slash, name, nlen + 1);
    return path;
}

/* Load a pipeline for a given kernel type if not already loaded. */
static bool ensure_pipeline(SignalChain *chain, SDL_GPUDevice *gpu,
                             ChainKernelType type, const char *shader_dir) {
    if (chain->pipeline_loaded[type]) return true;
    if (type >= CHAIN_KERNEL_COUNT) return false;

    char *path = build_shader_path(shader_dir, kernel_shader_names[type]);
    if (!path) return false;

    GpuPipelineResources res = {0};
    res.num_readonly_storage_buffers  = kernel_resources[type][0];
    res.num_readwrite_storage_buffers = kernel_resources[type][1];
    res.num_uniform_buffers           = kernel_resources[type][2];

    /* temporal_blit writes to a storage texture instead of a buffer. */
    if (type == CHAIN_KERNEL_TEMPORAL_BLIT)
        res.num_readwrite_storage_textures = 1;

    int wg_x = kernel_workgroup_x[type];
    int wg_y = (type == CHAIN_KERNEL_DEFLECTION ||
                type == CHAIN_KERNEL_BEAM ||
                type == CHAIN_KERNEL_TEMPORAL_BLIT) ? 16 : 1;

    bool ok = gpu_pipeline_create(gpu, &chain->pipelines[type], path, "main",
                                   wg_x, wg_y, 1, &res,
                                   kernel_shader_names[type]);
    free(path);
    if (ok) chain->pipeline_loaded[type] = true;
    return ok;
}

/* ============================================================================
 * Initialization
 * ============================================================================ */

bool chain_init(SignalChain *chain, SDL_GPUDevice *gpu,
                int sample_count, const char *shader_dir) {
    memset(chain, 0, sizeof(*chain));
    chain->sample_count = sample_count;
    chain->buf_size = (uint32_t)(sample_count * sizeof(float));
    chain->timing_enabled = true;

    /* Allocate ping-pong buffers. */
    chain->buf[0] = gpu_buffer_create(gpu, chain->buf_size, GPU_BUF_READWRITE);
    chain->buf[1] = gpu_buffer_create(gpu, chain->buf_size, GPU_BUF_READWRITE);
    if (!chain->buf[0] || !chain->buf[1]) {
        fprintf(stderr, "chain_init: failed to create ping-pong buffers (%u bytes each)\n",
                chain->buf_size);
        chain_destroy(chain, gpu);
        return false;
    }

    /* Allocate auxiliary buffers (for dual-output stages like I/Q demod). */
    chain->aux_size = chain->buf_size;
    for (int i = 0; i < CHAIN_MAX_AUX_BUFS; i++) {
        chain->aux[i] = gpu_buffer_create(gpu, chain->aux_size, GPU_BUF_READWRITE);
    }

    /* Carry buffer for RC prefix scan (sized for max blocks per dispatch). */
    int max_blocks = (sample_count + 1023) / 1024;
    chain->carry_size = (uint32_t)(max_blocks * 2 * sizeof(float));
    chain->carry_buf = gpu_buffer_create(gpu, chain->carry_size, GPU_BUF_READWRITE);

    chain->current_buf = 0;

    /* Load all pipeline types lazily (on first use). Store shader_dir
     * for lazy loading — but we can also pre-load them all now. */
    for (int t = 0; t < CHAIN_KERNEL_COUNT; t++) {
        ensure_pipeline(chain, gpu, (ChainKernelType)t, shader_dir);
        /* Non-fatal if a shader doesn't exist — stages using it just won't dispatch. */
    }

    LOGV("chain_init: %d samples, %u bytes per buffer, %d pipelines loaded\n",
           sample_count, chain->buf_size,
           ({int n=0; for(int i=0;i<CHAIN_KERNEL_COUNT;i++) if(chain->pipeline_loaded[i]) n++; n;}));

    return true;
}

/* ============================================================================
 * Stage management
 * ============================================================================ */

int chain_add_stage(SignalChain *chain, const char *name,
                    ChainKernelType kernel_type,
                    const void *params, uint32_t params_size,
                    uint32_t dispatch_x, uint32_t dispatch_y) {
    if (chain->num_stages >= CHAIN_MAX_STAGES) return -1;
    if (params_size > CHAIN_MAX_UNIFORM_SIZE) return -1;

    int idx = chain->num_stages++;
    ChainStage *s = &chain->stages[idx];
    memset(s, 0, sizeof(*s));
    s->name = name;
    s->kernel_type = kernel_type;
    s->enabled = true;
    s->bypass = false;
    if (params && params_size > 0) {
        memcpy(s->params, params, params_size);
    }
    s->params_size = params_size;
    s->dispatch_x = dispatch_x;
    s->dispatch_y = dispatch_y;
    s->dispatch_z = 1;
    s->taps_index = -1;   /* caller sets later if the kernel uses taps */

    /* Populate typed bindings from the kernel type by default. Callers
     * that need something non-standard can overwrite ro[] / rw[] /
     * taps_index / external[] or install a rebind hook afterwards. */
    chain_stage_set_default_io(s);

    return idx;
}

int chain_upload_taps(SignalChain *chain, SDL_GPUDevice *gpu,
                      const float *taps, int num_taps) {
    if (chain->num_tap_bufs >= CHAIN_MAX_TAP_BUFS) return -1;
    int idx = chain->num_tap_bufs++;
    uint32_t size = (uint32_t)(num_taps * sizeof(float));
    chain->tap_bufs[idx] = gpu_buffer_create(gpu, size, GPU_BUF_READONLY);
    if (!chain->tap_bufs[idx]) return -1;
    chain->tap_sizes[idx] = size;
    gpu_buffer_upload(gpu, chain->tap_bufs[idx], taps, size);
    return idx;
}

/* ============================================================================
 * Input / Output
 * ============================================================================ */

bool chain_upload_input(SignalChain *chain, SDL_GPUDevice *gpu,
                        const void *data, uint32_t size) {
    if (size > chain->buf_size) return false;
    chain->current_buf = 0;
    return gpu_buffer_upload(gpu, chain->buf[0], data, size);
}

/* Upload via copy pass in an external command buffer (no separate fence). */
bool chain_upload_input_cmd(SignalChain *chain, SDL_GPUDevice *gpu,
                             SDL_GPUCommandBuffer *cmd,
                             const void *data, uint32_t size) {
    if (size > chain->buf_size) return false;
    chain->current_buf = 0;

    /* Create transfer buffer, map, copy, unmap. */
    SDL_GPUTransferBufferCreateInfo tbci = {0};
    tbci.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
    tbci.size = size;
    SDL_GPUTransferBuffer *tbuf = SDL_CreateGPUTransferBuffer(gpu, &tbci);
    if (!tbuf) return false;

    void *mapped = SDL_MapGPUTransferBuffer(gpu, tbuf, false);
    if (!mapped) { SDL_ReleaseGPUTransferBuffer(gpu, tbuf); return false; }
    memcpy(mapped, data, size);
    SDL_UnmapGPUTransferBuffer(gpu, tbuf);

    /* Copy pass in the shared command buffer. */
    SDL_GPUCopyPass *copy = SDL_BeginGPUCopyPass(cmd);
    if (!copy) { SDL_ReleaseGPUTransferBuffer(gpu, tbuf); return false; }

    SDL_GPUTransferBufferLocation src = { .transfer_buffer = tbuf, .offset = 0 };
    SDL_GPUBufferRegion dst = { .buffer = chain->buf[0], .offset = 0, .size = size };
    SDL_UploadToGPUBuffer(copy, &src, &dst, false);
    SDL_EndGPUCopyPass(copy);

    SDL_ReleaseGPUTransferBuffer(gpu, tbuf);
    return true;
}

bool chain_download_output(SignalChain *chain, SDL_GPUDevice *gpu,
                            void *out, uint32_t size) {
    return gpu_buffer_download(gpu, chain->buf[chain->current_buf], out, size);
}

static bool copy_stage_snapshot(SDL_GPUCommandBuffer *cmd,
                                SDL_GPUBuffer *src,
                                SDL_GPUBuffer *dst,
                                uint32_t size) {
    if (!src || !dst || size == 0) return false;

    SDL_GPUCopyPass *copy = SDL_BeginGPUCopyPass(cmd);
    if (!copy) return false;

    SDL_GPUBufferLocation src_region = { .buffer = src, .offset = 0 };
    SDL_GPUBufferLocation dst_region = { .buffer = dst, .offset = 0 };
    SDL_CopyGPUBufferToBuffer(copy, &src_region, &dst_region, size, false);
    SDL_EndGPUCopyPass(copy);
    return true;
}

/* ============================================================================
 * Typed binding resolver
 * ============================================================================
 *
 * Turns a symbolic ChainBufRef into the concrete SDL_GPUBuffer* for a
 * stage dispatch. Centralising this keeps the per-kernel switch out
 * of chain_run_cmd and lets stage declarations be inspected / tested
 * in isolation.
 * ============================================================================ */

static SDL_GPUBuffer *resolve_buf_ref(const SignalChain *chain,
                                      const ChainStage *stage,
                                      ChainBufRef ref) {
    switch (ref) {
    case CBR_NONE:      return NULL;
    case CBR_BUF_SRC:   return chain->buf[chain->current_buf];
    case CBR_BUF_DST:   return chain->buf[1 - chain->current_buf];
    case CBR_AUX0:      return chain->aux[0];
    case CBR_AUX1:      return chain->aux[1];
    case CBR_AUX2:      return chain->aux[2];
    case CBR_AUX3:      return chain->aux[3];
    case CBR_TAPS:
        return (stage->taps_index >= 0 && stage->taps_index < chain->num_tap_bufs)
             ? chain->tap_bufs[stage->taps_index]
             : NULL;
    case CBR_CARRY:     return chain->carry_buf;
    case CBR_EXT0:      return stage->external[0];
    case CBR_EXT1:      return stage->external[1];
    case CBR_EXT2:      return stage->external[2];
    case CBR_EXT3:      return stage->external[3];
    }
    return NULL;
}

/* Does any of the RW bindings flip ping-pong (i.e. references DST)?
 * Stages that write in-place (CBR_BUF_SRC as an RW binding) do not
 * advance current_buf. Stages writing to a fresh DST ping-pong buffer
 * advance it. Stages writing only to aux/ext buffers also leave
 * current_buf alone. */
static bool rw_bindings_advance(const ChainStage *s) {
    for (int i = 0; i < s->rw_count; i++) {
        if (s->rw[i] == CBR_BUF_DST) return true;
    }
    return false;
}

/* Dispatch one stage via the typed bindings. Returns false on error.
 * Updates chain->current_buf if any RW binding is CBR_BUF_DST. */
static bool dispatch_typed_stage(SignalChain *chain, ChainStage *s,
                                  SDL_GPUCommandBuffer *cmd) {
    ChainKernelType kt = s->kernel_type;
    if (!chain->pipeline_loaded[kt]) {
        fprintf(stderr, "chain_run: stage '%s' uses kernel %d which isn't loaded\n",
                s->name, kt);
        return true;  /* skip — not fatal */
    }

    /* Allow per-frame rebinding before resolving (chroma aux layout
     * toggles with comb state, for example). */
    if (s->rebind) {
        if (!s->rebind((struct SignalChainFwd *)chain,
                        (struct ChainStageFwd *)s, s->rebind_user)) {
            return false;
        }
    }

    /* Resolve readwrite bindings at pass-begin time. */
    SDL_GPUStorageBufferReadWriteBinding rw_bindings[CHAIN_STAGE_MAX_RW] = {0};
    for (int i = 0; i < s->rw_count; i++) {
        rw_bindings[i].buffer = resolve_buf_ref(chain, s, s->rw[i]);
        if (!rw_bindings[i].buffer) {
            fprintf(stderr, "chain_run: stage '%s' rw[%d] resolves to NULL\n",
                    s->name, i);
            return false;
        }
    }

    SDL_GPUComputePass *pass = SDL_BeginGPUComputePass(
        cmd, NULL, 0, rw_bindings, (Uint32)s->rw_count);
    if (!pass) return false;

    SDL_BindGPUComputePipeline(pass, chain->pipelines[kt].pipeline);

    /* RC_FILTER and AGC dispatch per-line (one thread per scanline),
     * not per-sample. If the chain has a per-line stride, recompute
     * dispatch_x from num_lines. Other kernels use s->dispatch_x as
     * passed at registration. */
    uint32_t dispatch_x = s->dispatch_x;
    uint32_t dispatch_y = s->dispatch_y;
    uint32_t dispatch_z = s->dispatch_z;
    if ((kt == CHAIN_KERNEL_RC_FILTER || kt == CHAIN_KERNEL_AGC) &&
        chain->samples_per_line > 0) {
        int num_lines = chain->sample_count / chain->samples_per_line;
        dispatch_x = (uint32_t)((num_lines + 255) / 256);
        dispatch_y = 1;
        dispatch_z = 1;
    }

    /* Resolve readonly bindings and bind. */
    if (s->ro_count > 0) {
        SDL_GPUBuffer *ro_bufs[CHAIN_STAGE_MAX_RO];
        for (int i = 0; i < s->ro_count; i++) {
            ro_bufs[i] = resolve_buf_ref(chain, s, s->ro[i]);
            if (!ro_bufs[i]) {
                fprintf(stderr, "chain_run: stage '%s' ro[%d] resolves to NULL\n",
                        s->name, i);
                SDL_EndGPUComputePass(pass);
                return false;
            }
        }
        SDL_BindGPUComputeStorageBuffers(pass, 0, ro_bufs, (Uint32)s->ro_count);
    }

    if (s->params_size > 0) {
        SDL_PushGPUComputeUniformData(cmd, 0, s->params, s->params_size);
    }

    SDL_DispatchGPUCompute(pass, dispatch_x, dispatch_y, dispatch_z);
    SDL_EndGPUComputePass(pass);

    /* Debug capture: copy the stage's declared snapshot output (or
     * rw[0] as a fallback) into the per-stage debug buffer. Size is
     * stage->snapshot_size when non-zero (Matrix Decode writes RGB, 3×
     * signal) — otherwise the default chain->buf_size applies. */
    int sidx = (int)(s - chain->stages);
    if (chain->debug_stage_capture_enabled[sidx] &&
        chain->debug_stage_bufs[sidx]) {
        SDL_GPUBuffer *src = NULL;
        if (s->snapshot_src != CBR_NONE) {
            src = resolve_buf_ref(chain, s, s->snapshot_src);
        } else if (s->rw_count > 0) {
            src = rw_bindings[0].buffer;
        }
        uint32_t snap_bytes = s->snapshot_size ? s->snapshot_size : chain->buf_size;
        if (src && !copy_stage_snapshot(cmd, src,
                                        chain->debug_stage_bufs[sidx],
                                        snap_bytes)) {
            fprintf(stderr, "dispatch_typed_stage: snapshot copy failed for '%s'\n",
                    s->name ? s->name : "?");
            return false;
        }
    }

    /* Advance current_buf if this stage wrote to BUF_DST. */
    if (rw_bindings_advance(s)) {
        chain->current_buf = 1 - chain->current_buf;
    }

    return true;
}

/* Populate a stage's typed bindings from its kernel_type using the
 * canonical pattern each kernel expects. Mirrors the legacy dispatch
 * switch in chain_run_cmd, but declaratively — the pattern is visible
 * in the stage struct after this returns, and chain_run_cmd's typed
 * dispatcher uses the same fields for every kernel. */
void chain_stage_set_default_io(ChainStage *s) {
    if (!s) return;
    switch (s->kernel_type) {

    case CHAIN_KERNEL_RC_FILTER:
        /* In-place on buf[src]; carry buffer for per-block state. */
        s->rw[0] = CBR_BUF_SRC; s->rw[1] = CBR_CARRY; s->rw_count = 2;
        s->ro_count = 0;
        break;

    case CHAIN_KERNEL_AGC:
        /* Same shape as RC: in-place with carry. */
        s->rw[0] = CBR_BUF_SRC; s->rw[1] = CBR_CARRY; s->rw_count = 2;
        s->ro_count = 0;
        break;

    case CHAIN_KERNEL_RF:
        /* In-place on buf[src]. */
        s->rw[0] = CBR_BUF_SRC; s->rw_count = 1;
        s->ro_count = 0;
        break;

    case CHAIN_KERNEL_POINTWISE:
        /* Reads buf[src], writes buf[dst]. Flips current_buf. */
        s->rw[0] = CBR_BUF_SRC; s->rw[1] = CBR_BUF_DST; s->rw_count = 2;
        s->ro_count = 0;
        break;

    case CHAIN_KERNEL_FIR:
        /* Reads buf[src] + tap buffer, writes buf[dst]. Flips. */
        s->ro[0] = CBR_BUF_SRC; s->ro[1] = CBR_TAPS; s->ro_count = 2;
        s->rw[0] = CBR_BUF_DST; s->rw_count = 1;
        break;

    case CHAIN_KERNEL_DELAY:
        /* Reads buf[src] + aux[0] (delay history), writes buf[dst].
         * Flips current_buf. */
        s->ro[0] = CBR_BUF_SRC; s->ro[1] = CBR_AUX0; s->ro_count = 2;
        s->rw[0] = CBR_BUF_DST; s->rw_count = 1;
        break;

    case CHAIN_KERNEL_COMB:
        /* Reads buf[src]; writes buf[dst] (Y) + aux[0] (C). Flips. */
        s->ro[0] = CBR_BUF_SRC; s->ro_count = 1;
        s->rw[0] = CBR_BUF_DST; s->rw[1] = CBR_AUX0; s->rw_count = 2;
        break;

    case CHAIN_KERNEL_MODULATOR:
        /* Single-output modulator: reads buf[src], writes buf[dst].
         * The dual-output IQ demod mode doesn't use the generic chain
         * runner — it's driven via custom dispatch. */
        s->ro[0] = CBR_BUF_SRC; s->ro_count = 1;
        s->rw[0] = CBR_BUF_DST; s->rw_count = 1;
        break;

    default:
        /* Generic — leave bindings empty; caller can set them
         * manually. io_typed still flipped true so the dispatcher
         * doesn't fall through to the legacy switch. */
        break;
    }
    s->io_typed = true;
}

/* ============================================================================
 * Chain execution
 * ============================================================================ */

/* Record chain stages into an external command buffer (no fence). */
bool chain_run_cmd(SignalChain *chain, SDL_GPUCommandBuffer *cmd) {
    const double perf_freq = (double)SDL_GetPerformanceFrequency();
    /* EMA smoothing factor. ~10% of a new reading blends in each call,
     * so timing_avg_us tracks recent trends while smoothing jitter. */
    const double ema_alpha = 0.10;

    for (int i = 0; i < chain->num_stages; i++) {
        ChainStage *s = &chain->stages[i];
        if (!s->enabled || s->bypass) continue;

        Uint64 t_start = SDL_GetPerformanceCounter();

        /* Custom-dispatch stage: the stage's callback owns its pass
         * entirely. Only the post-signal RGB-domain meta stage uses
         * this today (video amp + h-blur + beam + temporal blit) —
         * those stages touch buffers the chain doesn't own. */
        if (s->custom) {
            if (!s->custom(chain, s, cmd, s->custom_user)) return false;
            /* Optional snapshot after a custom dispatch. Honors the
             * same snapshot_src/size declaration used by typed stages.
             * chain_set_stage_capture refuses to enable capture on a
             * custom stage with no snapshot_src, so reaching this path
             * with capture on implies a resolvable source. */
            if (chain->debug_stage_capture_enabled[i] &&
                chain->debug_stage_bufs[i] &&
                s->snapshot_src != CBR_NONE) {
                SDL_GPUBuffer *src = resolve_buf_ref(chain, s, s->snapshot_src);
                uint32_t snap_bytes = s->snapshot_size
                                      ? s->snapshot_size
                                      : chain->buf_size;
                if (src && !copy_stage_snapshot(cmd, src,
                                                chain->debug_stage_bufs[i],
                                                snap_bytes)) {
                    fprintf(stderr,
                            "chain_run_cmd: snapshot copy failed for custom '%s'\n",
                            s->name ? s->name : "?");
                    return false;
                }
            }
        } else {
            /* Typed-binding dispatch — the runner reads ro[] / rw[] /
             * taps_index / external[] set up at registration time (or
             * just before dispatch via `rebind`) and builds the pass
             * generically. */
            if (!dispatch_typed_stage(chain, s, cmd)) return false;
        }

        /* Per-stage timing. This measures CPU-side command-recording
         * cost, not GPU-side execution cost — real GPU time would
         * need SDL_GPU query objects. The visualiser / debug_tap
         * code publishes these values; previously they were always
         * zero. Wall-clock in microseconds, plus a smoothed EMA. */
        Uint64 t_end = SDL_GetPerformanceCounter();
        double us = (double)(t_end - t_start) * 1e6 / perf_freq;
        s->timing_us = us;
        s->timing_avg_us = (s->timing_avg_us == 0.0)
            ? us
            : s->timing_avg_us + (us - s->timing_avg_us) * ema_alpha;
    }

    return true;
}

/* Run chain with its own command buffer + fence (standalone mode). */
bool chain_run(SignalChain *chain, SDL_GPUDevice *gpu) {
    chain->total_timing_us = 0.0;
    Uint64 t0 = SDL_GetPerformanceCounter();

    SDL_GPUCommandBuffer *cmd = SDL_AcquireGPUCommandBuffer(gpu);
    if (!cmd) return false;

    if (!chain_run_cmd(chain, cmd)) {
        SDL_SubmitGPUCommandBuffer(cmd);
        return false;
    }

    SDL_GPUFence *fence = SDL_SubmitGPUCommandBufferAndAcquireFence(cmd);
    if (fence) {
        SDL_WaitForGPUFences(gpu, true, &fence, 1);
        SDL_ReleaseGPUFence(gpu, fence);
    }

    Uint64 t1 = SDL_GetPerformanceCounter();
    chain->total_timing_us = (double)(t1 - t0) * 1000000.0
                           / (double)SDL_GetPerformanceFrequency();
    return true;
}

/* ============================================================================
 * Stage queries
 * ============================================================================ */

void chain_set_stage_enabled(SignalChain *chain, int index, bool enabled) {
    if (index >= 0 && index < chain->num_stages)
        chain->stages[index].enabled = enabled;
}

void chain_set_stage_bypass(SignalChain *chain, int index, bool bypass) {
    if (index >= 0 && index < chain->num_stages)
        chain->stages[index].bypass = bypass;
}

void chain_update_params(SignalChain *chain, int index,
                          const void *params, uint32_t size) {
    if (index >= 0 && index < chain->num_stages && size <= CHAIN_MAX_UNIFORM_SIZE)
        memcpy(chain->stages[index].params, params, size);
}

double chain_get_stage_timing(const SignalChain *chain, int index) {
    if (index >= 0 && index < chain->num_stages)
        return chain->stages[index].timing_avg_us;
    return 0.0;
}

double chain_get_total_timing(const SignalChain *chain) {
    return chain->total_timing_us;
}

const char *chain_get_stage_name(const SignalChain *chain, int index) {
    if (index >= 0 && index < chain->num_stages)
        return chain->stages[index].name;
    return "?";
}

bool chain_get_stage_enabled(const SignalChain *chain, int index) {
    if (index >= 0 && index < chain->num_stages)
        return chain->stages[index].enabled;
    return false;
}

bool chain_get_stage_bypassed(const SignalChain *chain, int index) {
    if (index >= 0 && index < chain->num_stages)
        return chain->stages[index].bypass;
    return false;
}

int chain_get_num_stages(const SignalChain *chain) {
    return chain->num_stages;
}

bool chain_set_stage_capture(SignalChain *chain, SDL_GPUDevice *gpu,
                             int index, bool enabled) {
    if (!chain || !gpu || index < 0 || index >= chain->num_stages) return false;

    if (enabled) {
        ChainStage *s = &chain->stages[index];
        /* Custom stages without an explicit snapshot source have no
         * usable primary output to sample — refuse rather than silently
         * leaving the snapshot buffer empty and returning stale data. */
        if (s->custom && s->snapshot_src == CBR_NONE) {
            return false;
        }
        uint32_t bytes = s->snapshot_size ? s->snapshot_size : chain->buf_size;
        if (!chain->debug_stage_bufs[index]) {
            chain->debug_stage_bufs[index] = gpu_buffer_create(
                gpu, bytes, GPU_BUF_READWRITE);
            if (!chain->debug_stage_bufs[index]) return false;
        }
        chain->debug_stage_capture_enabled[index] = true;
    } else {
        chain->debug_stage_capture_enabled[index] = false;
        if (chain->debug_stage_bufs[index]) {
            SDL_ReleaseGPUBuffer(gpu, chain->debug_stage_bufs[index]);
            chain->debug_stage_bufs[index] = NULL;
        }
    }

    return true;
}

SDL_GPUBuffer *chain_get_stage_capture_buffer(const SignalChain *chain, int index) {
    if (!chain || index < 0 || index >= chain->num_stages) return NULL;
    return chain->debug_stage_bufs[index];
}

uint32_t chain_get_stage_snapshot_size(const SignalChain *chain, int index) {
    if (!chain || index < 0 || index >= chain->num_stages) return 0;
    uint32_t ss = chain->stages[index].snapshot_size;
    return ss ? ss : chain->buf_size;
}

/* ============================================================================
 * Cleanup
 * ============================================================================ */

void chain_destroy(SignalChain *chain, SDL_GPUDevice *gpu) {
    for (int i = 0; i < 2; i++) {
        if (chain->buf[i]) SDL_ReleaseGPUBuffer(gpu, chain->buf[i]);
    }
    for (int i = 0; i < CHAIN_MAX_AUX_BUFS; i++) {
        if (chain->aux[i]) SDL_ReleaseGPUBuffer(gpu, chain->aux[i]);
    }
    if (chain->carry_buf) SDL_ReleaseGPUBuffer(gpu, chain->carry_buf);
    for (int i = 0; i < chain->num_tap_bufs; i++) {
        if (chain->tap_bufs[i]) SDL_ReleaseGPUBuffer(gpu, chain->tap_bufs[i]);
    }
    for (int i = 0; i < CHAIN_MAX_STAGES; i++) {
        if (chain->debug_stage_bufs[i]) SDL_ReleaseGPUBuffer(gpu, chain->debug_stage_bufs[i]);
    }
    for (int i = 0; i < CHAIN_KERNEL_COUNT; i++) {
        if (chain->pipeline_loaded[i]) {
            gpu_pipeline_destroy(gpu, &chain->pipelines[i]);
        }
    }
    memset(chain, 0, sizeof(*chain));
}
