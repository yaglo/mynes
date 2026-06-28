/*
 * Audio GPU Chain — GPU Signal Chain Orchestration
 * ==================================================
 *
 * Wires the audio signal chain (audio_chain.h) onto GPU compute
 * dispatches (gpu_compute.h). Each AudioChain stage maps to one
 * gpu_dispatch_* call. Ping-pong intermediate buffers shuttle data
 * between stages; the RC filter operates in-place so we copy into
 * the target buffer before dispatching.
 *
 * Processing order (matches audio_chain.h exactly):
 *   Upload input -> buf_input
 *   Stage 1: RC highpass  (coupling cap)       in-place on buf_a
 *   Stage 2: RC highpass  (feedback network)    in-place on buf_a
 *   Stage 3: RC lowpass   (amp bandwidth)       in-place on buf_a
 *   Stage 4: pointwise    (saturation)          buf_a -> buf_b
 *   Stage 5: pointwise    (PSU hum)             buf_b -> buf_a  (or A->B if stage 4 skipped)
 *   Stage 6: pointwise    (noise floor)         swap direction
 *   Stage 7: RC lowpass   (cable capacitance)   in-place on current
 *   Stage 8: RC highpass  (TV input coupling)   in-place on current
 *   Stage 9: (speaker — skip, needs biquad)
 *   Stage 10: FIR decimate (current -> buf_output)
 *   Readback buf_output -> CPU
 *
 * The RC filter kernel works IN-PLACE on its data buffer, so there
 * is no separate input/output. We copy data into the target buffer
 * before each RC dispatch when necessary, keeping the other buffer
 * available as scratch.
 *
 * IMPORTANT: This is the VERIFICATION path. CPU audio is the primary
 * real-time output. The GPU chain exists for A/B testing, offline
 * rendering, and future latency-hiding.
 */

#ifndef AUDIO_GPU_H
#define AUDIO_GPU_H

#include "audio_chain.h"
#include "gpu_compute.h"
#include <stdbool.h>

/* ============================================================================
 * GPU audio chain state
 * ============================================================================ */

typedef struct {
    /* --- Compute pipelines (one per kernel type) --- */
    GpuPipeline pipe_pointwise;     /* pointwise.comp.spv (256 threads) */
    GpuPipeline pipe_rc_filter;     /* rc_filter.comp.spv (1024 threads) */
    GpuPipeline pipe_fir;           /* fir.comp.spv (256 threads) */

    /* --- GPU storage buffers --- */
    /* Input: raw APU samples uploaded from CPU each frame. */
    SDL_GPUBuffer *buf_input;

    /* Ping-pong intermediates (same size as input). The RC filter
     * operates in-place, so we copy into the active buffer before
     * dispatch. Pointwise dispatches write from one to the other. */
    SDL_GPUBuffer *buf_a;
    SDL_GPUBuffer *buf_b;

    /* RC filter carry buffer (inter-block prefix scan state).
     * Size: max_blocks * sizeof(float) * 2 (vec2 per block). */
    SDL_GPUBuffer *buf_carry;

    /* Output: decimated audio (much smaller than input). */
    SDL_GPUBuffer *buf_output;

    /* FIR taps buffer (uploaded once from AudioChain.decimation.taps). */
    SDL_GPUBuffer *buf_taps;

    /* --- Buffer sizes (bytes) --- */
    Uint32 input_size;          /* max_samples * sizeof(float) */
    Uint32 output_size;         /* max_output_samples * sizeof(float) */
    Uint32 carry_size;          /* max_blocks * sizeof(float) * 2 */
    Uint32 taps_size;           /* tap_count * sizeof(float) */

    /* --- Audio chain configuration (pointer, not owned) --- */
    AudioChain *chain;

    /* --- Verification flag --- */
    /* Set to true after the first successful A/B comparison with CPU
     * reference output. Cleared if chain parameters change. */
    bool verified;
} AudioGPUChain;

/* ============================================================================
 * Lifecycle
 * ============================================================================ */

/* Initialize the GPU audio chain: load compute pipelines from SPIR-V,
 * allocate GPU buffers sized for the chain's sample rate and frame size.
 *
 * gpu:        SDL3 GPU device (must already be created)
 * chain:      audio chain with prepared coefficients (audio_chain_prepare
 *             must have been called). The pointer is stored, not copied.
 * shader_dir: directory containing .spv files (e.g., "shaders/compute")
 *
 * Returns true on success. On failure, the struct is zeroed and an error
 * is logged to stderr. */
bool audio_gpu_init(AudioGPUChain *agc, SDL_GPUDevice *gpu,
                    AudioChain *chain, const char *shader_dir);

/* Process one frame of audio through the GPU signal chain.
 *
 * cpu_samples:  raw APU output from the CPU (float32, one per CPU cycle)
 * sample_count: number of input samples (typically ~29829 NTSC, ~33252 PAL)
 * output:       destination for decimated output (caller-allocated)
 * output_count: number of output samples expected (typically 800 or 960)
 *
 * This function uploads, dispatches the full chain, reads back, and blocks
 * until the GPU is done. It is NOT suitable for real-time use -- it stalls
 * the CPU on GPU readback. Use for verification and offline rendering.
 *
 * Returns true on success. */
bool audio_gpu_process(AudioGPUChain *agc, SDL_GPUDevice *gpu,
                       const float *cpu_samples, int sample_count,
                       float *output, int output_count);

/* Release all GPU resources (pipelines, buffers). Safe to call on a
 * zeroed struct. Does NOT free the AudioChain pointer. */
void audio_gpu_destroy(AudioGPUChain *agc, SDL_GPUDevice *gpu);

#endif /* AUDIO_GPU_H */
