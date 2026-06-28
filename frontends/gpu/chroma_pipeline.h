/*
 * chroma_pipeline.h -- Manual dispatch for the NES chroma path
 * =============================================================
 *
 * The NES composite decoder runs THREE GPU compute stages that don't
 * fit the generic SignalChain contract and are dispatched manually
 * outside chain_run:
 *
 *   1. Chroma demod (modulator kernel, mode 3)
 *        in:  composite or comb-separated C
 *        out: I (aux), Q (aux)
 *        — dual readwrite output, which chain_run can't express.
 *
 *   2. I-bandwidth FIR (chroma_i_fir)
 *        in:  aux[I raw], tap buf
 *        out: aux[I filtered]
 *        — aux-to-aux FIR, which chain_run can't express.
 *
 *   3. Q-bandwidth FIR (chroma_q_fir)
 *        in:  aux[Q raw], tap buf
 *        out: aux[Q filtered]
 *        — same aux-to-aux limitation.
 *
 * This header isolates all three into ONE entry point so the sprawl
 * of binding indices, ping-pong assumptions, and comb-active branches
 * lives in a single documented function — not copy-pasted across
 * video_gpu.c. The aux slot numbers come from
 * video_chain_chroma_aux_layout() (video_chain.h) which is separately
 * unit-tested.
 *
 * Contract assumed on entry:
 *   After chain_run_cmd returns, sig_chain.buf[1 - current_buf] holds
 *   the ghosting/RC-filtered composite BEFORE Luma FIR stripped it.
 *   If any stage between Ghosting and Luma FIR is added later that
 *   touches the ping-pong, this contract must be re-validated. Adding
 *   a test for "composite buffer is where we think it is after
 *   chain_run" would make the invariant checkable.
 */
#ifndef CHROMA_PIPELINE_H
#define CHROMA_PIPELINE_H

#include "video_gpu.h"

/* Install typed-binding hooks on the chroma stages:
 *   - demod
 *   - I FIR
 *   - Q FIR
 *   - PAL U-fix + 1H V-average stage (when present)
 *
 * Each gets an io_typed=true declaration plus a rebind callback that
 * updates its aux/external references each frame from the current
 * ChromaAuxLayout (which depends on whether the comb filter is
 * actively producing separated C). Clears any legacy custom-dispatch
 * hooks. Safe to call after video_gpu_init. */
void chroma_pipeline_install_typed(VideoGPUChain *vgc);

#endif /* CHROMA_PIPELINE_H */
