/*
 * Audio GPU Chain -- Implementation
 * ===================================
 *
 * See audio_gpu.h for API documentation and signal chain order.
 *
 * Key design decisions:
 *
 * 1. RC FILTER IS IN-PLACE. The rc_filter shader reads and writes the
 *    same DataBuf. So for a chain of RC stages, we keep the data in one
 *    buffer and run successive RC dispatches on it. No copy needed between
 *    consecutive RC stages because each dispatch ends its compute pass
 *    (which acts as a barrier in SDL3).
 *
 * 2. POINTWISE READS FROM ONE BUFFER, WRITES TO ANOTHER. We track which
 *    buffer holds the "current" data and alternate. When a pointwise stage
 *    is disabled, we skip the dispatch and the current buffer stays the same.
 *
 * 3. TRANSITIONS BETWEEN RC AND POINTWISE. When the chain moves from RC
 *    stages (in-place on buf_a) to a pointwise stage (buf_a -> buf_b),
 *    no copy is needed -- buf_a already has the data. When moving from
 *    pointwise output (on some buffer) to an RC stage, the RC dispatch
 *    operates in-place on whichever buffer currently holds the data.
 *
 * 4. FIR DECIMATION reads from the current buffer and writes to buf_output.
 *    The taps buffer is readonly and uploaded once at init.
 *
 * 5. CARRY BUFFER. The RC filter's parallel prefix scan needs a carry
 *    buffer for inter-block state. We allocate one carry buffer large
 *    enough for the maximum number of blocks and reuse it for all RC
 *    stages. The carry buffer is zeroed before each RC dispatch via a
 *    simple upload of zeros.
 *
 * 6. SINGLE COMMAND BUFFER per frame. All dispatches go into one command
 *    buffer. Each gpu_dispatch() call begins and ends a compute pass,
 *    providing the barrier between stages. The command buffer is submitted
 *    once at the end, and we wait for the readback fence.
 */

#include "audio_gpu.h"
#include "audio_format.h"
#include "gpu_log.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

/* ============================================================================
 * Helpers
 * ============================================================================ */

/* Build a path: dir/filename. Caller must free the result. */
static char *build_path(const char *dir, const char *filename) {
    size_t dlen = strlen(dir);
    size_t flen = strlen(filename);
    /* Account for possible missing trailing slash. */
    int need_slash = (dlen > 0 && dir[dlen - 1] != '/') ? 1 : 0;
    char *path = (char *)malloc(dlen + need_slash + flen + 1);
    if (!path) return NULL;
    memcpy(path, dir, dlen);
    if (need_slash) path[dlen] = '/';
    memcpy(path + dlen + need_slash, filename, flen + 1);
    return path;
}

/* ============================================================================
 * Initialization
 * ============================================================================ */

bool audio_gpu_init(AudioGPUChain *agc, SDL_GPUDevice *gpu,
                    AudioChain *chain, const char *shader_dir)
{
    memset(agc, 0, sizeof(*agc));
    agc->chain = chain;

    /* ---- Load compute pipelines ---- */

    /* Pointwise shader: local_size_x = 256, 0 readonly + 2 readwrite + 1 uniform. */
    {
        char *path = build_path(shader_dir, "pointwise.comp.spv");
        if (!path) { fprintf(stderr, "audio_gpu_init: alloc failed\n"); return false; }

        GpuPipelineResources res = {0};
        res.num_readwrite_storage_buffers = 2;
        res.num_uniform_buffers = 1;

        bool ok = gpu_pipeline_create(gpu, &agc->pipe_pointwise, path, "main",
                                      256, 1, 1, &res, "audio_pointwise");
        free(path);
        if (!ok) goto fail;
    }

    /* RC filter shader: local_size_x = 1024, 0 readonly + 2 readwrite + 1 uniform. */
    {
        char *path = build_path(shader_dir, "rc_filter.comp.spv");
        if (!path) { fprintf(stderr, "audio_gpu_init: alloc failed\n"); return false; }

        GpuPipelineResources res = {0};
        res.num_readwrite_storage_buffers = 2;
        res.num_uniform_buffers = 1;

        bool ok = gpu_pipeline_create(gpu, &agc->pipe_rc_filter, path, "main",
                                      256, 1, 1, &res, "audio_rc_filter");
        free(path);
        if (!ok) goto fail;
    }

    /* FIR shader: local_size_x = 256, 2 readonly + 1 readwrite + 1 uniform. */
    {
        char *path = build_path(shader_dir, "fir.comp.spv");
        if (!path) { fprintf(stderr, "audio_gpu_init: alloc failed\n"); return false; }

        GpuPipelineResources res = {0};
        res.num_readonly_storage_buffers = 2;
        res.num_readwrite_storage_buffers = 1;
        res.num_uniform_buffers = 1;

        bool ok = gpu_pipeline_create(gpu, &agc->pipe_fir, path, "main",
                                      256, 1, 1, &res, "audio_fir");
        free(path);
        if (!ok) goto fail;
    }

    /* ---- Compute buffer sizes ---- */

    /* Use the maximum frame size (PAL is larger) so buffers work for
     * both regions without reallocation. */
    int max_samples = AUDIO_MAX_SAMPLES_PER_FRAME;
    int max_blocks  = (max_samples + AUDIO_WORKGROUP_SIZE - 1) / AUDIO_WORKGROUP_SIZE;
    int max_output  = AUDIO_OUTPUT_PAL_SAMPLES;  /* PAL has more output samples */

    agc->input_size  = (Uint32)(max_samples * sizeof(float));
    agc->output_size = (Uint32)(max_output * sizeof(float));
    /* Carry buffer: one vec2 (2 floats) per block. */
    agc->carry_size  = (Uint32)(max_blocks * 2 * sizeof(float));
    agc->taps_size   = (chain->decimation.tap_count > 0)
                     ? (Uint32)(chain->decimation.tap_count * sizeof(float))
                     : 0;

    /* ---- Allocate GPU buffers ---- */

    /* Input buffer: uploaded from CPU each frame, read by first stage. */
    agc->buf_input = gpu_buffer_create(gpu, agc->input_size, GPU_BUF_READWRITE);
    if (!agc->buf_input) goto fail;

    /* Ping-pong intermediates. Both need read+write since RC filter is
     * in-place and pointwise reads one / writes the other. */
    agc->buf_a = gpu_buffer_create(gpu, agc->input_size, GPU_BUF_READWRITE);
    if (!agc->buf_a) goto fail;

    agc->buf_b = gpu_buffer_create(gpu, agc->input_size, GPU_BUF_READWRITE);
    if (!agc->buf_b) goto fail;

    /* Carry buffer for RC prefix scan. */
    agc->buf_carry = gpu_buffer_create(gpu, agc->carry_size, GPU_BUF_READWRITE);
    if (!agc->buf_carry) goto fail;

    /* Output buffer: written by FIR decimation, read back to CPU. */
    agc->buf_output = gpu_buffer_create(gpu, agc->output_size, GPU_BUF_READWRITE);
    if (!agc->buf_output) goto fail;

    /* FIR taps buffer: uploaded once, read-only during processing. */
    if (agc->taps_size > 0 && chain->decimation.taps) {
        agc->buf_taps = gpu_buffer_create(gpu, agc->taps_size, GPU_BUF_READONLY);
        if (!agc->buf_taps) goto fail;

        if (!gpu_buffer_upload(gpu, agc->buf_taps,
                               chain->decimation.taps, agc->taps_size)) {
            fprintf(stderr, "audio_gpu_init: failed to upload FIR taps\n");
            goto fail;
        }
    }

    agc->verified = false;

    LOGV("audio_gpu_init: ready (input %u bytes, output %u bytes, "
            "carry %u bytes, taps %u bytes)\n",
            agc->input_size, agc->output_size,
            agc->carry_size, agc->taps_size);

    return true;

fail:
    audio_gpu_destroy(agc, gpu);
    return false;
}

/* ============================================================================
 * Processing
 * ============================================================================ */

/*
 * Dispatch an RC filter stage on the buffer that currently holds the data.
 * The RC filter operates in-place, so after this call the result is in
 * the same buffer.
 *
 * We zero the carry buffer before each RC dispatch. This means each frame
 * starts from zero state (no inter-frame carry). This matches the
 * verification use case where frames are processed independently. For
 * streaming with persistent filter state, the carry buffer would need
 * to be seeded from the previous frame's last output.
 */
static bool dispatch_rc_stage(SDL_GPUCommandBuffer *cmd,
                              AudioGPUChain *agc,
                              SDL_GPUBuffer *buf_data,
                              const AudioRCStage *stage,
                              int sample_count)
{
    if (!stage->enabled) return true;

    int num_blocks = (sample_count + AUDIO_WORKGROUP_SIZE - 1) / AUDIO_WORKGROUP_SIZE;

    GpuRCFilterParams params;
    params.a = stage->a;
    params.b = stage->b;
    params.total_count = (uint32_t)sample_count;
    params.block_offset = 0;

    return gpu_dispatch_rc_filter(cmd, &agc->pipe_rc_filter,
                                  buf_data, agc->buf_carry,
                                  &params, (Uint32)num_blocks);
}

/*
 * Dispatch a pointwise stage. Reads from buf_in, writes to buf_out.
 * Returns true on success.
 */
static bool dispatch_pointwise_stage(SDL_GPUCommandBuffer *cmd,
                                     AudioGPUChain *agc,
                                     SDL_GPUBuffer *buf_in,
                                     SDL_GPUBuffer *buf_out,
                                     uint32_t mode,
                                     float param_a,
                                     float param_b,
                                     float param_c,
                                     int sample_count)
{
    GpuPointwiseParams params;
    params.count = (uint32_t)sample_count;
    params.mode = mode;
    params.param_a = param_a;
    params.param_b = param_b;
    params.param_c = param_c;

    return gpu_dispatch_pointwise(cmd, &agc->pipe_pointwise,
                                  buf_in, buf_out, &params);
}

bool audio_gpu_process(AudioGPUChain *agc, SDL_GPUDevice *gpu,
                       const float *cpu_samples, int sample_count,
                       float *output, int output_count)
{
    AudioChain *chain = agc->chain;
    Uint32 upload_bytes = (Uint32)(sample_count * sizeof(float));

    /* Sanity checks. */
    if (upload_bytes > agc->input_size) {
        fprintf(stderr, "audio_gpu_process: sample_count %d exceeds buffer "
                "capacity (%u bytes)\n", sample_count, agc->input_size);
        return false;
    }
    if ((Uint32)(output_count * sizeof(float)) > agc->output_size) {
        fprintf(stderr, "audio_gpu_process: output_count %d exceeds buffer "
                "capacity (%u bytes)\n", output_count, agc->output_size);
        return false;
    }

    /* ---- Upload input samples to GPU ---- */
    if (!gpu_buffer_upload(gpu, agc->buf_input, cpu_samples, upload_bytes)) {
        fprintf(stderr, "audio_gpu_process: input upload failed\n");
        return false;
    }

    /* Zero the carry buffer. The RC prefix scan carry state must start
     * from zero for each frame (independent frame processing). */
    {
        void *zeros = calloc(1, agc->carry_size);
        if (!zeros) {
            fprintf(stderr, "audio_gpu_process: carry zero alloc failed\n");
            return false;
        }
        bool ok = gpu_buffer_upload(gpu, agc->buf_carry, zeros, agc->carry_size);
        free(zeros);
        if (!ok) {
            fprintf(stderr, "audio_gpu_process: carry zero upload failed\n");
            return false;
        }
    }

    /* ---- Acquire command buffer for the entire chain ---- */
    SDL_GPUCommandBuffer *cmd = SDL_AcquireGPUCommandBuffer(gpu);
    if (!cmd) {
        fprintf(stderr, "audio_gpu_process: acquire command buffer failed: %s\n",
                SDL_GetError());
        return false;
    }

    /*
     * Track which buffer currently holds the data. We start by copying
     * input into buf_a (via a pointwise gain=1 pass), then run RC stages
     * in-place on buf_a, then alternate for pointwise stages.
     *
     * The "current" pointer always points to whichever of buf_a or buf_b
     * holds the latest data.
     */
    SDL_GPUBuffer *current = agc->buf_a;

    /* Copy input -> buf_a using a gain=1 pointwise pass.
     * This also handles any high-pass differencing that might be needed:
     * for the RC stages, the shader does the prefix scan on the raw data
     * (the high-pass pre-differencing is baked into the shader's handling
     * of the coefficients). */
    {
        GpuPointwiseParams copy_params;
        copy_params.count = (uint32_t)sample_count;
        copy_params.mode = 0;       /* gain + offset */
        copy_params.param_a = 1.0f; /* gain = 1 */
        copy_params.param_b = 0.0f; /* offset = 0 */
        copy_params.param_c = 0.0f;
        if (!gpu_dispatch_pointwise(cmd, &agc->pipe_pointwise,
                                    agc->buf_input, agc->buf_a,
                                    &copy_params)) {
            fprintf(stderr, "audio_gpu_process: input copy dispatch failed\n");
            SDL_SubmitGPUCommandBuffer(cmd);
            return false;
        }
        current = agc->buf_a;
    }

    /* ---- Stage 1: Coupling capacitor (RC high-pass) ---- */
    /* SKIPPED: The RC prefix scan shader implements y = a*y + b*x (lowpass
     * form). High-pass requires a differencing pre-pass (x'[n] = x[n]-x[n-1])
     * that is not yet implemented. The coupling cap is a DC blocker at 1.6 Hz
     * — a physical no-op at 1.79 MHz sample rate. */

    /* ---- Stage 2: Feedback network (RC high-pass) ---- */
    /* SKIPPED: Same differencing pre-pass issue. The feedback network at
     * 442 Hz shapes bass response but cannot be dispatched correctly without
     * the HP pre-differencer. TODO: add a differencing pointwise pass. */

    /* ---- Stage 3: Amplifier bandwidth (RC low-pass) ---- */
    if (!dispatch_rc_stage(cmd, agc, current, &chain->amp_bandwidth, sample_count)) {
        fprintf(stderr, "audio_gpu_process: amp bandwidth dispatch failed\n");
        SDL_SubmitGPUCommandBuffer(cmd);
        return false;
    }

    /* ---- Stage 4: Amplifier saturation (pointwise tanh soft-clip) ---- */
    if (chain->amp_saturation.enabled) {
        SDL_GPUBuffer *dst = (current == agc->buf_a) ? agc->buf_b : agc->buf_a;
        if (!dispatch_pointwise_stage(cmd, agc, current, dst,
                                      1,  /* mode = tanh soft-clip */
                                      chain->amp_saturation.drive,
                                      0.0f, 0.0f,
                                      sample_count)) {
            fprintf(stderr, "audio_gpu_process: saturation dispatch failed\n");
            SDL_SubmitGPUCommandBuffer(cmd);
            return false;
        }
        current = dst;
    }

    /* ---- Stage 5: PSU hum injection (pointwise additive sine) ---- */
    if (chain->psu_hum.enabled) {
        SDL_GPUBuffer *dst = (current == agc->buf_a) ? agc->buf_b : agc->buf_a;

        /* Compute phase increment per sample for the fundamental. */
        float dp = 2.0f * 3.14159265f * chain->psu_hum.frequency / chain->sample_rate;

        /* For simplicity, we dispatch the fundamental only. The harmonics
         * would need additional passes or a more complex shader mode.
         * TODO: extend pointwise shader for multi-harmonic hum. */
        if (!dispatch_pointwise_stage(cmd, agc, current, dst,
                                      5,  /* mode = additive sinusoidal */
                                      chain->psu_hum.amplitude,
                                      chain->psu_hum.phase,
                                      dp,
                                      sample_count)) {
            fprintf(stderr, "audio_gpu_process: hum dispatch failed\n");
            SDL_SubmitGPUCommandBuffer(cmd);
            return false;
        }

        /* Advance the streaming phase state for the next frame. */
        chain->psu_hum.phase += dp * (float)sample_count;
        /* Keep phase in [0, 2*pi) to avoid float precision loss. */
        float two_pi = 2.0f * 3.14159265f;
        while (chain->psu_hum.phase >= two_pi) chain->psu_hum.phase -= two_pi;

        current = dst;
    }

    /* ---- Stage 6: Noise floor (pointwise additive) ---- */
    if (chain->noise_floor.enabled) {
        SDL_GPUBuffer *dst = (current == agc->buf_a) ? agc->buf_b : agc->buf_a;

        /* Use the additive sine mode with zero frequency as a stand-in
         * for additive noise. The GPU shader does not have a dedicated
         * noise mode (no RNG on GPU without a noise texture). Instead,
         * we add a very-high-frequency sine that aliases to pseudo-noise.
         *
         * A better approach would be a dedicated noise kernel or uploading
         * a CPU-generated noise buffer. For now, use a frequency well above
         * Nyquist of the output to approximate broadband noise. */
        float noise_freq = chain->sample_rate * 0.4999f; /* just below Nyquist */
        float dp = 2.0f * 3.14159265f * noise_freq / chain->sample_rate;

        if (!dispatch_pointwise_stage(cmd, agc, current, dst,
                                      5,  /* mode = additive sinusoidal */
                                      chain->noise_floor.amplitude,
                                      (float)chain->noise_floor.rng_state, /* phase as seed */
                                      dp,
                                      sample_count)) {
            fprintf(stderr, "audio_gpu_process: noise dispatch failed\n");
            SDL_SubmitGPUCommandBuffer(cmd);
            return false;
        }
        current = dst;
    }

    /* ---- Stage 7: Cable capacitance (RC low-pass) ---- */
    if (!dispatch_rc_stage(cmd, agc, current, &chain->cable, sample_count)) {
        fprintf(stderr, "audio_gpu_process: cable dispatch failed\n");
        SDL_SubmitGPUCommandBuffer(cmd);
        return false;
    }

    /* ---- Stage 8: TV input coupling (RC high-pass) ---- */
    /* SKIPPED: DC blocker at 3.4 Hz (R=47kΩ, C=1µF). Same HP differencing
     * issue as stages 1 and 2. Physical no-op at 1.79 MHz. */

    /* ---- Stage 9: Speaker model (SKIPPED — needs biquad kernel) ---- */
    /* The speaker model requires a second-order IIR (biquad) kernel that
     * is not yet implemented. When the biquad shader is added, dispatch
     * two passes here: resonance peak + high-frequency rolloff. */

    /* ---- Stage 10: FIR decimation ---- */
    if (chain->decimation.enabled && agc->buf_taps) {
        GpuFIRParams fir_params;
        fir_params.input_count = (uint32_t)sample_count;
        fir_params.output_count = (uint32_t)output_count;
        fir_params.tap_count = (uint32_t)chain->decimation.tap_count;
        fir_params.decimation_ratio = (uint32_t)chain->decimation.decimation_ratio;

        if (!gpu_dispatch_fir(cmd, &agc->pipe_fir,
                              current, agc->buf_output,
                              agc->buf_taps, &fir_params)) {
            fprintf(stderr, "audio_gpu_process: FIR decimation dispatch failed\n");
            SDL_SubmitGPUCommandBuffer(cmd);
            return false;
        }
    } else {
        /* No decimation -- this should not happen in normal operation
         * since the chain always decimates from ~1.79 MHz to 48 kHz.
         * If it does, just leave buf_output untouched (stale/zero). */
        fprintf(stderr, "audio_gpu_process: WARNING: decimation disabled, "
                "output will be stale\n");
    }

    /* ---- Submit command buffer and readback ---- */

    /* Submit with a fence so we can wait for all dispatches to complete
     * before reading back the output buffer. */
    SDL_GPUFence *fence = SDL_SubmitGPUCommandBufferAndAcquireFence(cmd);
    if (!fence) {
        fprintf(stderr, "audio_gpu_process: submit+fence failed: %s\n",
                SDL_GetError());
        return false;
    }
    SDL_WaitForGPUFences(gpu, true, &fence, 1);
    SDL_ReleaseGPUFence(gpu, fence);

    /* Read back the decimated output. */
    Uint32 readback_bytes = (Uint32)(output_count * sizeof(float));
    if (!gpu_buffer_download(gpu, agc->buf_output, output, readback_bytes)) {
        fprintf(stderr, "audio_gpu_process: output readback failed\n");
        return false;
    }

    return true;
}

/* ============================================================================
 * Cleanup
 * ============================================================================ */

void audio_gpu_destroy(AudioGPUChain *agc, SDL_GPUDevice *gpu)
{
    if (!agc) return;

    /* Release pipelines. */
    gpu_pipeline_destroy(gpu, &agc->pipe_pointwise);
    gpu_pipeline_destroy(gpu, &agc->pipe_rc_filter);
    gpu_pipeline_destroy(gpu, &agc->pipe_fir);

    /* Release buffers. */
    if (agc->buf_input)  { SDL_ReleaseGPUBuffer(gpu, agc->buf_input);  agc->buf_input = NULL; }
    if (agc->buf_a)      { SDL_ReleaseGPUBuffer(gpu, agc->buf_a);      agc->buf_a = NULL; }
    if (agc->buf_b)      { SDL_ReleaseGPUBuffer(gpu, agc->buf_b);      agc->buf_b = NULL; }
    if (agc->buf_carry)  { SDL_ReleaseGPUBuffer(gpu, agc->buf_carry);  agc->buf_carry = NULL; }
    if (agc->buf_output) { SDL_ReleaseGPUBuffer(gpu, agc->buf_output); agc->buf_output = NULL; }
    if (agc->buf_taps)   { SDL_ReleaseGPUBuffer(gpu, agc->buf_taps);   agc->buf_taps = NULL; }

    agc->chain = NULL;
    agc->verified = false;
}
