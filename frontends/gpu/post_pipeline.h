/*
 * post_pipeline.h -- Post-signal GPU dispatches
 * ================================================
 *
 * Owns the RGB / beam-domain stages that run AFTER the composite
 * signal chain completes:
 *   1. Matrix decode (typed stage)
 *   2. RGB post (video amp + horizontal blur, custom stage)
 *   3. Deflection map (typed stage)
 *   4. Beam output (beam deposition + temporal blit, custom stage)
 *
 * Matrix decode and deflection now run as proper typed stages; the two
 * custom stages are the parts that still need side effects or storage
 * texture writes the generic SignalChain does not own yet.
 */
#ifndef POST_PIPELINE_H
#define POST_PIPELINE_H

#include "video_gpu.h"

/* Execute the RGB post stage (video amp + h-blur). */
bool post_rgb_dispatch(VideoGPUChain *vgc, SDL_GPUCommandBuffer *cmd);

/* Execute the beam-output stage (beam deposition + temporal blit). */
bool beam_output_dispatch(VideoGPUChain *vgc, SDL_GPUCommandBuffer *cmd);

/* Chain-stage custom-dispatch adapters. `user` must be the
 * VideoGPUChain. */
bool post_rgb_chain_dispatch(void *chain, void *stage,
                             SDL_GPUCommandBuffer *cmd, void *user);
bool beam_output_chain_dispatch(void *chain, void *stage,
                                SDL_GPUCommandBuffer *cmd, void *user);

/* Install the matrix-decode typed chain stage. Matrix decode moved
 * from the custom-dispatch meta-stage into a fully-typed ChainStage
 * with its own rebind hook — ChromaAuxLayout + per-frame uniform
 * pack — managed by chain_run_cmd's typed dispatcher. Call after
 * the stage slot has been registered in video_gpu_init. */
void post_pipeline_install_matrix_typed(VideoGPUChain *vgc);

/* Install the typed deflection-map stage. Call after the stage slot has
 * been registered in video_gpu_init; the rebind fills the landing-map
 * buffers and uniform pack each frame once beam params are available. */
void post_pipeline_install_deflection_typed(VideoGPUChain *vgc);

#endif /* POST_PIPELINE_H */
