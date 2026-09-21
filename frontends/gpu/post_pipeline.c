#include "post_pipeline.h"
#include "video_chain.h"
#include "signal_chain.h"
#include <string.h>

extern bool video_gpu_comb_active_public(const VideoGPUChain *vgc);
extern bool dispatch_video_amp_public     (VideoGPUChain *vgc, SDL_GPUCommandBuffer *cmd);
extern bool dispatch_h_blur_rgb_public    (VideoGPUChain *vgc, SDL_GPUCommandBuffer *cmd);
extern bool dispatch_beam_profile_public  (VideoGPUChain *vgc, SDL_GPUCommandBuffer *cmd);
extern bool dispatch_gun_current_public   (VideoGPUChain *vgc, SDL_GPUCommandBuffer *cmd);
extern bool dispatch_temporal_blit_public (VideoGPUChain *vgc, SDL_GPUCommandBuffer *cmd);

static inline ChainBufRef aux_ref(int slot) {
    return (ChainBufRef)((int)CBR_AUX0 + slot);
}

/* -------------------------------------------------------------------
 * Matrix decode — typed stage.
 * ro = [CBR_BUF_SRC (Y), chroma 1, chroma 2]
 * rw = [CBR_EXT0 = vgc->buf_rgb]
 *
 * NTSC:
 *   chroma 1/2 come from the filtered aux I/Q slots.
 *
 * PAL:
 *   chroma 1/2 come from the dedicated PAL correction buffers after
 *   odd-line U compensation + 1H V averaging, so the matrix sees a
 *   stable (V, U) pair.
 *
 * The rebind refills both the input bindings and the per-frame
 * 16-float color matrix + bias + kill-threshold uniform pack.
 * ------------------------------------------------------------------- */
typedef struct {
    uint32_t count;
    float m00, m01, m02;
    float m10, m11, m12;
    float m20, m21, m22;
    float bias_r, bias_g, bias_b;
    float color_killer_threshold;
    int32_t chroma_delay;
    uint32_t samples_per_line;
    uint32_t active_width, active_offset;
} MatrixDecodeParams;

static bool matrix_decode_rebind(struct SignalChainFwd *chain_fwd,
                                 struct ChainStageFwd *stage_fwd,
                                 void *user) {
    (void)chain_fwd;
    ChainStage *s      = (ChainStage *)stage_fwd;
    VideoGPUChain *vgc = (VideoGPUChain *)user;

    s->ro[0] = CBR_BUF_SRC;                /* Y from the main ping-pong */
    s->ro_count = 4; s->ro[3] = CBR_EXT3; s->external[3] = vgc->buf_receiver;
    s->rw[0] = CBR_EXT0;
    s->rw_count = 1;
    s->external[0] = vgc->buf_rgb;

    if (vgc->stage_pal_chroma >= 0) {
        if (!vgc->buf_pal_v || !vgc->buf_pal_u) return false;
        s->ro[1] = CBR_EXT1;
        s->ro[2] = CBR_EXT2;
        s->external[1] = vgc->buf_pal_v;
        s->external[2] = vgc->buf_pal_u;
    } else {
        bool comb_on = video_gpu_comb_active_public(vgc);
        ChromaAuxLayout aux = video_chain_chroma_aux_layout(comb_on);
        s->ro[1] = aux_ref(aux.i_filt);
        s->ro[2] = aux_ref(aux.q_filt);
        s->external[1] = NULL;
        s->external[2] = NULL;
    }

    MatrixDecodeParams p;
    p.count  = (uint32_t)vgc->signal_fmt.total_samples;
    p.active_width = (uint32_t)vgc->signal_fmt.samples_per_line;
    p.active_offset = (uint32_t)(65 * vgc->signal_fmt.samples_per_pixel);
    p.m00 = vgc->color_matrix[0][0];
    p.m01 = vgc->color_matrix[0][1];
    p.m02 = vgc->color_matrix[0][2];
    p.m10 = vgc->color_matrix[1][0];
    p.m11 = vgc->color_matrix[1][1];
    p.m12 = vgc->color_matrix[1][2];
    p.m20 = vgc->color_matrix[2][0];
    p.m21 = vgc->color_matrix[2][1];
    p.m22 = vgc->color_matrix[2][2];
    p.bias_r = vgc->color_bias[0];
    p.bias_g = vgc->color_bias[1];
    p.bias_b = vgc->color_bias[2];
    p.color_killer_threshold = vgc->chain ? vgc->chain->tv.color_killer : 0.0f;
    p.chroma_delay = 0; /* Both FIRs are centred on the same source sample. */
    p.samples_per_line = (uint32_t)vgc->raster_fmt.samples_per_line;
    if (!video_chain_stage_active(vgc->chain, 7)) {
        p.m01 = p.m02 = 0.0f;
        p.m11 = p.m12 = 0.0f;
        p.m21 = p.m22 = 0.0f;
    }
    memcpy(s->params, &p, sizeof(p));
    s->params_size = sizeof(p);
    s->dispatch_x = (p.count + 255) / 256;
    s->dispatch_y = 1;
    s->dispatch_z = 1;
    return true;
}

/* -------------------------------------------------------------------
 * Deflection map — typed stage.
 *
 * Builds a coherent landing map at beam-output resolution:
 *   ext0 = landed X positions for R/G/B + dwell factor
 *   ext1 = landed Y positions for R/G/B + sigma scale
 *
 * This moves timebase jitter, scanline correlation, edge focus and
 * convergence out of beam_profile.comp so the beam stage only deposits
 * light using a precomputed raster field.
 * ------------------------------------------------------------------- */
typedef struct {
    uint32_t signal_w;
    uint32_t out_w;
    uint32_t out_h;
    uint32_t rows_per_scanline;
    uint32_t frame_counter;
    float h_jitter;
    float v_jitter;
    float rf_interference;
    float geometry_warp;
    float convergence_static;
    float convergence_dynamic;
    float conv_r_x;
    float conv_r_y;
    float conv_b_x;
    float conv_b_y;
    float edge_focus;
    float velocity_dim;
    float psu_hum;
    float focus_breathing;
    float scanline_wobble;
    float corner_astigmatism;
    float barrel;
    float barrel_v;
    float overscan;
    float keystone;
    float rotation;
    float skew_x;
    float skew_y;
    float hv_sag;
    float frame_brightness;
    float h_pos;
    float v_pos;
    float h_size;
    float v_size;
    float microphonic_amount;
    float audio_bass_rms;
    float top_band_shift;
    float top_edge_skew;
    float top_band_start;
    float top_band_end;
    float top_edge_width;
} DeflectionParams;

static bool deflection_rebind(struct SignalChainFwd *chain_fwd,
                              struct ChainStageFwd *stage_fwd,
                              void *user) {
    (void)chain_fwd;
    ChainStage *s      = (ChainStage *)stage_fwd;
    VideoGPUChain *vgc = (VideoGPUChain *)user;
    const TVDisplayParams *tv = vgc->chain ? &vgc->chain->tv : NULL;

    if (!vgc->buf_deflection_x || !vgc->buf_deflection_y ||
        vgc->beam_out_w <= 0 || vgc->beam_out_h <= 0 ||
        vgc->beam_rows_per_scanline <= 0) {
        return false;
    }

    DeflectionParams p;
    memset(&p, 0, sizeof(p));
    p.signal_w            = (uint32_t)vgc->signal_fmt.samples_per_line;
    p.out_w               = (uint32_t)vgc->beam_out_w;
    p.out_h               = (uint32_t)vgc->beam_out_h;
    p.rows_per_scanline   = (uint32_t)vgc->beam_rows_per_scanline;
    p.frame_counter       = vgc->beam_frame_counter;
    p.h_jitter            = tv ? tv->h_jitter : 0.0f;
    p.v_jitter            = tv ? tv->v_jitter : 0.0f;
    p.rf_interference     = tv ? tv->rf_interference : 0.0f;
    p.geometry_warp       = tv ? tv->geometry_warp : 0.0f;
    p.convergence_static  = tv ? tv->convergence_static : 0.0f;
    p.convergence_dynamic = tv ? tv->convergence_dynamic : 0.0f;
    p.conv_r_x            = tv ? tv->conv_r_x : 0.0f;
    p.conv_r_y            = tv ? tv->conv_r_y : 0.0f;
    p.conv_b_x            = tv ? tv->conv_b_x : 0.0f;
    p.conv_b_y            = tv ? tv->conv_b_y : 0.0f;
    p.edge_focus          = tv ? tv->edge_focus : 0.0f;
    p.velocity_dim        = tv ? tv->velocity_dim : 0.0f;
    p.psu_hum             = vgc->chain ? vgc->chain->console_psu_hum : 0.0f;
    p.focus_breathing     = tv ? tv->focus_breathing : 0.0f;
    p.scanline_wobble     = tv ? tv->scanline_wobble : 0.0f;
    p.corner_astigmatism  = tv ? tv->corner_astigmatism : 0.0f;
    p.barrel              = tv ? tv->barrel : 0.0f;
    p.barrel_v            = tv ? tv->barrel_v : 0.0f;
    p.overscan            = tv ? tv->overscan : 0.0f;
    p.keystone            = tv ? tv->keystone : 0.0f;
    p.rotation            = tv ? tv->rotation : 0.0f;
    p.skew_x              = tv ? tv->skew_x : 0.0f;
    p.skew_y              = tv ? tv->skew_y : 0.0f;
    p.hv_sag              = tv ? tv->hv_sag : 0.0f;
    p.frame_brightness    = vgc->frame_brightness;
    p.h_pos               = tv ? tv->h_pos : 0.0f;
    p.v_pos               = tv ? tv->v_pos : 0.0f;
    p.h_size              = tv ? (tv->h_size > 0.01f ? tv->h_size : 1.0f) : 1.0f;
    p.v_size              = tv ? (tv->v_size > 0.01f ? tv->v_size : 1.0f) : 1.0f;
    p.microphonic_amount  = tv ? tv->microphonic_amount : 0.0f;
    p.audio_bass_rms      = vgc->audio_bass_rms;
    p.top_band_shift      = tv ? tv->top_band_shift : 0.0f;
    p.top_edge_skew       = tv ? tv->top_edge_skew : 0.0f;
    p.top_band_start      = tv ? tv->top_band_start : 18.0f;
    p.top_band_end        = tv ? tv->top_band_end : 34.0f;
    p.top_edge_width      = tv ? tv->top_edge_width : 0.08f;

    s->ro_count = 1; s->ro[0] = CBR_EXT2; s->external[2] = vgc->buf_crt_load;
    s->rw[0] = CBR_EXT0;
    s->rw[1] = CBR_EXT1;
    s->rw_count = 2;
    s->external[0] = vgc->buf_deflection_x;
    s->external[1] = vgc->buf_deflection_y;
    memcpy(s->params, &p, sizeof(p));
    s->params_size = sizeof(p);
    s->dispatch_x = (p.out_w + 15) / 16;
    s->dispatch_y = (p.out_h + 15) / 16;
    s->dispatch_z = 1;
    return true;
}

bool post_rgb_dispatch(VideoGPUChain *vgc, SDL_GPUCommandBuffer *cmd) {
    if (!vgc || !cmd) return false;

    const TVDisplayParams *tv = &vgc->chain->tv;
    bool loading = tv->beam_current_load > 0 || tv->video_black_droop > 0 || fabsf(tv->hv_sag) > 0 || tv->focus_breathing > 0;
    chain_set_stage_enabled(&vgc->sig_chain, vgc->stage_crt_load, loading);
    chain_set_stage_enabled(&vgc->sig_chain, vgc->stage_crt_supply, loading);
    (void)dispatch_video_amp_public(vgc, cmd);
    return true;
}

bool beam_output_dispatch(VideoGPUChain *vgc, SDL_GPUCommandBuffer *cmd) {
    if (!vgc || !cmd) return false;
    if (!vgc->buf_beam_rgba || !vgc->buf_deflection_x || !vgc->buf_deflection_y) {
        return true;
    }
    return dispatch_gun_current_public(vgc, cmd)
        && dispatch_h_blur_rgb_public(vgc, cmd)
        && dispatch_beam_profile_public(vgc, cmd)
        && dispatch_temporal_blit_public(vgc, cmd);
}

void post_pipeline_install_matrix_typed(VideoGPUChain *vgc) {
    if (!vgc || vgc->stage_matrix < 0) return;
    ChainStage *s = &vgc->sig_chain.stages[vgc->stage_matrix];
    s->io_typed = true;
    s->custom = NULL;
    s->custom_user = NULL;
    s->rebind = matrix_decode_rebind;
    s->rebind_user = vgc;
    s->ro[0] = CBR_BUF_SRC; s->ro[1] = CBR_AUX0; s->ro[2] = CBR_AUX1;
    s->ro_count = 3;
    s->rw[0] = CBR_EXT0; s->rw_count = 1;
    s->external[0] = vgc->buf_rgb;
    s->snapshot_src  = CBR_EXT0;
    s->snapshot_size = vgc->rgb_size;
    chain_set_stage_enabled(&vgc->sig_chain, vgc->stage_matrix, true);
}

void post_pipeline_install_deflection_typed(VideoGPUChain *vgc) {
    if (!vgc || vgc->stage_deflection < 0) return;
    ChainStage *s = &vgc->sig_chain.stages[vgc->stage_deflection];
    s->io_typed = true;
    s->custom = NULL;
    s->custom_user = NULL;
    s->rebind = deflection_rebind;
    s->rebind_user = vgc;
    s->ro_count = 1; s->ro[0] = CBR_EXT2; s->external[2] = vgc->buf_crt_load;
    s->rw[0] = CBR_EXT0;
    s->rw[1] = CBR_EXT1;
    s->rw_count = 2;
    s->external[0] = vgc->buf_deflection_x;
    s->external[1] = vgc->buf_deflection_y;
    s->snapshot_src  = CBR_EXT0;
    s->snapshot_size = vgc->deflection_size;
    chain_set_stage_enabled(&vgc->sig_chain, vgc->stage_deflection,
                            vgc->buf_deflection_x && vgc->buf_deflection_y);
}

bool post_rgb_chain_dispatch(void *chain, void *stage,
                             SDL_GPUCommandBuffer *cmd, void *user) {
    (void)chain;
    VideoGPUChain *vgc = (VideoGPUChain *)user;
    bool ok = post_rgb_dispatch(vgc, cmd);
    if (ok && stage) {
        ChainStage *s = (ChainStage *)stage;
        s->external[0] = vgc->buf_rgb;
        s->snapshot_size = vgc->rgb_size;
    }
    return ok;
}

bool beam_output_chain_dispatch(void *chain, void *stage,
                                SDL_GPUCommandBuffer *cmd, void *user) {
    (void)chain;
    (void)stage;
    return beam_output_dispatch((VideoGPUChain *)user, cmd);
}

static bool crt_load_rebind(struct SignalChainFwd *chain, struct ChainStageFwd *stage, void *user) {
    (void)chain;
    VideoGPUChain *v = user;
    ChainStage *s = (ChainStage *)stage;
    const TVDisplayParams *tv = &v->chain->tv;
    struct { uint32_t width, spp; float gamma, strength, dot_seconds; uint32_t mode; float elapsed; uint32_t region; float black_droop, recovery_us, pad[2]; } p = {
        v->signal_fmt.samples_per_line, v->signal_fmt.samples_per_pixel,
        tv->gamma > 0 ? tv->gamma : 2.4f, tv->beam_current_load,
        v->signal_fmt.samples_per_pixel / signal_region_sample_rate_hz(v->signal_fmt.region),
        s == &v->sig_chain.stages[v->stage_crt_supply], fmaxf(1,v->elapsed_frames), v->signal_fmt.region,
        tv->video_black_droop, tv->video_recovery_us > 0 ? tv->video_recovery_us : 18, {0,0}
    };
    s->rw_count=2; s->rw[0]=CBR_EXT0; s->rw[1]=CBR_EXT1;
    s->external[0]=v->buf_rgb; s->external[1]=v->buf_crt_load;
    memcpy(s->params,&p,sizeof(p)); s->params_size=sizeof(p);
    return true;
}
void post_pipeline_install_load_typed(VideoGPUChain *v) {
    int indices[]={v->stage_crt_load,v->stage_crt_supply};
    for(int i=0;i<2;i++) {
        ChainStage *s=&v->sig_chain.stages[indices[i]];
        s->io_typed=true; s->rebind=crt_load_rebind; s->rebind_user=v;
    }
}
