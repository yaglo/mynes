/* GPU backend for the streaming audio chain. Input/output rates and counts
 * are identical. One dispatch processes the short mono block in time order. */
#ifndef AUDIO_GPU_H
#define AUDIO_GPU_H
#include "audio_chain.h"
#include "gpu_compute.h"

typedef struct {
    GpuPipeline pipeline;
    SDL_GPUBuffer *input, *output;
    SDL_GPUTransferBuffer *upload, *download;
    AudioChain *chain;
    SDL_GPUFence *pending;
    int pending_count;
} AudioGPUChain;

bool audio_gpu_init(AudioGPUChain *agc, SDL_GPUDevice *gpu,
                    AudioChain *chain, const char *shader_dir);
bool audio_gpu_process(AudioGPUChain *agc, SDL_GPUDevice *gpu,
                       AudioState *state, const float *input, float *output, int count);
/* Nonblocking playback interface. One bounded in-flight block. Poll with
 * null destinations to retire an obsolete block after CPU deadline fallback. */
bool audio_gpu_begin(AudioGPUChain *agc, SDL_GPUDevice *gpu,
                     const AudioChain *chain, const AudioState *state, const float *input, int count);
int audio_gpu_poll(AudioGPUChain *agc, SDL_GPUDevice *gpu,
                   AudioState *state, float *output); /* 0 pending, 1 ready, -1 error */
void audio_gpu_destroy(AudioGPUChain *agc, SDL_GPUDevice *gpu);
#endif
