/* GPU resources and chain stages of the VHS deck. */
#include "vhs_gpu.h"
#include <stdlib.h>
#include <string.h>

#define TABLE_BYTES (VHS_TABLE_LINES * sizeof(VHSLineEntry))
#define DEFECT_BYTES (VHS_MAX_DEFECTS * sizeof(VHSDefect))

bool vhs_gpu_init(VHSGpu *g, SignalChain *sc, SDL_GPUDevice *gpu) {
    memset(g, 0, sizeof(*g));
    g->stage_tape = g->stage_playback = -1;
    const Uint32 samples = VHS_LINES * VHS_SPL;
    g->deck = calloc(1, sizeof(VHSDeck));
    g->luma = gpu_buffer_create(gpu, samples * sizeof(float), GPU_BUF_READWRITE);
    g->chroma = gpu_buffer_create(gpu, VHS_LINES * VHS_SPL3 * 2 * sizeof(float), GPU_BUF_READWRITE);
    g->mask = gpu_buffer_create(gpu, VHS_LINES * VHS_MASK_WORDS * sizeof(uint32_t), GPU_BUF_READWRITE);
    g->coeff = gpu_buffer_create(gpu, VHS_COEFF_VEC4 * 4 * sizeof(float), GPU_BUF_READONLY);
    g->lines = gpu_buffer_create(gpu, TABLE_BYTES, GPU_BUF_READONLY);
    g->defects = gpu_buffer_create(gpu, DEFECT_BYTES, GPU_BUF_READONLY);
    SDL_GPUTransferBufferCreateInfo info = {.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD,
                                            .size = TABLE_BYTES + DEFECT_BYTES};
    g->transfer = SDL_CreateGPUTransferBuffer(gpu, &info);
    if (!g->deck || !g->luma || !g->chroma || !g->mask || !g->coeff || !g->lines || !g->defects || !g->transfer)
        return false;
    /* The mask is read one line back by the compensator before the first
     * frame has written every line. */
    uint32_t *zero = calloc(VHS_LINES * VHS_MASK_WORDS, sizeof(uint32_t));
    bool ok = zero && gpu_buffer_upload(gpu, g->mask, zero, VHS_LINES * VHS_MASK_WORDS * sizeof(uint32_t));
    free(zero);
    if (!ok) return false;

    GpuVHSParams p = {0};
    g->stage_tape = chain_add_stage(sc, "VHS tape (record, transport, FM)", CHAIN_KERNEL_VHS_TAPE,
                                    &p, sizeof(p), VHS_LINES, 1);
    g->stage_playback = chain_add_stage(sc, "VHS playback (DOC, luma, 1H comb)", CHAIN_KERNEL_VHS_PLAYBACK,
                                        &p, sizeof(p), VHS_LINES, 1);
    if (g->stage_tape < 0 || g->stage_playback < 0) return false;
    ChainStage *t = &sc->stages[g->stage_tape], *b = &sc->stages[g->stage_playback];
    t->io_typed = true;
    t->ro_count = 4; t->ro[0] = CBR_BUF_SRC; t->ro[1] = CBR_EXT0; t->ro[2] = CBR_EXT1; t->ro[3] = CBR_EXT2;
    t->rw_count = 3; t->rw[0] = CBR_EXT3; t->rw[1] = CBR_EXT4; t->rw[2] = CBR_EXT5;
    b->io_typed = true;
    b->ro_count = 4; b->ro[0] = CBR_EXT3; b->ro[1] = CBR_EXT4; b->ro[2] = CBR_EXT5; b->ro[3] = CBR_EXT0;
    b->rw_count = 1; b->rw[0] = CBR_BUF_DST;
    SDL_GPUBuffer *ext[6] = {g->coeff, g->lines, g->defects, g->luma, g->chroma, g->mask};
    for (int i = 0; i < 6; i++) t->external[i] = b->external[i] = ext[i];
    t->enabled = b->enabled = false;
    return true;
}

bool vhs_gpu_configure(VHSGpu *g, SignalChain *sc, SDL_GPUDevice *gpu, const VHSParams *p, double fs) {
    if (!g->deck || g->stage_tape < 0) return false;
    vhs_deck_configure(g->deck, p, fs);
    chain_update_params(sc, g->stage_tape, &g->deck->gpu, sizeof(GpuVHSParams));
    chain_update_params(sc, g->stage_playback, &g->deck->gpu, sizeof(GpuVHSParams));
    return gpu_buffer_upload(gpu, g->coeff, g->deck->coeffs, sizeof(g->deck->coeffs));
}

bool vhs_gpu_frame(VHSGpu *g, SignalChain *sc, SDL_GPUDevice *gpu, SDL_GPUCommandBuffer *cmd, uint32_t frame,
                   float sync_depth) {
    if (!vhs_gpu_enabled(g, sc)) return true;
    /* The record AGC is keyed to sync: its steady state holds the sync tip
     * at -40 IRE whatever the source's level. The configured gain is the
     * one for the 2C02's sync, so a 2C02 source keeps it exactly. */
    const float nes_sync = 264.0f / 788.0f;
    if (sync_depth > 0) g->deck->gpu.in_gain = (float)(40.0 / (264.0 / 788.0)) * (nes_sync / sync_depth);
    uint8_t *mapped = SDL_MapGPUTransferBuffer(gpu, g->transfer, true);
    if (!mapped) return false;
    VHSLineEntry *table = (VHSLineEntry *)mapped;
    VHSDefect *defects = (VHSDefect *)(mapped + TABLE_BYTES);
    memset(defects, 0, DEFECT_BYTES);
    vhs_deck_frame(g->deck, frame, table, defects);
    SDL_UnmapGPUTransferBuffer(gpu, g->transfer);
    SDL_GPUCopyPass *copy = SDL_BeginGPUCopyPass(cmd);
    if (!copy) return false;
    SDL_GPUTransferBufferLocation src = {.transfer_buffer = g->transfer, .offset = 0};
    SDL_GPUBufferRegion dst = {.buffer = g->lines, .offset = 0, .size = TABLE_BYTES};
    SDL_UploadToGPUBuffer(copy, &src, &dst, false);
    src.offset = TABLE_BYTES;
    dst = (SDL_GPUBufferRegion){.buffer = g->defects, .offset = 0, .size = DEFECT_BYTES};
    SDL_UploadToGPUBuffer(copy, &src, &dst, false);
    SDL_EndGPUCopyPass(copy);
    chain_update_params(sc, g->stage_tape, &g->deck->gpu, sizeof(GpuVHSParams));
    chain_update_params(sc, g->stage_playback, &g->deck->gpu, sizeof(GpuVHSParams));
    return true;
}

void vhs_gpu_set_enabled(VHSGpu *g, SignalChain *sc, bool enabled) {
    chain_set_stage_enabled(sc, g->stage_tape, enabled);
    chain_set_stage_enabled(sc, g->stage_playback, enabled);
}

bool vhs_gpu_enabled(const VHSGpu *g, const SignalChain *sc) {
    return g->stage_tape >= 0 && sc->stages[g->stage_tape].enabled && !sc->stages[g->stage_tape].bypass;
}

void vhs_gpu_destroy(VHSGpu *g, SDL_GPUDevice *gpu) {
    SDL_GPUBuffer *b[6] = {g->luma, g->chroma, g->mask, g->coeff, g->lines, g->defects};
    for (int i = 0; i < 6; i++) if (b[i]) SDL_ReleaseGPUBuffer(gpu, b[i]);
    if (g->transfer) SDL_ReleaseGPUTransferBuffer(gpu, g->transfer);
    free(g->deck);
    memset(g, 0, sizeof(*g));
    g->stage_tape = g->stage_playback = -1;
}
