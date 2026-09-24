/*
 * GPU Compute Dispatch Infrastructure
 * =====================================
 *
 * Loads SPIR-V shaders via SDL3's SDL_GPU API, creates compute pipelines,
 * manages GPU storage buffers, and dispatches compute work. This is the
 * foundation layer that the signal chain runner composes into multi-stage
 * processing pipelines.
 *
 * SDL3 GPU compute model:
 *   1. Create a compute pipeline from SPIR-V bytecode
 *   2. Acquire a command buffer
 *   3. Begin a compute pass (binds read-write output buffers)
 *   4. Bind the pipeline
 *   5. Bind read-only input buffers
 *   6. Push uniform data
 *   7. Dispatch workgroups
 *   8. End compute pass
 *   9. Submit command buffer (optionally with fence for timing)
 *
 * SPIR-V binding layout (per SDL_CreateGPUComputePipeline docs):
 *   Set 0: Sampled textures, then readonly storage textures,
 *          then readonly storage buffers
 *   Set 1: Read-write storage textures, then read-write storage buffers
 *   Set 2: Uniform buffers
 *
 * IMPORTANT: When dispatches write to the same resource region, you MUST
 * end the compute pass and begin a new one between them. SDL3 does NOT
 * provide implicit barriers within a compute pass.
 */

#ifndef GPU_COMPUTE_H
#define GPU_COMPUTE_H

#include <SDL3/SDL.h>
#include <stdbool.h>
#include <stdint.h>

/* ============================================================================
 * Compute pipeline (wraps SDL_GPUComputePipeline + metadata)
 * ============================================================================ */

typedef struct {
    SDL_GPUComputePipeline *pipeline;
    Uint32 threadcount_x;       /* workgroup size X (from shader) */
    Uint32 threadcount_y;       /* workgroup size Y (from shader) */
    Uint32 threadcount_z;       /* workgroup size Z (from shader) */
    const char *name;           /* debug label (owned, freed on destroy) */
} GpuPipeline;

/* Resource counts for pipeline creation. Must match the shader's
 * descriptor layout exactly. */
typedef struct {
    Uint32 num_readonly_storage_buffers;
    Uint32 num_readwrite_storage_buffers;
    Uint32 num_uniform_buffers;
    /* Less common (zero for our signal processing shaders): */
    Uint32 num_samplers;
    Uint32 num_readonly_storage_textures;
    Uint32 num_readwrite_storage_textures;
} GpuPipelineResources;

/* Load a SPIR-V file from disk and create a compute pipeline.
 *
 * spv_path:     path to the .spv file
 * entrypoint:   shader entry point (typically "main")
 * threadcount:  workgroup dimensions (x, y, z) -- must match the shader's
 *               local_size declarations
 * resources:    descriptor counts matching the shader layout
 * name:         debug label (copied internally)
 *
 * Returns true on success. On failure, pipeline->pipeline is NULL and
 * an error is logged to stderr. */
bool gpu_pipeline_create(
    SDL_GPUDevice *device,
    GpuPipeline *pipeline,
    const char *spv_path,
    const char *entrypoint,
    Uint32 threadcount_x,
    Uint32 threadcount_y,
    Uint32 threadcount_z,
    const GpuPipelineResources *resources,
    const char *name);

/* Release a compute pipeline. Safe to call on a zeroed struct. */
void gpu_pipeline_destroy(SDL_GPUDevice *device, GpuPipeline *pipeline);

/* ============================================================================
 * Storage buffer management
 * ============================================================================ */

/* Usage flags for gpu_buffer_create. These map to SDL_GPUBufferUsageFlags
 * but are named for clarity in our signal processing context. */
typedef enum {
    GPU_BUF_READONLY  = 0,   /* COMPUTE_STORAGE_READ only */
    GPU_BUF_WRITEONLY = 1,   /* COMPUTE_STORAGE_WRITE only */
    GPU_BUF_READWRITE = 2,   /* COMPUTE_STORAGE_READ | COMPUTE_STORAGE_WRITE */
} GpuBufferUsage;

/* Create a GPU storage buffer for compute shader access.
 *
 * size_bytes: buffer size in bytes
 * usage:      how the buffer will be used in compute passes
 *
 * Returns the buffer handle, or NULL on failure. */
SDL_GPUBuffer *gpu_buffer_create(
    SDL_GPUDevice *device,
    Uint32 size_bytes,
    GpuBufferUsage usage);

/* Upload CPU data to a GPU buffer. Internally creates a transfer buffer,
 * maps it, copies, and submits a copy pass. Blocks until the upload
 * command buffer is submitted (does not wait for GPU completion).
 *
 * This is a convenience for initialization and per-frame uploads.
 * For streaming uploads in the render loop, the caller should manage
 * transfer buffers directly for better pipelining. */
bool gpu_buffer_upload(
    SDL_GPUDevice *device,
    SDL_GPUBuffer *buffer,
    const void *data,
    Uint32 size_bytes);

/* Download GPU buffer contents to CPU memory. Creates a transfer buffer,
 * submits a download copy pass, waits for completion via fence, then
 * copies to the output pointer. This BLOCKS until the GPU is done.
 *
 * Use for readback of final results (e.g., decimated audio output).
 * Do NOT use in the hot path -- readback stalls the pipeline. */
bool gpu_buffer_download(
    SDL_GPUDevice *device,
    SDL_GPUBuffer *buffer,
    void *out_data,
    Uint32 size_bytes);

/* ============================================================================
 * Compute dispatch
 * ============================================================================
 *
 * A dispatch binds one pipeline, some read-only input buffers, some
 * read-write output buffers, optional uniform data, and fires workgroups.
 *
 * The caller provides an already-acquired SDL_GPUCommandBuffer. This allows
 * batching multiple dispatches into a single command buffer submission,
 * which is required for multi-stage chains where each stage's output feeds
 * the next stage's input.
 *
 * SYNCHRONIZATION: Between dispatches that write to a buffer and subsequent
 * dispatches that read from it, you MUST end and begin a new compute pass.
 * SDL3 does not have explicit barriers -- pass boundaries are the barrier.
 */

/* Maximum buffer bindings per dispatch. */
#define GPU_MAX_READONLY_BUFFERS   8
#define GPU_MAX_READWRITE_BUFFERS  4
#define GPU_MAX_UNIFORM_SLOTS      4

/* Descriptor for a single compute dispatch. */
typedef struct {
    /* Pipeline to dispatch. */
    const GpuPipeline *pipeline;

    /* Read-only storage buffers (bound via SDL_BindGPUComputeStorageBuffers).
     * These are inputs the shader reads but does not write. */
    SDL_GPUBuffer *readonly_buffers[GPU_MAX_READONLY_BUFFERS];
    Uint32 num_readonly_buffers;

    /* Read-write storage buffers (bound at compute pass begin via
     * SDL_BeginGPUComputePass's storage_buffer_bindings parameter).
     * These are outputs the shader writes to. The shader may also
     * read from them if the pipeline was created with that capability. */
    SDL_GPUBuffer *readwrite_buffers[GPU_MAX_READWRITE_BUFFERS];
    Uint32 num_readwrite_buffers;

    /* Uniform data (pushed via SDL_PushGPUComputeUniformData). Each slot
     * is a contiguous block of data respecting std140 alignment. */
    struct {
        const void *data;
        Uint32 size;
    } uniforms[GPU_MAX_UNIFORM_SLOTS];
    Uint32 num_uniforms;

    /* Dispatch dimensions in workgroups (not threads). */
    Uint32 groupcount_x;
    Uint32 groupcount_y;
    Uint32 groupcount_z;
} GpuDispatchDesc;

/* Execute a single compute dispatch within a NEW compute pass.
 *
 * This function begins a compute pass, binds everything, dispatches, and
 * ends the compute pass. The pass boundary acts as a barrier, making the
 * output safe to read in the next dispatch.
 *
 * cmd: an already-acquired command buffer (from SDL_AcquireGPUCommandBuffer).
 *      The caller is responsible for submitting it afterward.
 *
 * Returns true on success. */
bool gpu_dispatch(SDL_GPUCommandBuffer *cmd, const GpuDispatchDesc *desc);

/* ============================================================================
 * Convenience dispatchers for the signal chain primitives
 * ============================================================================
 *
 * These are thin wrappers that fill in a GpuDispatchDesc and call
 * gpu_dispatch(). They match the shader interfaces defined in
 * shaders/compute/ (.comp.glsl files).
 */

/* Pointwise transfer function parameters (matches pointwise.comp.glsl). */
typedef struct {
    uint32_t count;         /* number of samples */
    uint32_t mode;          /* 0=gain, 1=tanh, 2=clip, 3=gamma, 4=cubic, 5=sine */
    float param_a;
    float param_b;
    float param_c;
} GpuPointwiseParams;

bool gpu_dispatch_pointwise(
    SDL_GPUCommandBuffer *cmd,
    const GpuPipeline *pipeline,
    SDL_GPUBuffer *buf_in,
    SDL_GPUBuffer *buf_out,
    const GpuPointwiseParams *params);

/* RC filter parameters (matches rc_filter.comp.glsl). */
typedef struct {
    float a;                    /* feedback coefficient */
    float b;                    /* input coefficient */
    uint32_t total_count;       /* total samples */
    uint32_t block_offset;      /* not used in sequential mode */
    uint32_t samples_per_line;  /* samples per scanline (2048 for NTSC) */
    uint32_t num_lines;         /* number of scanlines (240) */
    float nonlinear_tau_samples; /* NTSC output pole at reference white */
    /* NES-001 output follower (tools/circuits/nes001_video_chain.cir): the
     * PNP pulls the emitter down at once, but when the PPU steps up only
     * R2 (510) charges C5 (330 pF) towards +5 V. follower_k is exp(-1/tau)
     * per sample (0 = off); follower_headroom is (Vcc - Ve_blank) in units
     * of the blank-to-white swing, 2.0 for 5 V, a 2.0 V emitter at blanking
     * and a 1.5 V swing. */
    float follower_k;
    float follower_headroom;
    float pad[2];
} GpuRCFilterParams;

bool gpu_dispatch_rc_filter(
    SDL_GPUCommandBuffer *cmd,
    const GpuPipeline *pipeline,
    SDL_GPUBuffer *buf_data,
    SDL_GPUBuffer *buf_carry,
    const GpuRCFilterParams *params,
    Uint32 num_workgroups);

/* FIR convolution parameters (matches fir.comp.glsl). */
typedef struct {
    uint32_t input_count;
    uint32_t output_count;
    uint32_t tap_count;
    uint32_t decimation_ratio;
    uint32_t samples_per_line;  /* 0=global FIR, >0=per-scanline boundary clamp */
} GpuFIRParams;

bool gpu_dispatch_fir(
    SDL_GPUCommandBuffer *cmd,
    const GpuPipeline *pipeline,
    SDL_GPUBuffer *buf_in,
    SDL_GPUBuffer *buf_out,
    SDL_GPUBuffer *buf_taps,
    const GpuFIRParams *params);

/* Delay / ghost / comb parameters (matches delay.comp.glsl). */
typedef struct {
    uint32_t count;
    uint32_t mode;          /* 0=delay, 1=ghost, 2=comb_add, 3=comb_sub */
    int32_t  delay_samples;
    float    level;
} GpuDelayParams;

bool gpu_dispatch_delay(
    SDL_GPUCommandBuffer *cmd,
    const GpuPipeline *pipeline,
    SDL_GPUBuffer *buf_in,
    SDL_GPUBuffer *buf_out,
    SDL_GPUBuffer *buf_prev,       /* secondary input for comb modes */
    const GpuDelayParams *params);

/* Baseband RF impairment model; timing includes horizontal blanking. */
typedef struct {
    uint32_t count, samples_per_line;
    float noise_amplitude, hum_amplitude;
    uint32_t frame_seed, full_line_samples;
    float sample_rate, hum_phase, hum_hz;
} GpuRFParams;
typedef struct { uint32_t count, samples_per_line, tap_count, reserved; } GpuRFIFParams;
/* VHS deck, shared by vhs_tape (V1) and vhs_playback (V2). Units are IRE
 * referenced to the input sync, Hz and 12 fsc samples; see vhs_deck.c. */
typedef struct {
    uint32_t frame, defect_count, doc, total;
    float in_gain, dark_clip, white_clip, f_sync;
    float hz_per_pct, sample_rate, mod_noise_sigma, rf_noise_sigma;
    float env_norm, doc_on, doc_off, chroma_noise_sigma;
    float burst_target, burst_norm, spacing_db, colour_under_hz;
    float canceller_limit, sharpness, detail_limit, y_delay;
    float c_delay, out_scale, playback_acc, click_scale;
    float sharp_d, reserved0, reserved1, reserved2;
} GpuVHSParams;

/* Horizontal AFC is independent of the colour-burst PLL. Zero response
 * selects the legacy loop, including its frame acquisition behaviour.
 * h_pll selects a second-order loop (proportional h_kp, integral h_ki per
 * line) whose detector gain is h_vblank_gain for h_vblank_lines from
 * vertical sync; it keeps its state across frames. clamp_gain is the
 * keyed black clamp's charge per line, 1 - exp(-1 / clamp lines). */
typedef struct {
    uint32_t count, full_width, samples_per_dot, region;
    float h_response, h_kp, h_ki, h_vblank_gain;
    uint32_t h_pll, h_vblank_lines; float clamp_gain; uint32_t reserved1;
} GpuReceiverPLLParams;

/* Comb filter Y/C separator parameters (matches comb_filter.comp.glsl). */
typedef struct {
    uint32_t count;             /* total samples */
    uint32_t samples_per_line;  /* 2048 (NTSC) or 2560 (PAL) */
    uint32_t mode;              /* 0=bypass, 1=1line, 2=2line, 3=3line */
    float    blend;             /* comb strength (0..1) */
    uint32_t delay_samples;     /* receiver 1H delay; 0 = samples_per_line */
} GpuCombParams;

/* AGC parameters (matches agc.comp.glsl). */
typedef struct {
    uint32_t total_count;       /* total samples */
    uint32_t samples_per_line;  /* 2048 (NTSC) or 2560 (PAL) */
    uint32_t num_lines;         /* 240 */
    float    target_level;      /* desired peak amplitude (0.85-0.95) */
    float    attack_coeff;      /* gain decrease speed (0.1=fast) */
    float    release_coeff;     /* gain increase speed (0.02=slow) */
    float    min_gain;          /* minimum gain floor (0.5) */
    float    max_gain;          /* maximum gain ceiling (2.0) */
} GpuAGCParams;

/* Modulator / demodulator parameters. modulator.comp.glsl reads the first
 * seven; receiver_demod.comp.glsl reads them all. */
typedef struct {
    uint32_t count;
    uint32_t mode;              /* 0=cos, 1=sin, 2=AM, 3=IQ */
    float    phase;
    float    dp;                /* phase increment per sample */
    float    param_a;           /* mod_index (mode 2) or gain (mode 3) */
    uint32_t samples_per_line;  /* 0=continuous, >0=reset phase per scanline */
    float    line_phase_inc;    /* phase advance per scanline (radians) */
    float    burst_reference;   /* burst amplitude the ACC holds, blanking to white = 1 */
} GpuModulatorParams;

bool gpu_dispatch_modulator(
    SDL_GPUCommandBuffer *cmd,
    const GpuPipeline *pipeline,
    SDL_GPUBuffer *buf_in,
    SDL_GPUBuffer *buf_out,
    SDL_GPUBuffer *buf_out2,       /* Q channel output for IQ mode */
    const GpuModulatorParams *params);

/* ============================================================================
 * Performance measurement
 * ============================================================================
 *
 * SDL3 GPU has no timestamp query support. We measure dispatch latency
 * by submitting the command buffer with a fence, waiting for it, and
 * measuring the wall-clock time. This is coarse but sufficient for
 * profiling the signal chain.
 *
 * For per-dispatch timing, submit each dispatch as its own command buffer.
 * For aggregate timing, submit the entire chain and measure total time.
 */

typedef struct {
    const char *name;           /* dispatch label */
    double elapsed_us;          /* wall-clock microseconds (GPU + overhead) */
} GpuTimingEntry;

#define GPU_MAX_TIMING_ENTRIES 32

typedef struct {
    GpuTimingEntry entries[GPU_MAX_TIMING_ENTRIES];
    int count;
    double total_us;            /* sum of all entries */
} GpuTimingLog;

/* Initialize / reset a timing log. */
void gpu_timing_reset(GpuTimingLog *log);

/* Submit a command buffer with a fence, wait for completion, and record
 * the elapsed time. The command buffer is consumed (submitted) by this
 * call -- the caller must NOT submit it separately.
 *
 * device: GPU device for fence operations
 * cmd:    command buffer to submit and time
 * name:   label for this timing entry
 * log:    timing log to append to (NULL to skip recording)
 *
 * Returns true on success. */
bool gpu_submit_and_time(
    SDL_GPUDevice *device,
    SDL_GPUCommandBuffer *cmd,
    const char *name,
    GpuTimingLog *log);

/* Print a timing log summary to stderr. */
void gpu_timing_print(const GpuTimingLog *log);

/* ============================================================================
 * Utility: compute workgroup count for 1D dispatch
 * ============================================================================ */

/* Returns ceil(total_elements / workgroup_size). */
static inline Uint32 gpu_workgroup_count(Uint32 total_elements,
                                          Uint32 workgroup_size) {
    return (total_elements + workgroup_size - 1) / workgroup_size;
}

#endif /* GPU_COMPUTE_H */
