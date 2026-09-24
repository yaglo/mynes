/* GPU resources and chain stages of the VHS deck (vhs_deck.h). */
#ifndef VHS_GPU_H
#define VHS_GPU_H

#include "signal_chain.h"
#include "vhs_deck.h"

typedef struct {
    VHSDeck *deck;
    SDL_GPUBuffer *luma, *chroma, *mask, *coeff, *lines, *defects;
    SDL_GPUTransferBuffer *transfer;
    int stage_tape, stage_playback;
} VHSGpu;

/* Allocate the tape buffers and add the two stages to sc (disabled). */
bool vhs_gpu_init(VHSGpu *g, SignalChain *sc, SDL_GPUDevice *gpu);
/* Redesign the deck for p and upload its filter sections. */
bool vhs_gpu_configure(VHSGpu *g, SignalChain *sc, SDL_GPUDevice *gpu, const VHSParams *p, double fs);
/* Build and upload the timing table and dropouts for emulated frame
 * number frame, in cmd before the chain runs. sync_depth is the source's
 * sync below blanking in chain units (264/788 for the 2C02, 0.4 for an
 * encoder IC): the deck's keyed AGC brings it to -40 IRE. */
bool vhs_gpu_frame(VHSGpu *g, SignalChain *sc, SDL_GPUDevice *gpu, SDL_GPUCommandBuffer *cmd, uint32_t frame,
                   float sync_depth);
void vhs_gpu_set_enabled(VHSGpu *g, SignalChain *sc, bool enabled);
bool vhs_gpu_enabled(const VHSGpu *g, const SignalChain *sc);
void vhs_gpu_destroy(VHSGpu *g, SDL_GPUDevice *gpu);

#endif
