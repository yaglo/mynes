#include "chroma_pipeline.h"
#include "video_gpu.h"
#include "signal_chain.h"
#include "video_chain.h"
#include <stdio.h>
#include <string.h>

/* Shared trampoline — chroma knows about the comb state via
 * video_gpu.c; this keeps the chroma pipeline free of internal
 * VideoGPUChain access. */
extern bool video_gpu_comb_active_public(const VideoGPUChain *vgc);

/* -------------------------------------------------------------------
 * Typed-binding rebind hooks
 *
 * All three chroma stages express their bindings via ChainStage's
 * typed ro[] / rw[] arrays. Only thing that varies between frames is
 * which aux slot holds I raw, Q raw, I filtered, Q filtered — this
 * depends on whether the comb filter is actively producing a
 * separated chroma component. The rebind hook re-reads that state and
 * updates the stage's ro/rw entries just before dispatch.
 * ------------------------------------------------------------------- */

static inline ChainBufRef aux_ref(int slot) {
    /* CBR_AUX0..3 are consecutive in the enum. */
    return (ChainBufRef)((int)CBR_AUX0 + slot);
}

static bool chroma_demod_rebind(struct SignalChainFwd *chain,
                                struct ChainStageFwd *stage_fwd,
                                void *user) {
    (void)chain;
    ChainStage *s = (ChainStage *)stage_fwd;
    VideoGPUChain *vgc = (VideoGPUChain *)user;
    bool comb_active = video_gpu_comb_active_public(vgc);
    ChromaAuxLayout aux = video_chain_chroma_aux_layout(comb_active);

    /* Dual RW output: I raw and Q raw. */
    s->rw[0] = aux_ref(aux.i_raw);
    s->rw[1] = aux_ref(aux.q_raw);
    s->rw_count = 2;

    /* Chroma source. Comb active → aux[0] carries separated C.
     * Comb inactive → read composite from the "other" ping-pong
     * buffer (the one Luma FIR consumed). That's CBR_BUF_DST from
     * the typed dispatcher's perspective — it names the buffer
     * opposite to current_buf, regardless of what it's used for. */
    s->ro[0] = comb_active ? CBR_AUX0 : CBR_BUF_DST;
    s->ro_count = 1;
    return true;
}

static bool chroma_fir_rebind(struct SignalChainFwd *chain,
                              struct ChainStageFwd *stage_fwd,
                              void *user) {
    (void)chain;
    ChainStage *s = (ChainStage *)stage_fwd;
    VideoGPUChain *vgc = (VideoGPUChain *)user;
    bool comb_active = video_gpu_comb_active_public(vgc);
    ChromaAuxLayout aux = video_chain_chroma_aux_layout(comb_active);

    /* Which FIR are we? The caller sets taps_index at init time and
     * stashes the channel (0=I, 1=Q) via custom_user; we recover it
     * from the stage's name since it's a stable string pointer. */
    bool is_q = (s->name && s->name[strlen("Chroma ") + 0] == 'Q');
    int src = is_q ? aux.q_raw  : aux.i_raw;
    int dst = is_q ? aux.q_filt : aux.i_filt;

    s->ro[0] = aux_ref(src);
    s->ro[1] = CBR_TAPS;
    s->ro_count = 2;
    s->rw[0] = aux_ref(dst);
    s->rw_count = 1;
    return true;
}

static bool pal_chroma_rebind(struct SignalChainFwd *chain,
                              struct ChainStageFwd *stage_fwd,
                              void *user) {
    (void)chain;
    ChainStage *s = (ChainStage *)stage_fwd;
    VideoGPUChain *vgc = (VideoGPUChain *)user;
    bool comb_active = video_gpu_comb_active_public(vgc);
    ChromaAuxLayout aux = video_chain_chroma_aux_layout(comb_active);

    s->ro[0] = aux_ref(aux.i_filt);   /* PAL V */
    s->ro[1] = aux_ref(aux.q_filt);   /* PAL U before odd-line fix */
    s->ro_count = 2;

    s->rw[0] = CBR_EXT0;
    s->rw[1] = CBR_EXT1;
    s->rw_count = 2;
    s->external[0] = vgc->buf_pal_v;
    s->external[1] = vgc->buf_pal_u;
    return true;
}

/* Register chroma stages' custom bindings. Call after video_gpu_init
 * has added the three stages to the chain. Each becomes a typed
 * stage with its own rebind hook; custom-dispatch hook is cleared. */
void chroma_pipeline_install_typed(VideoGPUChain *vgc) {
    if (!vgc) return;
    SignalChain *sc = &vgc->sig_chain;

    if (vgc->stage_chroma_demod >= 0) {
        ChainStage *d = &sc->stages[vgc->stage_chroma_demod];
        d->io_typed = true;
        d->rebind = chroma_demod_rebind;
        d->rebind_user = vgc;
        d->custom = NULL;
        d->custom_user = NULL;
        /* Initial bindings — rebind hook overwrites each frame. */
        d->ro_count = 1; d->ro[0] = CBR_BUF_DST;
        d->rw_count = 2; d->rw[0] = CBR_AUX1; d->rw[1] = CBR_AUX2;
        /* Demod ignores chain ping-pong — doesn't touch BUF_DST
         * in rw, so current_buf stays put. */
    }
    if (vgc->stage_chroma_i_fir >= 0) {
        ChainStage *f = &sc->stages[vgc->stage_chroma_i_fir];
        f->io_typed = true;
        f->rebind = chroma_fir_rebind;
        f->rebind_user = vgc;
        f->custom = NULL;
        f->custom_user = NULL;
        f->ro_count = 2; f->ro[0] = CBR_AUX0; f->ro[1] = CBR_TAPS;
        f->rw_count = 1; f->rw[0] = CBR_AUX2;
    }
    if (vgc->stage_chroma_q_fir >= 0) {
        ChainStage *f = &sc->stages[vgc->stage_chroma_q_fir];
        f->io_typed = true;
        f->rebind = chroma_fir_rebind;
        f->rebind_user = vgc;
        f->custom = NULL;
        f->custom_user = NULL;
        f->ro_count = 2; f->ro[0] = CBR_AUX1; f->ro[1] = CBR_TAPS;
        f->rw_count = 1; f->rw[0] = CBR_AUX3;
    }
    if (vgc->stage_pal_chroma >= 0) {
        ChainStage *p = &sc->stages[vgc->stage_pal_chroma];
        p->io_typed = true;
        p->rebind = pal_chroma_rebind;
        p->rebind_user = vgc;
        p->custom = NULL;
        p->custom_user = NULL;
        p->ro_count = 2; p->ro[0] = CBR_AUX2; p->ro[1] = CBR_AUX3;
        p->rw_count = 2; p->rw[0] = CBR_EXT0; p->rw[1] = CBR_EXT1;
        p->external[0] = vgc->buf_pal_v;
        p->external[1] = vgc->buf_pal_u;
        p->snapshot_src = CBR_EXT0;
    }
}
