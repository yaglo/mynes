#include "audio_gpu.h"
#include <stdio.h>
#include <string.h>

/* The input block carries the state, the samples and the block's aux
 * signal (RF sound buzz); the output the state and the samples. */
#define BLOCK_BYTES (sizeof(AudioState) + 2 * AUDIO_BLOCK_CAPACITY * sizeof(float))
_Static_assert(sizeof(AudioState) == 72, "audio shader state layout");
_Static_assert(sizeof(AudioParams) == 224, "audio shader uniform layout");

bool audio_gpu_init(AudioGPUChain *a, SDL_GPUDevice *gpu,
                    AudioChain *chain, const char *shader_dir) {
    memset(a, 0, sizeof(*a));
    a->chain = chain;
    char path[1024];
    snprintf(path, sizeof(path), "%s/audio_stream.comp.spv", shader_dir);
    GpuPipelineResources res = { .num_readonly_storage_buffers = 1,
        .num_readwrite_storage_buffers = 1, .num_uniform_buffers = 1 };
    if (!gpu_pipeline_create(gpu, &a->pipeline, path, "main", 1, 1, 1, &res, "audio_stream")) goto fail;
    a->input = gpu_buffer_create(gpu, BLOCK_BYTES, GPU_BUF_READONLY);
    a->output = gpu_buffer_create(gpu, BLOCK_BYTES, GPU_BUF_READWRITE);
    SDL_GPUTransferBufferCreateInfo info = { .usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD, .size = BLOCK_BYTES };
    a->upload = SDL_CreateGPUTransferBuffer(gpu, &info);
    info.usage = SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD;
    a->download = SDL_CreateGPUTransferBuffer(gpu, &info);
    if (!a->input || !a->output || !a->upload || !a->download) goto fail;
    return true;
fail:
    audio_gpu_destroy(a, gpu);
    return false;
}

bool audio_gpu_begin(AudioGPUChain *a, SDL_GPUDevice *gpu,
                     const AudioChain *chain, const AudioState *state, const float *input,
                     const float *aux, int count) {
    if (a->pending || count <= 0 || count > AUDIO_BLOCK_CAPACITY || !state || !input) return false;
    Uint32 bytes = sizeof(*state) + count * sizeof(float);
    Uint32 in_bytes = bytes + (aux ? count * sizeof(float) : 0);
    void *mapped = SDL_MapGPUTransferBuffer(gpu, a->upload, false);
    if (!mapped) return false;
    memcpy(mapped, state, sizeof(*state));
    memcpy((char *)mapped + sizeof(*state), input, count * sizeof(float));
    if (aux) memcpy((char *)mapped + bytes, aux, count * sizeof(float));
    SDL_UnmapGPUTransferBuffer(gpu, a->upload);
    SDL_GPUCommandBuffer *cmd = SDL_AcquireGPUCommandBuffer(gpu);
    if (!cmd) return false;
    SDL_GPUCopyPass *copy = SDL_BeginGPUCopyPass(cmd);
    SDL_GPUTransferBufferLocation upload = { .transfer_buffer = a->upload };
    SDL_GPUBufferRegion in = { .buffer = a->input, .size = in_bytes };
    SDL_UploadToGPUBuffer(copy, &upload, &in, false);
    SDL_EndGPUCopyPass(copy);
    AudioParams params;
    audio_chain_params(chain, count, &params);
    if (!aux) params.flags &= ~32u;
    GpuDispatchDesc desc = { .pipeline = &a->pipeline,
        .readonly_buffers = {a->input}, .num_readonly_buffers = 1,
        .readwrite_buffers = {a->output}, .num_readwrite_buffers = 1,
        .uniforms = {{&params, sizeof(params)}}, .num_uniforms = 1,
        .groupcount_x = 1, .groupcount_y = 1, .groupcount_z = 1 };
    if (!gpu_dispatch(cmd, &desc)) { SDL_CancelGPUCommandBuffer(cmd); return false; }
    copy = SDL_BeginGPUCopyPass(cmd);
    SDL_GPUBufferRegion out = { .buffer = a->output, .size = bytes };
    SDL_GPUTransferBufferLocation download = { .transfer_buffer = a->download };
    SDL_DownloadFromGPUBuffer(copy, &out, &download);
    SDL_EndGPUCopyPass(copy);
    a->pending = SDL_SubmitGPUCommandBufferAndAcquireFence(cmd);
    a->pending_count = count;
    return a->pending != NULL;
}

int audio_gpu_poll(AudioGPUChain *a, SDL_GPUDevice *gpu, AudioState *state, float *output) {
    if (!a->pending) return -1;
    if (!SDL_QueryGPUFence(gpu, a->pending)) return 0;
    SDL_ReleaseGPUFence(gpu, a->pending);
    a->pending = NULL;
    if (!state || !output) return 1;
    void *mapped = SDL_MapGPUTransferBuffer(gpu, a->download, false);
    if (!mapped) return -1;
    memcpy(state, mapped, sizeof(*state));
    memcpy(output, (char *)mapped + sizeof(*state), a->pending_count * sizeof(float));
    SDL_UnmapGPUTransferBuffer(gpu, a->download);
    return 1;
}

/* Blocking helper is retained only for offline regression comparisons. */
bool audio_gpu_process(AudioGPUChain *a, SDL_GPUDevice *gpu,
                       AudioState *state, const float *input, const float *aux,
                       float *output, int count) {
    if (count == 0) return true;
    if (!output || !audio_gpu_begin(a, gpu, a->chain, state, input, aux, count)) return false;
    if (!SDL_WaitForGPUFences(gpu, true, &a->pending, 1)) return false;
    return audio_gpu_poll(a, gpu, state, output) == 1;
}

void audio_gpu_destroy(AudioGPUChain *a, SDL_GPUDevice *gpu) {
    if (a->pending) {
        SDL_WaitForGPUFences(gpu, true, &a->pending, 1);
        SDL_ReleaseGPUFence(gpu, a->pending);
    }
    gpu_pipeline_destroy(gpu, &a->pipeline);
    if (a->input) SDL_ReleaseGPUBuffer(gpu, a->input);
    if (a->output) SDL_ReleaseGPUBuffer(gpu, a->output);
    if (a->upload) SDL_ReleaseGPUTransferBuffer(gpu, a->upload);
    if (a->download) SDL_ReleaseGPUTransferBuffer(gpu, a->download);
    memset(a, 0, sizeof(*a));
}
