/* Two producers share a device, as video and GPU audio do in playback.
 * Each fence must keep ownership until its caller releases it, including
 * while another thread cleans up and reuses command buffers. */
#include <SDL3/SDL.h>
#include <stdio.h>

typedef struct { SDL_GPUDevice *gpu; unsigned id; } Producer;

static int produce(void *user) {
    Producer *p = user;
    SDL_GPUTransferBufferCreateInfo ti = { .usage=SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD, .size=4096 };
    SDL_GPUTransferBuffer *upload = SDL_CreateGPUTransferBuffer(p->gpu, &ti);
    ti.usage = SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD;
    SDL_GPUTransferBuffer *download = SDL_CreateGPUTransferBuffer(p->gpu, &ti);
    SDL_GPUBufferCreateInfo bi = { .usage=SDL_GPU_BUFFERUSAGE_COMPUTE_STORAGE_READ, .size=4096 };
    SDL_GPUBuffer *buffer = SDL_CreateGPUBuffer(p->gpu, &bi);
    if (!upload || !download || !buffer) return 1;
    int failed = 0;
    for (unsigned n=1; n<=10000; n++) {
        Uint32 expected = (p->id << 24) | n;
        Uint32 *mapped = SDL_MapGPUTransferBuffer(p->gpu, upload, false);
        if (!mapped) { failed=1; break; }
        for (int i=0; i<1024; i++) mapped[i]=expected;
        SDL_UnmapGPUTransferBuffer(p->gpu, upload);
        SDL_GPUCommandBuffer *cmd = SDL_AcquireGPUCommandBuffer(p->gpu);
        if (!cmd) { failed=1; break; }
        SDL_GPUCopyPass *copy = SDL_BeginGPUCopyPass(cmd);
        SDL_GPUTransferBufferLocation src = { .transfer_buffer=upload };
        SDL_GPUBufferRegion region = { .buffer=buffer, .size=4096 };
        SDL_GPUTransferBufferLocation dst = { .transfer_buffer=download };
        SDL_UploadToGPUBuffer(copy, &src, &region, false);
        SDL_DownloadFromGPUBuffer(copy, &region, &dst);
        SDL_EndGPUCopyPass(copy);
        SDL_GPUFence *fence = SDL_SubmitGPUCommandBufferAndAcquireFence(cmd);
        if (!fence) { failed=1; break; }
        Uint64 deadline = SDL_GetTicks()+2000;
        while (!SDL_QueryGPUFence(p->gpu, fence)) {
            if (SDL_GetTicks() > deadline) {
                fprintf(stderr,"Producer %u fence timed out at %u\n",p->id,n);
                return 1; /* Do not wait again in teardown after a timeout. */
            }
            SDL_DelayNS(10000);
        }
        SDL_ReleaseGPUFence(p->gpu, fence);
        mapped = SDL_MapGPUTransferBuffer(p->gpu, download, false);
        if (!mapped) { failed=1; break; }
        for (int i=0; i<1024; i++) if (mapped[i]!=expected) { failed=1; break; }
        SDL_UnmapGPUTransferBuffer(p->gpu, download);
        if (failed) { fprintf(stderr,"Producer %u premature fence at %u\n",p->id,n); break; }
    }
    SDL_ReleaseGPUBuffer(p->gpu, buffer);
    SDL_ReleaseGPUTransferBuffer(p->gpu, upload);
    SDL_ReleaseGPUTransferBuffer(p->gpu, download);
    return failed;
}

int main(void) {
    if (!SDL_Init(SDL_INIT_VIDEO)) return 1;
    SDL_GPUDevice *gpu = SDL_CreateGPUDevice(SDL_GPU_SHADERFORMAT_SPIRV | SDL_GPU_SHADERFORMAT_MSL, true, NULL);
    if (!gpu) { fprintf(stderr,"GPU device: %s\n",SDL_GetError()); return 1; }
    Producer producers[2] = {{gpu,1},{gpu,2}};
    SDL_Thread *worker = SDL_CreateThread(produce,"fence producer",&producers[0]);
    if (!worker) return 1;
    int first=0, second=produce(&producers[1]);
    SDL_WaitThread(worker,&first);
    /* Avoid a second indefinite backend wait when the regression fails. */
    if (first || second) return 1;
    SDL_DestroyGPUDevice(gpu);
    SDL_Quit();
    puts("20000 concurrent fence/readback cycles passed");
    return 0;
}
