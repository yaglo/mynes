/*
 * Video GPU Chain -- Implementation (Signal Chain Refactor)
 * ===========================================================
 *
 * See video_gpu.h for API documentation and signal chain stage layout.
 *
 * Uses the generic SignalChain runner for the luma path (RC filters +
 * luma FIR). The chain runner handles ping-pong buffers, per-stage
 * compute dispatch, and timing.
 *
 * The chroma path is now part of the typed SignalChain flow:
 *
 *   chain_run:
 *     RC / RF / ghost / comb / luma FIR
 *     chroma demod
 *     chroma I/Q FIR
 *     PAL correction stage (PAL only)
 *     matrix decode
 *     RGB-domain post pipeline
 *
 * The DAC shader remains separate (its descriptor layout is unique),
 * but once the waveform is in the chain the rest of the decode stays
 * inside chain_run_cmd.
 */

#include "video_gpu.h"
#include "signal_precompute.h"
#include "gpu_log.h"
#include "chroma_pipeline.h"
#include "post_pipeline.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* Forward declarations.
 * All dispatch functions accept a shared command buffer (cmd). Each creates
 * its own compute pass (pass boundary = barrier) and returns. The caller
 * manages cmd lifecycle and submits once at the end. */
static bool dispatch_video_amp(VideoGPUChain *vgc, SDL_GPUCommandBuffer *cmd);

void video_gpu_scanned_trace(const VideoGPUChain *v, float trace[4]) {
    const DecodeWindow *w = &v->window;
    if (v->chain && v->chain->tv.monitor_model == 1) {
        trace[0] = -1e9f; trace[1] = 1e9f; trace[2] = 0; trace[3] = (float)w->lines;
        return;
    }
    trace[0] = w->trace_x0; trace[1] = w->trace_x1;
    trace[2] = (float)w->trace_row0; trace[3] = (float)w->trace_row1;
}
static bool dispatch_h_blur_rgb(VideoGPUChain *vgc, SDL_GPUCommandBuffer *cmd);
static bool dispatch_beam_profile(VideoGPUChain *vgc, SDL_GPUCommandBuffer *cmd);
static bool dispatch_aux_fir_cmd(VideoGPUChain *vgc, SDL_GPUCommandBuffer *cmd,
                                  int stage_idx, int src_aux, int dst_aux);
/* The standard burst, as an amplitude with blanking to white at 1: NTSC
 * 40 IRE peak to peak of 100, PAL 300 mV of 700. The ACC holds the burst
 * here, so the console's 47.7 IRE burst reads two thirds as much chroma. */
static float demod_burst_reference(const VideoGPUChain *vgc) {
    return vgc->signal_fmt.region == SIGNAL_REGION_PAL ? 150.0f / 700.0f : 0.20f;
}

/* The overlay covers the console picture's place in the decode window, read
 * at each dispatch so a window changed after init is followed. */
static void osd_params(const VideoGPUChain *v,ChainStage *s) {
    const DecodeWindow *w=&v->window;
    uint32_t p[]={(uint32_t)decode_window_samples(w),(uint32_t)w->width,
                  (uint32_t)w->picture_x,(uint32_t)w->picture_row,(uint32_t)w->picture_w,0,0,0};
    memcpy(s->params,p,sizeof(p)); s->params_size=sizeof(p);
    s->dispatch_x=(p[0]+255)/256;
}

static bool rebind_osd(struct SignalChainFwd *chain,struct ChainStageFwd *stage,void *user) {
    (void)chain;
    VideoGPUChain *v=user;
    ChainStage *s=(ChainStage *)stage;
    osd_params(v,s);
    s->ro_count=1; s->ro[0]=CBR_EXT0; s->external[0]=v->buf_osd;
    s->rw_count=1; s->rw[0]=CBR_EXT1; s->external[1]=v->buf_rgb;
    return v->buf_osd && v->buf_rgb;
}

bool video_gpu_set_osd(VideoGPUChain *v,SDL_GPUDevice *gpu,const uint32_t *rgba) {
    if(v->stage_osd<0) return false;
    if(!rgba) { chain_set_stage_enabled(&v->sig_chain,v->stage_osd,false); return true; }
    const size_t bytes=256*240*sizeof(uint32_t);
    if(!v->buf_osd) {
        v->buf_osd=gpu_buffer_create(gpu,(Uint32)bytes,GPU_BUF_READONLY);
        if(!v->buf_osd) return false;
    }
    if(!v->osd_cache || memcmp(v->osd_cache,rgba,bytes)) {
        if(!gpu_buffer_upload(gpu,v->buf_osd,rgba,(Uint32)bytes)) return false;
        if(!v->osd_cache) v->osd_cache=malloc(bytes);
        if(!v->osd_cache) return false;
        memcpy(v->osd_cache,rgba,bytes);
    }
    chain_set_stage_enabled(&v->sig_chain,v->stage_osd,true);
    return true;
}

static bool rebind_rf_if(struct SignalChainFwd *chain,struct ChainStageFwd *stage,void *user) {
    (void)chain;
    VideoGPUChain *v=user;
    ChainStage *s=(ChainStage *)stage,*am=&v->sig_chain.stages[v->stage_rf];
    s->reuse_output=!am->enabled || am->bypass;
    s->rw[0]=s->reuse_output ? CBR_BUF_SRC : CBR_BUF_DST;
    return true;
}

/* The keyed top-sync AGC's constants from the preset: agc_attack_ms is the
 * loop's small-signal time constant and agc_release_ms the time it takes
 * to discharge 52 dB (TDA8362 objective specification: 2 ms and 25 ms
 * with C = 2.2 uF). The charge slews at the same 52 dB per attack time
 * for large steps. */
static GpuAGCParams agc_params_from(const VideoGPUChain *v) {
    const VideoChain *chain = v->chain;
    float line_ms = 1000.0f * (float)v->raster_fmt.samples_per_line / signal_format_sample_rate_hz(&v->signal_fmt);
    float attack_ms = chain->rf.agc_attack_ms > 0 ? chain->rf.agc_attack_ms : 2.0f;
    float release_ms = chain->rf.agc_release_ms > 0 ? chain->rf.agc_release_ms : 25.0f;
    GpuAGCParams p = {0};
    p.total_count = (uint32_t)v->raster_fmt.total_samples;
    p.samples_per_line = (uint32_t)v->raster_fmt.samples_per_line;
    p.num_lines = (uint32_t)v->raster_fmt.lines;
    p.target_level = 264.0f / 788.0f;
    p.attack_coeff = 1.0f - expf(-line_ms / attack_ms);
    p.release_coeff = 52.0f * line_ms / release_ms;
    p.attack_slew_db = 52.0f * line_ms / attack_ms;
    p.noise_peak = 1.0f;
    p.min_gain = 0.5f;
    p.max_gain = 2.0f;
    return p;
}

static GpuReceiverPLLParams receiver_pll_params(const VideoGPUChain *v) {
    GpuReceiverPLLParams p = {0};
    p.count = (uint32_t)v->raster_fmt.lines;
    p.full_width = (uint32_t)v->raster_fmt.samples_per_line;
    p.samples_per_dot = (uint32_t)v->raster_fmt.samples_per_pixel;
    p.region = (uint32_t)v->raster_fmt.region;
    const TVDisplayParams *tv = &v->chain->tv;
    if (tv->h_pll_hz > 0) {
        /* Second-order loop per line: Kp = 2 zeta wn, Ki = wn^2. The loop
         * samples one sync edge per line, and its recurrence z^2 + (Kp g +
         * Ki g - 2) z + (1 - Kp g) is stable only while Kp g < 2 and
         * 4 - 2 Kp g - Ki g > 0. The natural frequency is held to 1 kHz
         * and the V-blank gain to the value that keeps Kp g at or below
         * 1.5 with a margin of 1 on the second bound, so no setting runs
         * away during the 21 fast lines. */
        double line_rate = signal_format_sample_rate_hz(&v->signal_fmt) / p.full_width;
        double wn = 2 * M_PI * fmin(tv->h_pll_hz, 1000) / line_rate;
        double zeta = tv->h_pll_damping > 0 ? tv->h_pll_damping : 0.7;
        double kp = 2 * zeta * wn, ki = wn * wn;
        double g = tv->h_pll_vblank_gain > 0 ? tv->h_pll_vblank_gain : 1.0;
        g = fmin(g, fmin(1.5 / kp, 3 / (2 * kp + ki)));
        p.h_pll = 1;
        p.h_kp = (float)kp;
        p.h_ki = (float)ki;
        p.h_vblank_gain = (float)fmax(g, 1.0);
        p.h_vblank_lines = 21;
    } else if (tv->h_afc_tau_ms > 0) {
        float line_ms = 1000.0f * p.full_width / signal_format_sample_rate_hz(&v->signal_fmt);
        p.h_response = -expm1f(-line_ms / tv->h_afc_tau_ms);
    }
    /* Keyed clamp: the back-porch measurement charges the clamp by this
     * fraction each line. */
    double clamp_lines = tv->clamp_lines > 0 ? tv->clamp_lines : VIDEO_CLAMP_LINES_DEFAULT;
    p.clamp_gain = (float)-expm1(-1 / clamp_lines);
    return p;
}
static bool vhs_active(const VideoChain *c) {
    return c->vhs.enabled && c->signal_fmt.region==SIGNAL_REGION_NTSC &&
        (c->connection==VIDEO_CONN_COMPOSITE || c->connection==VIDEO_CONN_RF);
}

/* Generic aperture correction, not a calibrated TV/chip response. Keep
 * this separate from Y extraction: its input is already band-limited Y.
 * A zero setting bypasses the pass entirely. */
#define LUMA_PEAKING_TAPS 31
static void luma_peaking_taps(const VideoChain *c, float *taps) {
    memset(taps, 0, LUMA_PEAKING_TAPS * sizeof(*taps));
    taps[LUMA_PEAKING_TAPS / 2] = 1.0f;
    float cutoff = c->tv.luma_bandwidth / signal_format_sample_rate_hz(&c->signal_fmt);
    cutoff = fminf(0.2f, fmaxf(0.005f, cutoff));
    signal_apply_peaking(taps, LUMA_PEAKING_TAPS, cutoff, c->tv.luma_peaking);
    if (c->tv.aperture_max_db > 0)
        signal_normalize_aperture_gain(taps, LUMA_PEAKING_TAPS,
            fminf(1.0f, fmaxf(0.0f, c->tv.luma_peaking)) * c->tv.aperture_max_db);
}
static bool luma_peaking_active(const VideoChain *c) {
    return video_chain_stage_active(c, 8) && c->tv.luma_peaking >= 0.001f;
}

static bool dispatch_temporal_blit(VideoGPUChain *vgc, SDL_GPUCommandBuffer *cmd);

/* ============================================================================
 * Helpers
 * ============================================================================ */

/* Is the comb filter actually performing Y/C separation?
 * True only when: stage exists, enabled, not bypassed, AND shader mode > 0.
 * Shader mode 0 = bypass (C=0), so the comb runs but produces no chroma. */
bool video_gpu_comb_active_public(const VideoGPUChain *vgc);

static bool video_gpu_comb_active(const VideoGPUChain *vgc) {
    if (vgc->source_separated) return true;
    if (vgc->stage_comb < 0) return false;
    const ChainStage *s = &vgc->sig_chain.stages[vgc->stage_comb];
    if (!s->enabled || s->bypass) return false;
    const GpuCombParams *cp = (const GpuCombParams *)s->params;
    return cp->mode > 0;
}

/* Public trampoline — lets chroma_pipeline.c (same TU boundary as
 * other split-out dispatch modules) query comb state without
 * exposing the struct. */
bool video_gpu_comb_active_public(const VideoGPUChain *vgc) {
    return video_gpu_comb_active(vgc);
}

static char *build_path(const char *dir, const char *filename) {
    size_t dlen = strlen(dir);
    size_t flen = strlen(filename);
    int need_slash = (dlen > 0 && dir[dlen - 1] != '/') ? 1 : 0;
    char *path = (char *)malloc(dlen + need_slash + flen + 1);
    if (!path) return NULL;
    memcpy(path, dir, dlen);
    if (need_slash) path[dlen] = '/';
    memcpy(path + dlen + need_slash, filename, flen + 1);
    return path;
}

/* Gun bandwidths are independent. Averages erase intentional channel
 * differences; skipping a wide amplifier also skipped its other controls. */
static void update_video_amp(VideoGPUChain *vgc) {
    const TVDisplayParams *tv = &vgc->chain->tv;
    float bandwidth[3] = {tv->r_bandwidth, tv->g_bandwidth, tv->b_bandwidth};
    float rate = signal_format_sample_rate_hz(&vgc->signal_fmt);
    float narrowest = fminf(bandwidth[0], fminf(bandwidth[1], bandwidth[2]));
    float cutoff = fmaxf(narrowest / rate, 0.01f);
    int count = ((int)ceilf(1.5f / cutoff)) | 1;
    if (count < 9) count = 9;
    if (count > 31) count = 31;
    vgc->vamp_tap_count = count;
    for (int c = 0; c < 3; c++) {
        float taps[32] = {0};
        float frequency = fminf(fmaxf(bandwidth[c] / rate, 0.01f), 0.49f);
        if (tv->rgb_bandwidth_3db)
            signal_design_fir_3db(taps, count, frequency);
        else
            signal_design_fir(taps, count, frequency);
        for (int i = 0; i < count; i++) vgc->vamp_taps[i][c] = taps[i];
    }
    vgc->vamp_enabled = vgc->buf_rgb2 != NULL;
}

/* ============================================================================
 * Initialization
 * ============================================================================ */

bool video_gpu_init(VideoGPUChain *vgc, SDL_GPUDevice *gpu,
                    const VideoChain *chain, const char *shader_dir,
                    const float *fir_y_taps, int fir_y_n,
                    const float *fir_c_taps, int fir_c_n,
                    const float *fir_q_taps, int fir_q_n)
{
    memset(vgc, 0, sizeof(*vgc));
    vgc->chain = chain;
    vgc->signal_fmt = chain->signal_fmt;
    vgc->raster_fmt = chain->signal_fmt;
    vgc->raster_fmt.samples_per_line = signal_format_full_line(&chain->signal_fmt);
    vgc->raster_fmt.lines = chain->signal_fmt.region == SIGNAL_REGION_PAL ? 312 : 262;
    vgc->raster_fmt.total_samples = vgc->raster_fmt.samples_per_line * vgc->raster_fmt.lines;
    vgc->signal_line_phase = signal_region_line_phase(chain->signal_fmt.region);
    /* Decode everything the receiver scans, the console's border included. */
    decode_window_raster(&vgc->window, chain->signal_fmt.region, chain->signal_fmt.samples_per_pixel,
                         chain->signal_fmt.dots_per_line, SIGNAL_PICTURE_DOT);
    vgc->backdrop_entry = 0x0f;

    /* Initialize stage indices to -1 (not registered). */
    vgc->stage_console_hp  = -1;
    vgc->stage_console_lp  = -1;
    vgc->stage_cable_rc    = -1;
    vgc->stage_tv_input_hp = -1;
    vgc->stage_rf          = -1;
    vgc->vhs.stage_tape = vgc->vhs.stage_playback = -1;
    vgc->stage_osd = -1;
    vgc->stage_rf_if   = -1;
    vgc->stage_agc         = -1;
    vgc->stage_agc_loop    = -1;
    vgc->stage_ghosting    = -1;
    vgc->stage_luma_fir    = -1;
    vgc->stage_luma_peaking = -1;
    vgc->stage_chroma_demod = -1;
    vgc->stage_chroma_i_fir = -1;
    vgc->stage_chroma_q_fir = -1;
    vgc->stage_pal_chroma  = -1;
    vgc->stage_matrix      = -1;
    vgc->stage_post_pipeline = -1;
    vgc->stage_deflection  = -1;
    vgc->stage_beam_output = -1;
    vgc->stage_comb_bandpass = -1;
    vgc->stage_comb        = -1;

    /* ---- Validate FIR parameters ---- */
    if (fir_y_n <= 0 || fir_y_n > VIDEO_GPU_MAX_FIR_TAPS) {
        fprintf(stderr, "video_gpu_init: invalid fir_y_n %d (max %d)\n",
                fir_y_n, VIDEO_GPU_MAX_FIR_TAPS);
        return false;
    }
    if (fir_c_n <= 0 || fir_c_n > VIDEO_GPU_MAX_FIR_TAPS) {
        fprintf(stderr, "video_gpu_init: invalid fir_c_n %d (max %d)\n",
                fir_c_n, VIDEO_GPU_MAX_FIR_TAPS);
        return false;
    }
    if (fir_q_n <= 0 || fir_q_n > VIDEO_GPU_MAX_FIR_TAPS) {
        fprintf(stderr, "video_gpu_init: invalid fir_q_n %d (max %d)\n",
                fir_q_n, VIDEO_GPU_MAX_FIR_TAPS);
        return false;
    }

    /* Copy FIR taps locally. */
    memcpy(vgc->fir_y_taps, fir_y_taps, (size_t)fir_y_n * sizeof(float));
    vgc->fir_y_n = fir_y_n;
    memcpy(vgc->fir_c_taps, fir_c_taps, (size_t)fir_c_n * sizeof(float));
    vgc->fir_c_n = fir_c_n;
    memcpy(vgc->fir_q_taps, fir_q_taps, (size_t)fir_q_n * sizeof(float));
    vgc->fir_q_n = fir_q_n;

    /* ---- Compute buffer sizes ---- */
    const SignalFormat *fmt = &vgc->raster_fmt;
    int total_samples = fmt->total_samples;

    vgc->signal_size = (Uint32)(total_samples * sizeof(float));
    vgc->rgb_size = (Uint32)(decode_window_samples(&vgc->window) * 3 * sizeof(float));

    /* ---- Initialize the generic signal chain ---- */
    if (!chain_init(&vgc->sig_chain, gpu, total_samples, shader_dir)) {
        fprintf(stderr, "video_gpu_init: signal chain init failed\n");
        return false;
    }
    vgc->sig_chain.samples_per_line = fmt->samples_per_line;

    /* Per-line references, the carried loop state and the second-order
     * horizontal loop's integrator and V-blank line count. */
    vgc->buf_receiver = gpu_buffer_create(gpu, (Uint32)(fmt->lines + 2) * 4 * sizeof(float), GPU_BUF_READWRITE);
    vgc->buf_receiver_measurements = gpu_buffer_create(gpu, (Uint32)fmt->lines * 4 * sizeof(float), GPU_BUF_READWRITE);
    if (!vgc->buf_receiver || !vgc->buf_receiver_measurements) goto fail;
    float receiver_zero[314 * 4] = {0};
    if (!gpu_buffer_upload(gpu, vgc->buf_receiver, receiver_zero, (fmt->lines + 2) * 4 * sizeof(float))) goto fail;

    /* ---- Upload FIR tap coefficients ---- */
    int taps_y_idx = chain_upload_taps(&vgc->sig_chain, gpu,
                                        fir_y_taps, fir_y_n);
    if (taps_y_idx < 0) {
        fprintf(stderr, "video_gpu_init: failed to upload Y FIR taps\n");
        goto fail;
    }

    int taps_c_idx = chain_upload_taps(&vgc->sig_chain, gpu,
                                        fir_c_taps, fir_c_n);
    if (taps_c_idx < 0) {
        fprintf(stderr, "video_gpu_init: failed to upload C FIR taps\n");
        goto fail;
    }

    int taps_q_idx = chain_upload_taps(&vgc->sig_chain, gpu,
                                        fir_q_taps, fir_q_n);
    if (taps_q_idx < 0) {
        fprintf(stderr, "video_gpu_init: failed to upload Q FIR taps\n");
        goto fail;
    }

    /* ---- Build signal chain stages ---- */
    int dispatch_x_256  = (total_samples + 255) / 256;
    int dispatch_x_1024 = (total_samples + 1023) / 1024;

    /* Compute sample rate for RC alpha calculations from the live region. */
    float fsample = signal_format_sample_rate_hz(&vgc->signal_fmt);

    /* Add blanking/sync/burst before the analogue path so receiver timing
     * and DC restoration observe the same distortions as the picture. */
    GpuRasterParams raster_params = {0};
    vgc->stage_raster = chain_add_stage(&vgc->sig_chain, "PPU raster / sync / burst",
        CHAIN_KERNEL_RASTER, &raster_params, sizeof(raster_params), dispatch_x_256, 1);
    if (vgc->stage_raster < 0) goto fail;
    ChainStage *raster = &vgc->sig_chain.stages[vgc->stage_raster];
    raster->io_typed = true;
    raster->ro_count=2; raster->ro[0]=CBR_BUF_SRC; raster->ro[1]=CBR_AUX3;
    raster->rw_count=2; raster->rw[0]=CBR_BUF_DST; raster->rw[1]=CBR_AUX2;

    /* Legacy coupling-capacitor field is not implemented here. The
     * receiver performs measured back-porch restoration; CRT amplifier
     * recovery is separate. Do not expose a capacitor control that does
     * nothing, or mislabel 75 ohm / 10 uF (212 Hz) as a sub-Hz pole. */
    vgc->stage_console_hp = -1;

    /* Stage 1: Console Output LP (RC lowpass, video amp bandwidth limit).
     * The equivalent console pole defaults to 6 MHz independently
     * of the receiver bandwidth; this is a generic approximation.
     * Using the TV's bandwidth here would double-filter (the luma FIR
     * already handles TV-side bandwidth limiting after Y/C separation). */
    {
        float fc = video_console_bandwidth(chain);
        float alpha = expf(-2.0f * (float)M_PI * fc / fsample);
        GpuRCFilterParams rc_params={0};
        rc_params.a            = alpha;
        rc_params.b            = 1.0f - alpha;
        rc_params.total_count  = (uint32_t)total_samples;
        rc_params.block_offset = 0;
        rc_params.samples_per_line = (uint32_t)vgc->raster_fmt.samples_per_line;
        rc_params.num_lines = (uint32_t)vgc->raster_fmt.lines;
        if(chain->signal_fmt.region==SIGNAL_REGION_NTSC &&
           (chain->connection==VIDEO_CONN_COMPOSITE || chain->connection==VIDEO_CONN_RF)) {
            rc_params.nonlinear_tau_samples=fmaxf(chain->console_phase_distortion_ns,0)*1e-9f*fsample;
            if(chain->console_follower_tau_ns>0) {
                rc_params.follower_k=expf(-1e9f/(chain->console_follower_tau_ns*fsample));
                rc_params.follower_headroom=2.0f;
            }
        }
        vgc->stage_console_lp = chain_add_stage(&vgc->sig_chain,
            "Console Output LP", CHAIN_KERNEL_RC_FILTER,
            &rc_params, sizeof(rc_params), dispatch_x_1024, 1);
        if (vgc->stage_console_lp >= 0)
            chain_set_stage_enabled(&vgc->sig_chain, vgc->stage_console_lp,
                video_chain_stage_active(chain, 2));
    }

    /* Short-lead equivalent shunt capacitance; see video_cable_bandwidth. */
    {
        float cable_bw = video_cable_bandwidth(chain);
        float alpha = expf(-2.0f * (float)M_PI * cable_bw / fsample);
        GpuRCFilterParams rc_params={0};
        rc_params.a            = alpha;
        rc_params.b            = 1.0f - alpha;
        rc_params.total_count  = (uint32_t)total_samples;
        rc_params.block_offset = 0;
        rc_params.samples_per_line = (uint32_t)vgc->raster_fmt.samples_per_line;
        rc_params.num_lines = (uint32_t)vgc->raster_fmt.lines;
        vgc->stage_cable_rc = chain_add_stage(&vgc->sig_chain,
            "Cable RC", CHAIN_KERNEL_RC_FILTER,
            &rc_params, sizeof(rc_params), dispatch_x_1024, 1);
        if (vgc->stage_cable_rc >= 0)
            chain_set_stage_enabled(&vgc->sig_chain, vgc->stage_cable_rc,
                video_chain_stage_active(chain, 3));
    }

    // The source Y signal follows the same linear console/cable transfer.
    // Both filters are in-place on AUX2; no extra full-frame buffers needed.
    int sources[2]={vgc->stage_console_lp,vgc->stage_cable_rc};
    int *stages[2]={&vgc->stage_y_console,&vgc->stage_y_cable};
    const char *names[2]={"Y console bandwidth","Y cable bandwidth"};
    for(int i=0;i<2;i++) {
        ChainStage *source=&vgc->sig_chain.stages[sources[i]];
        int index=chain_add_stage(&vgc->sig_chain,names[i],CHAIN_KERNEL_RC_FILTER,
                                  source->params,source->params_size,1,1);
        if(index<0) goto fail;
        *stages[i]=index;
        ChainStage *stage=&vgc->sig_chain.stages[index];
        stage->io_typed=true; stage->ro_count=0; stage->rw_count=2;
        stage->rw[0]=CBR_AUX2; stage->rw[1]=CBR_CARRY;
        stage->enabled=false;
    }

    /* DC restoration is handled by the receiver's porch measurement. */
    vgc->stage_tv_input_hp = -1;

    /* AM channel -> complex IF -> envelope, before TV reception. */
    {
        vgc->buf_rf_carrier=gpu_buffer_create(gpu,(Uint32)total_samples*2*sizeof(float),GPU_BUF_READWRITE);
        if(!vgc->buf_rf_carrier) goto fail;
        GpuRFParams rp={0};
        rp.count=(uint32_t)total_samples;
        rp.samples_per_line=(uint32_t)fmt->samples_per_line;
        rp.noise_amplitude=video_rf_noise_rms(&chain->rf);
        rp.hum_amplitude=fmaxf(0,chain->console_psu_hum);
        vgc->stage_rf=chain_add_stage(&vgc->sig_chain,"RF AM channel",CHAIN_KERNEL_RF,
                                     &rp,sizeof(rp),dispatch_x_256,1);
        float taps[2*SIGNAL_RF_IF_TAPS];
        signal_design_rf_if(taps,fsample,chain->rf.mod_bandwidth>0 ? chain->rf.mod_bandwidth : 4e6f,
                            chain->rf.if_asymmetry,chain->rf.tuning_offset_hz);
        int ti=chain_upload_taps(&vgc->sig_chain,gpu,taps,2*SIGNAL_RF_IF_TAPS);
        GpuRFIFParams ip={(uint32_t)total_samples,(uint32_t)fmt->samples_per_line,SIGNAL_RF_IF_TAPS,
                          chain->rf.detector==2 ? 0u : 1u};
        vgc->stage_rf_if=chain_add_stage(&vgc->sig_chain,"RF IF / envelope",CHAIN_KERNEL_RF_IF,
                                        &ip,sizeof(ip),dispatch_x_256,1);
        if(vgc->stage_rf<0 || vgc->stage_rf_if<0 || ti<0) goto fail;
        vgc->sig_chain.stages[vgc->stage_rf].external[0]=vgc->buf_rf_carrier;
        vgc->sig_chain.stages[vgc->stage_rf_if].external[0]=vgc->buf_rf_carrier;
        vgc->sig_chain.stages[vgc->stage_rf_if].taps_index=ti;
        vgc->sig_chain.stages[vgc->stage_rf_if].rebind=rebind_rf_if;
        vgc->sig_chain.stages[vgc->stage_rf_if].rebind_user=vgc;
        // The scalar visualizer exposes voltage before modulation; the next
        // tap is detected voltage. Do not display interleaved I/Q as scanlines.
        vgc->sig_chain.stages[vgc->stage_rf].snapshot_src=CBR_BUF_SRC;
        chain_set_stage_enabled(&vgc->sig_chain,vgc->stage_rf,video_chain_stage_active(chain,4));
        chain_set_stage_enabled(&vgc->sig_chain,vgc->stage_rf_if,video_chain_stage_active(chain,4));
    }

    /* Stage 4c: the set's keyed top-sync AGC on RF. The loop stage walks the
     * frame's lines with one capacitor state carried across frames and
     * writes a gain per line; the apply stage scales the lines. */
    {
        GpuAGCParams agc_params = agc_params_from(vgc);
        vgc->buf_agc_gains = gpu_buffer_create(gpu, (Uint32)(vgc->raster_fmt.lines + 1) * 4 * sizeof(float), GPU_BUF_READWRITE);
        if (!vgc->buf_agc_gains) goto fail;
        float *zeros = calloc((size_t)(vgc->raster_fmt.lines + 1) * 4, sizeof(float));
        bool cleared = zeros && gpu_buffer_upload(gpu, vgc->buf_agc_gains, zeros, (Uint32)(vgc->raster_fmt.lines + 1) * 4 * sizeof(float));
        free(zeros);
        if (!cleared) goto fail;
        vgc->stage_agc_loop = chain_add_stage(&vgc->sig_chain, "AGC loop", CHAIN_KERNEL_AGC_LOOP,
            &agc_params, sizeof(agc_params), 1, 1);
        vgc->stage_agc = chain_add_stage(&vgc->sig_chain, "AGC", CHAIN_KERNEL_AGC,
            &agc_params, sizeof(agc_params), dispatch_x_256, 1);
        if (vgc->stage_agc_loop < 0 || vgc->stage_agc < 0) goto fail;
        vgc->sig_chain.stages[vgc->stage_agc_loop].external[0] = vgc->buf_agc_gains;
        vgc->sig_chain.stages[vgc->stage_agc].external[0] = vgc->buf_agc_gains;
        chain_set_stage_enabled(&vgc->sig_chain, vgc->stage_agc_loop, chain->connection==VIDEO_CONN_RF);
        chain_set_stage_enabled(&vgc->sig_chain, vgc->stage_agc, chain->connection==VIDEO_CONN_RF);
    }

    /* Stage 5: Ghosting (cable impedance reflection).
     * Adds a delayed, attenuated copy of the signal simulating cable
     * impedance mismatch reflections. Only enabled for presets with
     * ghost_level > 0 in their cable params. */
    {
        GpuDelayParams ghost_params;
        ghost_params.count         = (uint32_t)total_samples;
        ghost_params.mode          = 1;   /* ghost mode */
        ghost_params.delay_samples = chain->cable.ghost_delay > 0
            ? chain->cable.ghost_delay : 16;
        ghost_params.level         = chain->cable.ghost_level;

        vgc->stage_ghosting = chain_add_stage(&vgc->sig_chain,
            "Ghosting", CHAIN_KERNEL_DELAY,
            &ghost_params, sizeof(ghost_params), dispatch_x_256, 1);
        if (vgc->stage_ghosting >= 0)
            chain_set_stage_enabled(&vgc->sig_chain, vgc->stage_ghosting,
                chain->cable.ghost_level > 0.001f);
    }

    ChainStage *ghost = &vgc->sig_chain.stages[vgc->stage_ghosting];
    vgc->stage_y_ghost = chain_add_stage(&vgc->sig_chain, "Y cable reflection", CHAIN_KERNEL_DELAY,
        ghost->params, ghost->params_size, dispatch_x_256, 1);
    if (vgc->stage_y_ghost < 0) goto fail;
    ChainStage *y_ghost = &vgc->sig_chain.stages[vgc->stage_y_ghost];
    y_ghost->io_typed = true;
    y_ghost->ro_count = 2; y_ghost->ro[0] = CBR_AUX2; y_ghost->ro[1] = CBR_AUX0;
    y_ghost->rw_count = 1; y_ghost->rw[0] = CBR_AUX3;
    y_ghost->enabled = false;

    if (!vhs_gpu_init(&vgc->vhs, &vgc->sig_chain, gpu) ||
        !vhs_gpu_configure(&vgc->vhs, &vgc->sig_chain, gpu, &chain->vhs, fsample)) goto fail;
    vhs_gpu_set_enabled(&vgc->vhs, &vgc->sig_chain, vhs_active(chain));

    uint32_t receiver_params[] = {(uint32_t)fmt->lines, (uint32_t)fmt->samples_per_line,
        (uint32_t)fmt->samples_per_pixel, (uint32_t)fmt->region};
    vgc->stage_receiver = chain_add_stage(&vgc->sig_chain, "Sync / burst detector",
        CHAIN_KERNEL_RECEIVER, receiver_params, sizeof(receiver_params), gpu_workgroup_count((uint32_t)fmt->lines, CHAIN_SCANLINE_WORKGROUP_SIZE), 1);
    if (vgc->stage_receiver < 0) goto fail;
    ChainStage *receiver = &vgc->sig_chain.stages[vgc->stage_receiver];
    receiver->io_typed=true;
    /* The separator reads the previous frame's loop state to key its
     * gates from the flywheel when a line's edge is lost in snow. */
    receiver->ro_count=2; receiver->ro[0]=CBR_BUF_SRC; receiver->ro[1]=CBR_EXT1;
    receiver->rw_count=1; receiver->rw[0]=CBR_EXT0;
    receiver->external[0]=vgc->buf_receiver_measurements;
    receiver->external[1]=vgc->buf_receiver;
    GpuReceiverPLLParams pll_params = receiver_pll_params(vgc);
    vgc->stage_receiver_pll = chain_add_stage(&vgc->sig_chain, "Receiver PLL / clamp",
        CHAIN_KERNEL_RECEIVER_PLL, &pll_params, sizeof(pll_params), 1, 1);
    if (vgc->stage_receiver_pll < 0) goto fail;
    ChainStage *pll = &vgc->sig_chain.stages[vgc->stage_receiver_pll];
    pll->io_typed = true;
    pll->ro_count = 1; pll->ro[0] = CBR_EXT0; pll->external[0] = vgc->buf_receiver_measurements;
    pll->rw_count = 1; pll->rw[0] = CBR_EXT1; pll->external[1] = vgc->buf_receiver;

    uint32_t yc_count=(uint32_t)total_samples;
    vgc->stage_yc_route=chain_add_stage(&vgc->sig_chain,"Separate Y/C input",CHAIN_KERNEL_YC_ROUTE,
                                      &yc_count,sizeof(yc_count),dispatch_x_256,1);
    if(vgc->stage_yc_route<0) goto fail;
    ChainStage *yc=&vgc->sig_chain.stages[vgc->stage_yc_route];
    yc->io_typed=true; yc->ro_count=2; yc->ro[0]=CBR_BUF_SRC; yc->ro[1]=CBR_AUX2;
    yc->rw_count=2; yc->rw[0]=CBR_BUF_DST; yc->rw[1]=CBR_AUX0;
    yc->enabled=false;

    /* Only the chroma band is line-combed. Averaging full composite erases
     * horizontal boundaries even when they contain no chroma. */
    {
        float taps[49];
        signal_design_chroma_bandpass(taps,49,fsample,fmaxf(chain->tv.chroma_bandwidth,0.25e6f));
        int ti=chain_upload_taps(&vgc->sig_chain,gpu,taps,49);
        if(ti<0) goto fail;
        GpuFIRParams bp={(uint32_t)total_samples,(uint32_t)total_samples,49,1,
                         (uint32_t)fmt->samples_per_line};
        vgc->stage_comb_bandpass=chain_add_stage(&vgc->sig_chain,"Comb chroma band",
            CHAIN_KERNEL_FIR,&bp,sizeof(bp),dispatch_x_256,1);
        if(vgc->stage_comb_bandpass<0) goto fail;
        ChainStage *s=&vgc->sig_chain.stages[vgc->stage_comb_bandpass];
        s->taps_index=ti; s->rw[0]=CBR_AUX1;
        s->enabled=video_chain_stage_active(chain,6) && (chain->signal_fmt.region==SIGNAL_REGION_NTSC ? video_chain_comb_shader_mode(chain->comb_type) : 0u)>0;
    }

    /* Stage 6: Comb Filter (Y/C separator).
     * Separates luma and chroma by exploiting the 180° subcarrier phase
     * inversion between adjacent scanlines. Writes Y to buf[dst] and
     * C to aux[0]. When enabled, the chroma demod reads from aux[0]
     * instead of buf[0] (raw composite). */
    {
        /* Single source of truth for the comb_type → shader-mode map
         * lives in video_chain.h so all sites agree. */
        uint32_t shader_mode = (chain->signal_fmt.region==SIGNAL_REGION_NTSC ? video_chain_comb_shader_mode(chain->comb_type) : 0u);

        GpuCombParams comb_params;
        comb_params.delay_samples = chain->signal_fmt.region == SIGNAL_REGION_NTSC ? 2730u : 0u;
        comb_params.count             = (uint32_t)total_samples;
        comb_params.samples_per_line  = (uint32_t)fmt->samples_per_line;
        comb_params.mode              = shader_mode;
        /* Legacy field: fraction of extracted chroma subtracted from luma.
         * This is not a calibrated rejection ratio or a decoder quality rank. */
        if (chain->comb_notch_depth > 0.0f)
            comb_params.blend = chain->comb_notch_depth;
        else
            comb_params.blend = (chain->comb_type == VIDEO_COMB_1LINE) ? 0.65f : 0.85f;

        vgc->stage_comb = chain_add_stage(&vgc->sig_chain,
            "Comb Filter", CHAIN_KERNEL_COMB,
            &comb_params, sizeof(comb_params), dispatch_x_256, 1);
        if (vgc->stage_comb >= 0) {
            /* Enable only when comb is active AND mode is not NONE/BYPASS.
             * NONE and BYPASS both map to shader mode 0 (C=0). */
            bool comb_useful = video_chain_stage_active(chain, 6)
                && shader_mode > 0;  /* shader_mode 0 = bypass/none */
            chain_set_stage_enabled(&vgc->sig_chain, vgc->stage_comb, comb_useful);
        }
    }

    /* Stage 4: Luma FIR (Y bandwidth limit).
     * Reads from buf[current_buf] (RC-filtered composite or comb Y output),
     * writes to buf[1-current_buf] via ping-pong.
     * This extracts the luma component by rejecting the 3.58 MHz chroma
     * subcarrier, so the matrix decode receives clean Y instead of composite. */
    {
        GpuFIRParams fir_params;
        fir_params.input_count     = (uint32_t)total_samples;
        fir_params.output_count    = (uint32_t)total_samples;
        fir_params.tap_count       = (uint32_t)fir_y_n;
        fir_params.decimation_ratio = 1;
        fir_params.samples_per_line = (uint32_t)fmt->samples_per_line;

        vgc->stage_luma_fir = chain_add_stage(&vgc->sig_chain,
            "Luma FIR", CHAIN_KERNEL_FIR,
            &fir_params, sizeof(fir_params), dispatch_x_256, 1);
        if (vgc->stage_luma_fir < 0) {
            fprintf(stderr, "video_gpu_init: failed to add Luma FIR stage\n");
            goto fail;
        }
        vgc->sig_chain.stages[vgc->stage_luma_fir].taps_index = taps_y_idx;
        chain_set_stage_enabled(&vgc->sig_chain, vgc->stage_luma_fir,
            video_chain_stage_active(chain, 8));
    }

    /* Stage 5: Chroma Demod (modulator IQ extraction).
     * Reads composite signal, writes I to aux[0] and Q to aux[1].
     * This is a dual-output stage — the chain runner writes the second
     * output to the readwrite binding slot 1 (aux[0]) via the
     * modulator's mode 3 IQ demod. */
    {
        GpuModulatorParams demod_params;
        demod_params.count            = (uint32_t)total_samples;
        demod_params.mode             = 3;   /* IQ demod */
        demod_params.phase            = 0.0f;
        demod_params.dp               = 2.0f * (float)M_PI / 12.0f;
        demod_params.param_a          = 1.0f;
        demod_params.samples_per_line = (uint32_t)fmt->samples_per_line;
        demod_params.line_phase_inc   = (float)chain->signal_fmt.lines > 0
            ? (float)signal_region_line_phase(vgc->signal_fmt.region) * 2.0f * (float)M_PI / 12.0f  /* match the DAC clock */
            : 0.0f;
        demod_params.burst_reference  = demod_burst_reference(vgc);

        vgc->stage_chroma_demod = chain_add_stage(&vgc->sig_chain,
            "Chroma Demod", CHAIN_KERNEL_RECEIVER_DEMOD,
            &demod_params, sizeof(demod_params), dispatch_x_256, 1);
        if (vgc->stage_chroma_demod < 0) {
            fprintf(stderr, "video_gpu_init: failed to add Chroma Demod stage\n");
            goto fail;
        }
        /* Chroma stages use typed bindings + a per-frame rebind hook
         * (installed below, after the Q FIR slot is registered). They
         * run under chain_run_cmd's typed dispatcher — no custom
         * hook. */
        chain_set_stage_enabled(&vgc->sig_chain, vgc->stage_chroma_demod, true);
    }

    /* Stage 6: Chroma I FIR (I bandwidth limit).
     * NOTE: This stage needs to read from aux[0] (demod I output) and
     * write to aux[2]. The chain runner doesn't natively support
     * aux-to-aux FIR. For Phase 1, we disable this stage in the chain
     * and dispatch it manually after chain_run. */
    {
        GpuFIRParams fir_params;
        fir_params.input_count     = (uint32_t)total_samples;
        fir_params.output_count    = (uint32_t)total_samples;
        fir_params.tap_count       = (uint32_t)fir_c_n;
        fir_params.decimation_ratio = 1;
        fir_params.samples_per_line = (uint32_t)fmt->samples_per_line;

        vgc->stage_chroma_i_fir = chain_add_stage(&vgc->sig_chain,
            "Chroma I FIR", CHAIN_KERNEL_FIR,
            &fir_params, sizeof(fir_params), dispatch_x_256, 1);
        if (vgc->stage_chroma_i_fir < 0) {
            fprintf(stderr, "video_gpu_init: failed to add Chroma I FIR stage\n");
            goto fail;
        }
        vgc->sig_chain.stages[vgc->stage_chroma_i_fir].taps_index = taps_c_idx;
        /* Enabled — uses typed bindings + rebind (see
         * chroma_pipeline_install_typed called below). */
        chain_set_stage_enabled(&vgc->sig_chain, vgc->stage_chroma_i_fir, true);
    }

    /* Stage 7: Chroma Q FIR (Q bandwidth limit).
     * Uses its own tap buffer (taps_q_idx) so I and Q can be filtered
     * at different bandwidths — NTSC spec: I ≈ 1.3 MHz (wider), Q ≈
     * 0.5 MHz (narrower). Presets that want cheap-set equiband I/Q
     * just set chroma_q_bandwidth = 0 and get the same taps as I. */
    {
        GpuFIRParams fir_params;
        fir_params.input_count     = (uint32_t)total_samples;
        fir_params.output_count    = (uint32_t)total_samples;
        fir_params.tap_count       = (uint32_t)fir_q_n;
        fir_params.decimation_ratio = 1;
        fir_params.samples_per_line = (uint32_t)fmt->samples_per_line;

        vgc->stage_chroma_q_fir = chain_add_stage(&vgc->sig_chain,
            "Chroma Q FIR", CHAIN_KERNEL_FIR,
            &fir_params, sizeof(fir_params), dispatch_x_256, 1);
        if (vgc->stage_chroma_q_fir < 0) {
            fprintf(stderr, "video_gpu_init: failed to add Chroma Q FIR stage\n");
            goto fail;
        }
        vgc->sig_chain.stages[vgc->stage_chroma_q_fir].taps_index = taps_q_idx;
        /* Enabled — typed bindings + rebind below. */
        chain_set_stage_enabled(&vgc->sig_chain, vgc->stage_chroma_q_fir, true);
    }

    /* PAL-only decoder correction:
     *   - odd-line U sign compensation
     *   - 1H delay-line V averaging
     *
     * Mirrors the PAL-CRT-inspired CPU decoder in src/nes/composite.h so
     * the matrix stage receives a stable (V, U) pair instead of the raw
     * PAL demod outputs. NTSC skips this entirely. */
    if (fmt->region == SIGNAL_REGION_PAL) {
        struct {
            uint32_t count;
            uint32_t samples_per_line;
        } pal_params;
        pal_params.count = (uint32_t)total_samples;
        pal_params.samples_per_line = (uint32_t)fmt->samples_per_line;

        vgc->stage_pal_chroma = chain_add_stage(&vgc->sig_chain,
            "PAL Chroma", CHAIN_KERNEL_PAL_CHROMA,
            &pal_params, sizeof(pal_params), dispatch_x_256, 1);
        if (vgc->stage_pal_chroma < 0) {
            fprintf(stderr, "video_gpu_init: failed to add PAL Chroma stage\n");
            goto fail;
        }
        chain_set_stage_enabled(&vgc->sig_chain, vgc->stage_pal_chroma,
            video_chain_stage_active(chain, 7));
    }

    /* Now that all three chroma slots exist, install their typed
     * bindings + rebind hooks. PAL installs a fourth typed stage for
     * the delay-line decoder correction. chain_run_cmd drives them
     * directly — no custom-dispatch hook, no manual trampoline. */
    chroma_pipeline_install_typed(vgc);

    /* Chroma demod must consume the pre-Y ping-pong buffer first. Once
     * I/Q are in aux buffers we can safely advance the Y ping-pong again. */
    {
        float taps[LUMA_PEAKING_TAPS];
        luma_peaking_taps(chain, taps);
        int ti = chain_upload_taps(&vgc->sig_chain, gpu, taps, LUMA_PEAKING_TAPS);
        GpuFIRParams p = {(uint32_t)total_samples, (uint32_t)total_samples,
            LUMA_PEAKING_TAPS, 1, (uint32_t)fmt->samples_per_line};
        vgc->stage_luma_peaking = chain_add_stage(&vgc->sig_chain,
            "Luma sharpness", CHAIN_KERNEL_FIR, &p, sizeof(p), dispatch_x_256, 1);
        if (ti < 0 || vgc->stage_luma_peaking < 0) goto fail;
        vgc->sig_chain.stages[vgc->stage_luma_peaking].taps_index = ti;
        chain_set_stage_enabled(&vgc->sig_chain, vgc->stage_luma_peaking,
            luma_peaking_active(chain));
    }

    /* Matrix decode — fully typed chain stage. Reads Y from the main
     * ping-pong, I/Q from the aux slots set by the chroma rebind,
     * writes to vgc->buf_rgb via external[]. */
    {
        vgc->stage_matrix = chain_add_stage(&vgc->sig_chain,
            "Matrix Decode", CHAIN_KERNEL_MATRIX,
            NULL, 0, 1, 1);
        if (vgc->stage_matrix < 0) {
            fprintf(stderr, "video_gpu_init: failed to add Matrix Decode stage\n");
            goto fail;
        }
        /* post_pipeline_install_matrix_typed is called below (after
         * buf_rgb is allocated) so the rebind can hand out pointers. */
    }

    {
        vgc->stage_osd=chain_add_stage(&vgc->sig_chain,"TV RGB OSD",CHAIN_KERNEL_OSD,NULL,0,1,1);
        if(vgc->stage_osd<0) goto fail;
        ChainStage *s=&vgc->sig_chain.stages[vgc->stage_osd];
        osd_params(vgc,s);
        s->io_typed=true; s->rebind=rebind_osd; s->rebind_user=vgc;
        s->snapshot_src=CBR_EXT1; s->snapshot_size=vgc->rgb_size;
        chain_set_stage_enabled(&vgc->sig_chain,vgc->stage_osd,false);
    }

    /* RGB post stage — still custom because video_amp swaps buf_rgb /
     * buf_rgb2 as a side effect before h_blur consumes the result. */
    {
        vgc->stage_post_pipeline = chain_add_stage(&vgc->sig_chain,
            "RGB amplifiers", CHAIN_KERNEL_VIDEO_AMP,
            NULL, 0, 1, 1);
        if (vgc->stage_post_pipeline < 0) {
            fprintf(stderr, "video_gpu_init: failed to add RGB Post stage\n");
            goto fail;
        }
        vgc->sig_chain.stages[vgc->stage_post_pipeline].custom =
            post_rgb_chain_dispatch;
        vgc->sig_chain.stages[vgc->stage_post_pipeline].custom_user = vgc;
        vgc->sig_chain.stages[vgc->stage_post_pipeline].io_typed = false;
        chain_set_stage_enabled(&vgc->sig_chain, vgc->stage_post_pipeline, true);
    }

    vgc->stage_crt_load = chain_add_stage(&vgc->sig_chain, "CRT video rail loading",
        CHAIN_KERNEL_CRT_LOAD, NULL, 0, 1, 1);
    vgc->stage_crt_supply = chain_add_stage(&vgc->sig_chain, "CRT supply recovery",
        CHAIN_KERNEL_CRT_LOAD, NULL, 0, 1, 1);
    if (vgc->stage_crt_load < 0 || vgc->stage_crt_supply < 0) goto fail;

    /* Deflection map — typed stage producing coherent landing data for
     * the beam shader. Installed after the beam-sized buffers exist. */
    {
        vgc->stage_deflection = chain_add_stage(&vgc->sig_chain,
            "Deflection Map", CHAIN_KERNEL_DEFLECTION,
            NULL, 0, 1, 1);
        if (vgc->stage_deflection < 0) {
            fprintf(stderr, "video_gpu_init: failed to add Deflection Map stage\n");
            goto fail;
        }
    }

    /* Beam output — custom because temporal_blit writes a storage
     * texture and copies cur→prev as a side effect. */
    {
        vgc->stage_beam_output = chain_add_stage(&vgc->sig_chain,
            "Gun / beam / phosphor", CHAIN_KERNEL_BEAM,
            NULL, 0, 1, 1);
        if (vgc->stage_beam_output < 0) {
            fprintf(stderr, "video_gpu_init: failed to add Beam Output stage\n");
            goto fail;
        }
        vgc->sig_chain.stages[vgc->stage_beam_output].custom =
            beam_output_chain_dispatch;
        vgc->sig_chain.stages[vgc->stage_beam_output].custom_user = vgc;
        vgc->sig_chain.stages[vgc->stage_beam_output].io_typed = false;
        chain_set_stage_enabled(&vgc->sig_chain, vgc->stage_beam_output, false);
    }

    /* ---- Load DAC pipeline (separate from chain) ---- */
    {
        char *path = build_path(shader_dir, "dac_2c02.comp.spv");
        if (!path) { fprintf(stderr, "video_gpu_init: alloc failed\n"); goto fail; }
        GpuPipelineResources res = {0};
        /* 3 RO = index_data + signal_table + signal_table_alt (PAL). */
        res.num_readonly_storage_buffers = 3;
        res.num_readwrite_storage_buffers = 2;
        res.num_uniform_buffers = 1;
        bool ok = gpu_pipeline_create(gpu, &vgc->pipe_dac, path, "main",
                                      256, 1, 1, &res, "video_dac");
        free(path);
        if (!ok) { fprintf(stderr, "video_gpu_init: DAC shader failed (non-fatal)\n"); }
    }

    /* ---- RGB console encoder source (optional; used by video_gpu_process_rgb) ---- */
    {
        char *path = build_path(shader_dir, "encoder_rgb.comp.spv");
        if (!path) { fprintf(stderr, "video_gpu_init: alloc failed\n"); goto fail; }
        GpuPipelineResources res = {0};
        res.num_readonly_storage_buffers = 2;   /* pixels + ramp */
        res.num_readwrite_storage_buffers = 2;  /* waveform + luma */
        res.num_uniform_buffers = 1;
        bool ok = gpu_pipeline_create(gpu, &vgc->pipe_encoder, path, "main",
                                      256, 1, 1, &res, "video_encoder_rgb");
        free(path);
        if (!ok) { fprintf(stderr, "video_gpu_init: RGB encoder shader failed (non-fatal)\n"); }
    }
    /* The raster draws the 2C02's sync and square burst unless a source
     * replaces them with an encoder IC's standard levels. */
    vgc->raster_sync_level = -264.0f / 788.0f;
    vgc->raster_burst_amp = 0.0f;
    vgc->raster_burst_sine = false;

    /* ---- Default demod parameters (NTSC colorburst) ---- */
    vgc->demod_line_phase = signal_region_line_phase(fmt->region) * 2.0f * (float)M_PI / 12.0f;
    vgc->demod_phase = 0.0f;
    vgc->demod_dp    = 2.0f * (float)M_PI / 12.0f;

    /* Default identity color matrix. */
    memset(vgc->color_matrix, 0, sizeof(vgc->color_matrix));
    vgc->color_matrix[0][0] = 1.0f;
    vgc->color_matrix[1][1] = 1.0f;
    vgc->color_matrix[2][2] = 1.0f;
    memset(vgc->color_bias, 0, sizeof(vgc->color_bias));

    /* RGB output buffers for GPU matrix decode + video amp.
     * buf_rgb: matrix decode writes here (interleaved R,G,B).
     * buf_rgb2: video amp reads buf_rgb, writes here. */
    vgc->buf_rgb = gpu_buffer_create(gpu, vgc->rgb_size, GPU_BUF_READWRITE);
    if (!vgc->buf_rgb) {
        fprintf(stderr, "video_gpu_init: failed to create RGB output buffer\n");
        goto fail;
    }
    vgc->buf_rgb2 = gpu_buffer_create(gpu, vgc->rgb_size, GPU_BUF_READWRITE);
    if (!vgc->buf_rgb2) {
        fprintf(stderr, "video_gpu_init: failed to create RGB2 buffer (non-fatal)\n");
    }
    vgc->buf_gun_current = gpu_buffer_create(gpu, vgc->rgb_size, GPU_BUF_READWRITE);
    if (!vgc->buf_gun_current) goto fail;
    if (fmt->region == SIGNAL_REGION_PAL) {
        vgc->buf_pal_v = gpu_buffer_create(gpu, vgc->signal_size, GPU_BUF_READWRITE);
        vgc->buf_pal_u = gpu_buffer_create(gpu, vgc->signal_size, GPU_BUF_READWRITE);
        if (!vgc->buf_pal_v || !vgc->buf_pal_u) {
            fprintf(stderr, "video_gpu_init: failed to create PAL chroma buffers\n");
            goto fail;
        }
    }

    /* Wire the typed post-decode stages now that the backing buffers
     * exist. The deflection stage stays disabled until beam params
     * allocate its display-resolution landing buffers. */
    /* The load map: rail load per window dot, the mean current per row and
     * the supply's state. */
    size_t load_floats = (size_t)vgc->window.dots * vgc->window.lines + vgc->window.lines + 1;
    vgc->buf_crt_load = gpu_buffer_create(gpu, (Uint32)(load_floats*sizeof(float)), GPU_BUF_READWRITE);
    if (!vgc->buf_crt_load) goto fail;
    float *load_zero = calloc(load_floats, sizeof(float));
    if (!load_zero) goto fail;
    bool load_ok = gpu_buffer_upload(gpu, vgc->buf_crt_load, load_zero, (Uint32)(load_floats*sizeof(float)));
    free(load_zero);
    if (!load_ok) goto fail;
    post_pipeline_install_load_typed(vgc);
    post_pipeline_install_matrix_typed(vgc);
    post_pipeline_install_deflection_typed(vgc);
    vgc->sig_chain.stages[vgc->stage_post_pipeline].snapshot_src = CBR_EXT0;
    vgc->sig_chain.stages[vgc->stage_post_pipeline].snapshot_size = vgc->rgb_size;
    vgc->sig_chain.stages[vgc->stage_post_pipeline].external[0] = vgc->buf_rgb2;

    update_video_amp(vgc);
    vgc->sig_chain.first_stage=video_connection_uses_signal_decode(chain->connection) ? 0 : vgc->stage_osd;

    vgc->timing_enabled = false;
    vgc->frame_brightness = 0.0f;
    vgc->apl_smoothed = 0.5f;
    vgc->audio_bass_rms = 0.0f;

    LOGV("video_gpu_init: ready (signal %u bytes [%dx%d], "
         "Y taps %d, C taps %d, %d chain stages)\n",
         vgc->signal_size, fmt->samples_per_line, fmt->lines,
         fir_y_n, fir_c_n, chain_get_num_stages(&vgc->sig_chain));

    /* Verbose diagnostic: dump FIR taps and enabled chain stages. */
    if (gpu_verbose) {
        LOGV("  Y FIR taps[0..%d]: ", fir_y_n - 1);
        for (int i = 0; i < fir_y_n && i < 8; i++)
            LOGV("%.4f ", fir_y_taps[i]);
        if (fir_y_n > 8) LOGV("...");
        LOGV("\n");

        LOGV("  C FIR taps[0..%d]: ", fir_c_n - 1);
        for (int i = 0; i < fir_c_n && i < 8; i++)
            LOGV("%.4f ", fir_c_taps[i]);
        if (fir_c_n > 8) LOGV("...");
        LOGV("\n");

        LOGV("  Chain stages:\n");
        for (int i = 0; i < vgc->sig_chain.num_stages; i++) {
            ChainStage *s = &vgc->sig_chain.stages[i];
            LOGV("    [%d] %-20s kernel=%d enabled=%d bypass=%d dx=%u\n",
                 i, s->name, s->kernel_type, s->enabled, s->bypass, s->dispatch_x);
        }
    }

    return true;

fail:
    video_gpu_destroy(vgc, gpu);
    return false;
}

/* ============================================================================
 * Parameter updates
 * ============================================================================ */

void video_gpu_update_rc_params(VideoGPUChain *vgc)
{
    const VideoChain *chain = vgc->chain;
    GpuReceiverPLLParams pll_params = receiver_pll_params(vgc);
    if (vgc->stage_receiver_pll >= 0)
        chain_update_params(&vgc->sig_chain, vgc->stage_receiver_pll, &pll_params, sizeof(pll_params));
    int total_samples = vgc->raster_fmt.total_samples;
    float fsample = signal_format_sample_rate_hz(&vgc->signal_fmt);

    if(vgc->stage_rf>=0)
        ((GpuRFParams *)vgc->sig_chain.stages[vgc->stage_rf].params)->hum_amplitude=fmaxf(0,chain->console_psu_hum);
    if(vgc->stage_ghosting>=0)
        chain_set_stage_enabled(&vgc->sig_chain,vgc->stage_ghosting,chain->cable.ghost_level>.001f);

    /* Console Output LP: Generic console output pole; not a measured universal 2C02 bandwidth. */
    if (vgc->stage_console_lp >= 0) {
        float fc = video_console_bandwidth(chain);
        float alpha = expf(-2.0f * (float)M_PI * fc / fsample);
        GpuRCFilterParams rc_params={0};
        rc_params.a            = alpha;
        rc_params.b            = 1.0f - alpha;
        rc_params.total_count  = (uint32_t)total_samples;
        rc_params.block_offset = 0;
        rc_params.samples_per_line = (uint32_t)vgc->raster_fmt.samples_per_line;
        rc_params.num_lines = (uint32_t)vgc->raster_fmt.lines;
        if(chain->signal_fmt.region==SIGNAL_REGION_NTSC &&
           (chain->connection==VIDEO_CONN_COMPOSITE || chain->connection==VIDEO_CONN_RF)) {
            rc_params.nonlinear_tau_samples=fmaxf(chain->console_phase_distortion_ns,0)*1e-9f*fsample;
            if(chain->console_follower_tau_ns>0) {
                rc_params.follower_k=expf(-1e9f/(chain->console_follower_tau_ns*fsample));
                rc_params.follower_headroom=2.0f;
            }
        }
        chain_update_params(&vgc->sig_chain, vgc->stage_console_lp,
                            &rc_params, sizeof(rc_params));
    }

    if (vgc->stage_cable_rc >= 0) {
        float cable_bw = video_cable_bandwidth(chain);
        float alpha = expf(-2.0f * (float)M_PI * cable_bw / fsample);
        GpuRCFilterParams rc_params={0};
        rc_params.a            = alpha;
        rc_params.b            = 1.0f - alpha;
        rc_params.total_count  = (uint32_t)total_samples;
        rc_params.block_offset = 0;
        rc_params.samples_per_line = (uint32_t)vgc->raster_fmt.samples_per_line;
        rc_params.num_lines = (uint32_t)vgc->raster_fmt.lines;
        chain_update_params(&vgc->sig_chain, vgc->stage_cable_rc,
                            &rc_params, sizeof(rc_params));
    }

    /* Ghosting stage: update delay and level from cable params on-the-fly. */
    if (vgc->stage_ghosting >= 0) {
        GpuDelayParams ghost_params;
        ghost_params.count         = (uint32_t)total_samples;
        ghost_params.mode          = 1;   /* ghost mode */
        ghost_params.delay_samples = chain->cable.ghost_delay > 0
            ? chain->cable.ghost_delay : 16;
        ghost_params.level         = chain->cable.ghost_level;
        chain_update_params(&vgc->sig_chain, vgc->stage_ghosting,
                            &ghost_params, sizeof(ghost_params));
    }
}

void video_gpu_set_color_matrix(VideoGPUChain *vgc,
                                const float matrix[3][3],
                                const float bias[3])
{
    memcpy(vgc->color_matrix, matrix, sizeof(vgc->color_matrix));
    memcpy(vgc->color_bias, bias, sizeof(vgc->color_bias));
    LOGV("video_gpu_set_color_matrix:\n"
            "  [%.4f %.4f %.4f | bias %.4f]\n"
            "  [%.4f %.4f %.4f | bias %.4f]\n"
            "  [%.4f %.4f %.4f | bias %.4f]\n",
            matrix[0][0], matrix[0][1], matrix[0][2], bias[0],
            matrix[1][0], matrix[1][1], matrix[1][2], bias[1],
            matrix[2][0], matrix[2][1], matrix[2][2], bias[2]);
}

bool video_gpu_update_fir_taps(VideoGPUChain *vgc, SDL_GPUDevice *gpu,
                               const float *fir_y_taps, int fir_y_n,
                               const float *fir_c_taps, int fir_c_n,
                               const float *fir_q_taps, int fir_q_n)
{
    if (fir_y_n <= 0 || fir_y_n > VIDEO_GPU_MAX_FIR_TAPS ||
        fir_c_n <= 0 || fir_c_n > VIDEO_GPU_MAX_FIR_TAPS ||
        fir_q_n <= 0 || fir_q_n > VIDEO_GPU_MAX_FIR_TAPS) {
        fprintf(stderr, "video_gpu_update_fir_taps: invalid tap counts "
                "(%d, %d, %d)\n", fir_y_n, fir_c_n, fir_q_n);
        return false;
    }

    memcpy(vgc->fir_y_taps, fir_y_taps, (size_t)fir_y_n * sizeof(float));
    vgc->fir_y_n = fir_y_n;
    memcpy(vgc->fir_c_taps, fir_c_taps, (size_t)fir_c_n * sizeof(float));
    vgc->fir_c_n = fir_c_n;
    memcpy(vgc->fir_q_taps, fir_q_taps, (size_t)fir_q_n * sizeof(float));
    vgc->fir_q_n = fir_q_n;

    /* Re-upload tap buffers via the chain's tap buffer infrastructure.
     * The chain owns the tap buffers — we upload new data to the existing
     * buffer objects. If the new tap count exceeds the buffer size, we need
     * to recreate (but chain_upload_taps always creates a new buffer, so
     * for a hot update we upload directly to the existing buffer). */
    SignalChain *sc = &vgc->sig_chain;
    int y_tap_idx = vgc->sig_chain.stages[vgc->stage_luma_fir].taps_index;
    int c_tap_idx = vgc->sig_chain.stages[vgc->stage_chroma_i_fir].taps_index;
    int q_tap_idx = vgc->sig_chain.stages[vgc->stage_chroma_q_fir].taps_index;
    float comb_taps[49];
    signal_design_chroma_bandpass(comb_taps,49,signal_format_sample_rate_hz(&vgc->signal_fmt),
        fmaxf(vgc->chain->tv.chroma_bandwidth,0.25e6f));
    int comb_tap_idx=sc->stages[vgc->stage_comb_bandpass].taps_index;
    float rf_taps[2*SIGNAL_RF_IF_TAPS];
    float rf_bw=vgc->chain->rf.mod_bandwidth>0 ? vgc->chain->rf.mod_bandwidth : 4e6f;
    signal_design_rf_if(rf_taps,signal_format_sample_rate_hz(&vgc->signal_fmt),rf_bw,
                        vgc->chain->rf.if_asymmetry,vgc->chain->rf.tuning_offset_hz);
    int rf_tap_idx=sc->stages[vgc->stage_rf_if].taps_index;

    if (!vhs_gpu_configure(&vgc->vhs, sc, gpu, &vgc->chain->vhs, signal_format_sample_rate_hz(&vgc->signal_fmt)))
        return false;
    vhs_gpu_set_enabled(&vgc->vhs, sc, vhs_active(vgc->chain));

    float peaking_taps[LUMA_PEAKING_TAPS];
    luma_peaking_taps(vgc->chain, peaking_taps);
    int peaking_idx = sc->stages[vgc->stage_luma_peaking].taps_index;

    struct { int idx; int n; const float *taps; const char *label; } uploads[] = {
        { peaking_idx, LUMA_PEAKING_TAPS, peaking_taps, "Luma sharpness" },
        { y_tap_idx, fir_y_n, fir_y_taps, "Y" },
        { c_tap_idx, fir_c_n, fir_c_taps, "I" },
        { q_tap_idx, fir_q_n, fir_q_taps, "Q" },
        { rf_tap_idx, 2*SIGNAL_RF_IF_TAPS, rf_taps, "RF IF" },
        { comb_tap_idx, 49, comb_taps, "Comb band" },
    };
    for (size_t u = 0; u < sizeof(uploads) / sizeof(uploads[0]); u++) {
        int idx = uploads[u].idx;
        if (idx < 0 || idx >= sc->num_tap_bufs) continue;
        Uint32 new_size = (Uint32)(uploads[u].n * sizeof(float));
        if (new_size > sc->tap_sizes[idx]) {
            SDL_ReleaseGPUBuffer(gpu, sc->tap_bufs[idx]);
            sc->tap_bufs[idx] = gpu_buffer_create(gpu, new_size, GPU_BUF_READONLY);
            if (!sc->tap_bufs[idx]) return false;
            sc->tap_sizes[idx] = new_size;
        }
        if (!gpu_buffer_upload(gpu, sc->tap_bufs[idx], uploads[u].taps, new_size)) {
            fprintf(stderr, "video_gpu_update_fir_taps: %s taps upload failed\n",
                    uploads[u].label);
            return false;
        }
    }

    /* Update the FIR uniform params (tap count may have changed). */
    chain_set_stage_enabled(sc, vgc->stage_luma_peaking, luma_peaking_active(vgc->chain));
    {
        GpuFIRParams fir_y;
        fir_y.input_count     = (uint32_t)vgc->raster_fmt.total_samples;
        fir_y.output_count    = (uint32_t)vgc->raster_fmt.total_samples;
        fir_y.tap_count       = (uint32_t)fir_y_n;
        fir_y.decimation_ratio = 1;
        fir_y.samples_per_line = (uint32_t)vgc->raster_fmt.samples_per_line;
        chain_update_params(&vgc->sig_chain, vgc->stage_luma_fir,
                            &fir_y, sizeof(fir_y));
    }
    {
        GpuFIRParams fir_c;
        memset(&fir_c, 0, sizeof(fir_c));
        fir_c.input_count      = (uint32_t)vgc->raster_fmt.total_samples;
        fir_c.output_count     = (uint32_t)vgc->raster_fmt.total_samples;
        fir_c.tap_count        = (uint32_t)fir_c_n;
        fir_c.decimation_ratio = 1;
        fir_c.samples_per_line = (uint32_t)vgc->raster_fmt.samples_per_line;
        chain_update_params(&vgc->sig_chain, vgc->stage_chroma_i_fir,
                            &fir_c, sizeof(fir_c));
    }
    {
        GpuFIRParams fir_q;
        memset(&fir_q, 0, sizeof(fir_q));
        fir_q.input_count      = (uint32_t)vgc->raster_fmt.total_samples;
        fir_q.output_count     = (uint32_t)vgc->raster_fmt.total_samples;
        fir_q.tap_count        = (uint32_t)fir_q_n;
        fir_q.decimation_ratio = 1;
        fir_q.samples_per_line = (uint32_t)vgc->raster_fmt.samples_per_line;
        chain_update_params(&vgc->sig_chain, vgc->stage_chroma_q_fir,
                            &fir_q, sizeof(fir_q));
    }

    return true;
}

/* Destroys + re-initialises the VideoGPUChain in place with the new
 * region carried on `chain->signal_fmt`. Must be called on the GPU
 * thread with no pending command buffers in flight — callers are
 * expected to have already flushed or waited on the device. */
bool video_gpu_rebuild_for_region(VideoGPUChain *vgc, SDL_GPUDevice *gpu,
                                   const VideoChain *chain,
                                   const char *shader_dir,
                                   const float *fir_y_taps, int fir_y_n,
                                   const float *fir_c_taps, int fir_c_n,
                                   const float *fir_q_taps, int fir_q_n)
{
    if (!vgc || !gpu || !chain || !shader_dir) return false;

    /* Preserve render-domain beam output dimensions across the
     * rebuild. These are set by main.c once at startup (from the
     * display window size) and otherwise never touched; without
     * re-establishing them post-init the whole beam / halation /
     * temporal-blit pipeline short-circuits and we fall back to a
     * raw RGB upload — the "too sharp, wrong colors" PAL symptom. */
    int saved_out_w = vgc->beam_out_w;
    int saved_out_h = vgc->beam_out_h;
    int saved_rps   = vgc->beam_rows_per_scanline;
    float saved_sigma_n = vgc->beam_sigma_narrow;
    float saved_sigma_w = vgc->beam_sigma_wide;

    video_gpu_destroy(vgc, gpu);
    if (!video_gpu_init(vgc, gpu, chain, shader_dir,
                        fir_y_taps, fir_y_n,
                        fir_c_taps, fir_c_n,
                        fir_q_taps, fir_q_n))
        return false;

    /* Re-create the beam profile output buffer + storage texture at
     * the same display dimensions we had before. The sigma values
     * get properly overwritten by gpu_cb_update_beam_params moments
     * later; we just need a nonzero pair here so set_beam_params
     * succeeds. */
    if (saved_out_w > 0 && saved_out_h > 0 && saved_rps > 0) {
        video_gpu_set_beam_params(vgc, gpu,
                                  saved_out_w, saved_out_h, saved_rps,
                                  saved_sigma_n > 0.0f ? saved_sigma_n : 0.20f,
                                  saved_sigma_w > 0.0f ? saved_sigma_w : 0.70f);
    }
    return true;
}

void video_gpu_reinit_stages(VideoGPUChain *vgc, const VideoChain *chain)
{
    vgc->chain = chain;
    vgc->sig_chain.first_stage = video_connection_uses_signal_decode(chain->connection)
        ? 0 : vgc->stage_osd;
    int total_samples = vgc->raster_fmt.total_samples;

    /* Stage enables CAN be toggled at runtime — chain_run_cmd ping-pongs
     * based on the currently-enabled stages, not on an init-time routing
     * table. The chroma path resolves its own aux/buffer bindings at
     * dispatch time through the typed rebind hooks, so it doesn't hold
     * stale assumptions about which upstream stages ran.
     *
     * This function still only updates stage PARAMETERS rather than
     * re-adding stages — changing the chain topology (stage order / new
     * stages) would require a chain rebuild. Adjusting which of the
     * existing stages are enabled is safe. */

    vhs_gpu_set_enabled(&vgc->vhs, &vgc->sig_chain, vhs_active(chain));

    /* RF stage: params + enable follow the current connection. */
    if (vgc->stage_rf >= 0) {
        bool rf_on = video_chain_stage_active(chain, 4);
        chain_set_stage_enabled(&vgc->sig_chain, vgc->stage_rf, rf_on);
        if (rf_on) {
            GpuRFParams rf_params = {0};
            rf_params.count            = (uint32_t)total_samples;
            rf_params.samples_per_line = (uint32_t)vgc->raster_fmt.samples_per_line;
            rf_params.noise_amplitude=video_rf_noise_rms(&chain->rf);
            rf_params.hum_amplitude    = fmaxf(0.0f, chain->console_psu_hum);
            chain_update_params(&vgc->sig_chain, vgc->stage_rf,
                                &rf_params, sizeof(rf_params));
        }
    }

    if (vgc->stage_rf_if >= 0) {
        chain_set_stage_enabled(&vgc->sig_chain, vgc->stage_rf_if,
            video_chain_stage_active(chain, 4));
        GpuRFIFParams ip={(uint32_t)total_samples,(uint32_t)vgc->raster_fmt.samples_per_line,SIGNAL_RF_IF_TAPS,
                          chain->rf.detector==2 ? 0u : 1u};
        chain_update_params(&vgc->sig_chain, vgc->stage_rf_if, &ip, sizeof(ip));
    }

    /* AGC: the loop's constants from the preset's attack and release times. */
    if (vgc->stage_agc >= 0 && vgc->stage_agc_loop >= 0) {
        bool agc_on = chain->connection==VIDEO_CONN_RF;
        chain_set_stage_enabled(&vgc->sig_chain, vgc->stage_agc_loop, agc_on);
        chain_set_stage_enabled(&vgc->sig_chain, vgc->stage_agc, agc_on);
        if (agc_on) {
        GpuAGCParams agc_params = agc_params_from(vgc);
        chain_update_params(&vgc->sig_chain, vgc->stage_agc_loop,
                            &agc_params, sizeof(agc_params));
        chain_update_params(&vgc->sig_chain, vgc->stage_agc,
                            &agc_params, sizeof(agc_params));
        }
    }

    /* Ghosting: update delay and level from cable params. */
    if (vgc->stage_ghosting >= 0) {
        chain_set_stage_enabled(&vgc->sig_chain, vgc->stage_ghosting,
            video_chain_stage_active(chain, 3)
            && chain->cable.ghost_level > 0.001f);
        GpuDelayParams ghost_params;
        ghost_params.count         = (uint32_t)total_samples;
        ghost_params.mode          = 1;   /* ghost mode */
        ghost_params.delay_samples = chain->cable.ghost_delay > 0
            ? chain->cable.ghost_delay : 16;
        ghost_params.level         = chain->cable.ghost_level;
        chain_update_params(&vgc->sig_chain, vgc->stage_ghosting,
                            &ghost_params, sizeof(ghost_params));
    }

    /* Comb filter: update both params and enable. S-Video keeps the
     * stage physically absent even if the OSD comb selector changes. */
    if (vgc->stage_comb >= 0) {
        uint32_t shader_mode = (chain->signal_fmt.region==SIGNAL_REGION_NTSC ? video_chain_comb_shader_mode(chain->comb_type) : 0u);
        GpuCombParams comb_params;
        comb_params.delay_samples = chain->signal_fmt.region == SIGNAL_REGION_NTSC ? 2730u : 0u;
        comb_params.count            = (uint32_t)total_samples;
        comb_params.samples_per_line = (uint32_t)vgc->raster_fmt.samples_per_line;
        comb_params.mode             = shader_mode;
        /* Notch-depth override: honour preset's comb_notch_depth if set,
         * otherwise use the per-comb-type default (same logic as init). */
        if (chain->comb_notch_depth > 0.0f)
            comb_params.blend = chain->comb_notch_depth;
        else
            comb_params.blend = (chain->comb_type == VIDEO_COMB_1LINE) ? 0.65f : 0.85f;
        chain_update_params(&vgc->sig_chain, vgc->stage_comb,
                            &comb_params, sizeof(comb_params));
        chain_set_stage_enabled(&vgc->sig_chain, vgc->stage_comb,
            video_chain_stage_active(chain, 6) && shader_mode > 0u);
        chain_set_stage_enabled(&vgc->sig_chain, vgc->stage_comb_bandpass,
            video_chain_stage_active(chain, 6) && shader_mode > 0u);
    }

    /* Luma FIR: don't change enable — always active from init. */
    if (vgc->stage_luma_fir >= 0) {
        /* params already updated via video_gpu_update_fir_taps() */
    }
    if (vgc->stage_luma_peaking >= 0)
        chain_set_stage_enabled(&vgc->sig_chain, vgc->stage_luma_peaking,
            luma_peaking_active(chain));

    if (vgc->stage_chroma_demod >= 0) {
        bool chroma_on = video_chain_stage_active(chain, 7);
        chain_set_stage_enabled(&vgc->sig_chain, vgc->stage_chroma_demod, chroma_on);
    }
    if (vgc->stage_chroma_i_fir >= 0) {
        chain_set_stage_enabled(&vgc->sig_chain, vgc->stage_chroma_i_fir,
            video_chain_stage_active(chain, 7));
    }
    if (vgc->stage_chroma_q_fir >= 0) {
        chain_set_stage_enabled(&vgc->sig_chain, vgc->stage_chroma_q_fir,
            video_chain_stage_active(chain, 7));
    }
    if (vgc->stage_pal_chroma >= 0) {
        chain_set_stage_enabled(&vgc->sig_chain, vgc->stage_pal_chroma,
            video_chain_stage_active(chain, 7));
    }
    if (vgc->stage_matrix >= 0) {
        chain_set_stage_enabled(&vgc->sig_chain, vgc->stage_matrix,
            video_chain_stage_active(chain, 9));
    }

    update_video_amp(vgc);

    LOGV("video_gpu_reinit_stages: rf=%d comb=%d chroma=%d matrix=%d vamp=%d\n",
            vgc->stage_rf >= 0 ? vgc->sig_chain.stages[vgc->stage_rf].enabled : -1,
            vgc->stage_comb >= 0 ? vgc->sig_chain.stages[vgc->stage_comb].enabled : -1,
            vgc->stage_chroma_demod >= 0 ? vgc->sig_chain.stages[vgc->stage_chroma_demod].enabled : -1,
            vgc->stage_matrix >= 0 ? vgc->sig_chain.stages[vgc->stage_matrix].enabled : -1,
            vgc->vamp_enabled);
}

void video_gpu_set_demod(VideoGPUChain *vgc, float phase, float dp)
{
    vgc->demod_phase = phase;
    vgc->demod_dp    = dp;

    /* Update the demod stage uniform params. */
    GpuModulatorParams demod_params;
    memset(&demod_params, 0, sizeof(demod_params));
    demod_params.count            = (uint32_t)vgc->raster_fmt.total_samples;
    demod_params.mode             = 3;
    demod_params.burst_reference  = demod_burst_reference(vgc);
    demod_params.phase            = phase;
    demod_params.dp               = dp;
    demod_params.param_a          = 1.0f;
    demod_params.samples_per_line = (uint32_t)vgc->raster_fmt.samples_per_line;
    demod_params.line_phase_inc   = vgc->demod_line_phase;
    chain_update_params(&vgc->sig_chain, vgc->stage_chroma_demod,
                        &demod_params, sizeof(demod_params));
}

void video_gpu_set_dynamic_state(VideoGPUChain *vgc,
                                 float frame_brightness,
                                 float apl_smoothed,
                                 float audio_bass_rms)
{
    if (!vgc) return;
    vgc->frame_brightness = frame_brightness;
    vgc->apl_smoothed = apl_smoothed;
    vgc->audio_bass_rms = audio_bass_rms;
}

/* ============================================================================
 * Forward declarations
 * ============================================================================ */

static bool dispatch_beam_profile(VideoGPUChain *vgc, SDL_GPUCommandBuffer *cmd);

/* ============================================================================
 * Processing — manual chroma dispatch helper
 * ============================================================================
 *
 * The chain runner handles the luma path (RC filters + luma FIR) and the
 * chroma demod (which writes I/Q to aux[0]/aux[1]). But the chroma FIR
 * stages need to read from aux buffers, which the chain_run ping-pong
 * model doesn't support. So we dispatch them manually here.
 *
 * This dispatches: FIR(aux[src_idx]) -> aux[dst_idx]
 */
static bool dispatch_aux_fir_cmd(VideoGPUChain *vgc, SDL_GPUCommandBuffer *cmd,
                                  int stage_idx, int src_aux, int dst_aux)
{
    SignalChain *sc = &vgc->sig_chain;
    ChainStage *s = &sc->stages[stage_idx];

    if (!sc->pipeline_loaded[CHAIN_KERNEL_FIR]) {
        fprintf(stderr, "dispatch_aux_fir_cmd: FIR pipeline not loaded\n");
        return false;
    }

    /* FIR: 2 readonly (input + taps) + 1 readwrite (output) + 1 uniform. */
    SDL_GPUStorageBufferReadWriteBinding rw_bindings[1] = {0};
    rw_bindings[0].buffer = sc->aux[dst_aux];

    SDL_GPUComputePass *pass = SDL_BeginGPUComputePass(
        cmd, NULL, 0, rw_bindings, 1);
    if (!pass) return false;

    SDL_BindGPUComputePipeline(pass, sc->pipelines[CHAIN_KERNEL_FIR].pipeline);

    /* Readonly: input (aux[src]) + taps. */
    SDL_GPUBuffer *ro[] = {
        sc->aux[src_aux],
        (s->taps_index < sc->num_tap_bufs) ? sc->tap_bufs[s->taps_index] : sc->aux[src_aux]
    };
    SDL_BindGPUComputeStorageBuffers(pass, 0, ro, 2);

    /* Push uniform parameters. */
    if (s->params_size > 0) {
        SDL_PushGPUComputeUniformData(cmd, 0, s->params, s->params_size);
    }

    SDL_DispatchGPUCompute(pass, s->dispatch_x, s->dispatch_y, s->dispatch_z);
    SDL_EndGPUComputePass(pass);

    return true;
}

/* Public trampoline for chroma_pipeline.c. */
bool dispatch_aux_fir_cmd_public(VideoGPUChain *vgc,
                                  SDL_GPUCommandBuffer *cmd,
                                  int stage_idx,
                                  int src_aux_idx,
                                  int dst_aux_idx) {
    return dispatch_aux_fir_cmd(vgc, cmd, stage_idx, src_aux_idx, dst_aux_idx);
}

/* Public trampolines for post_pipeline.c — so the post-signal stages
 * (video amp, h-blur, deflection, beam, temporal blit) can run from outside this
 * TU without exposing the static dispatch functions. */
bool dispatch_video_amp_public(VideoGPUChain *vgc, SDL_GPUCommandBuffer *cmd) {
    return dispatch_video_amp(vgc, cmd);
}
bool dispatch_h_blur_rgb_public(VideoGPUChain *vgc, SDL_GPUCommandBuffer *cmd) {
    return dispatch_h_blur_rgb(vgc, cmd);
}
bool dispatch_gun_current_public(VideoGPUChain *v, SDL_GPUCommandBuffer *cmd) {
    const TVDisplayParams *tv=&v->chain->tv;
    float gamma=tv->gamma>0 ? tv->gamma : 2.4f;
    float pickup=(1.0f-v->chain->cable.shield_effectiveness)*v->chain->cable.length_meters*0.005f;
    const DecodeWindow *w=&v->window;
    /* Noise is seeded by picture sample and line, so the border extends the
     * picture's pattern rather than moving it. */
    struct { uint32_t count; float r,g,b; uint32_t width,seed; float noise,spp,black_floor,apl_bias; int32_t picture_x,picture_row;
             float trace[4]; }
        p={(uint32_t)decode_window_samples(w), gamma+tv->phosphor_gamma_offset_r,
           gamma+tv->phosphor_gamma_offset_g,gamma+tv->phosphor_gamma_offset_b,
           (uint32_t)w->width,v->beam_frame_counter,
           tv->noise_level+pickup,(float)w->spp,tv->black_floor,
           tv->apl_black_lift*(v->apl_smoothed-0.5f)*0.15f,w->picture_x,w->picture_row,{0}};
    video_gpu_scanned_trace(v,p.trace);
    GpuDispatchDesc d={.pipeline=&v->sig_chain.pipelines[CHAIN_KERNEL_GUN_CURRENT],
        .readonly_buffers={v->buf_rgb},.num_readonly_buffers=1,
        .readwrite_buffers={v->buf_gun_current},.num_readwrite_buffers=1,
        .uniforms={{&p,sizeof(p)}},.num_uniforms=1,
        .groupcount_x=(p.count+255)/256,.groupcount_y=1,.groupcount_z=1};
    return gpu_dispatch(cmd,&d);
}
bool dispatch_beam_profile_public(VideoGPUChain *vgc, SDL_GPUCommandBuffer *cmd) {
    return dispatch_beam_profile(vgc, cmd);
}
bool dispatch_temporal_blit_public(VideoGPUChain *vgc, SDL_GPUCommandBuffer *cmd) {
    return dispatch_temporal_blit(vgc, cmd);
}

/* ============================================================================
 * Processing
 * ============================================================================ */

/*
 * GPU video pipeline:
 *
 *   1. Upload waveform to chain buf[0]
 *   2. chain_run: dispatches the entire enabled signal chain
 *      - composite/RF/S-Video share the same core path
 *      - PAL inserts its decoder-correction stage before matrix decode
 *   3. If rgb_out requested, download buf_rgb to CPU
 */

static void update_demod_params(VideoGPUChain *vgc) {
    GpuRasterParams raster = {
        .count = (uint32_t)vgc->raster_fmt.total_samples,
        .full_width = (uint32_t)vgc->raster_fmt.samples_per_line,
        .active_width = (uint32_t)vgc->signal_fmt.samples_per_line,
        .samples_per_dot = (uint32_t)vgc->signal_fmt.samples_per_pixel,
        .sync_level = vgc->raster_sync_level,
        .burst_amp = vgc->raster_burst_amp,
        .burst_sine = vgc->raster_burst_sine ? 1u : 0u,
        .phase_base = (float)vgc->signal_phase_base,
        .line_phase = (float)vgc->signal_line_phase,
        .region = (uint32_t)vgc->signal_fmt.region,
        .lines = (uint32_t)vgc->raster_fmt.lines,
        .separate_yc = vgc->source_separated ? 1u : 0u
    };
    memcpy(raster.backdrop, vgc->backdrop, sizeof(raster.backdrop));
    memcpy(raster.gray_backdrop, vgc->gray_backdrop, sizeof(raster.gray_backdrop));
    chain_update_params(&vgc->sig_chain, vgc->stage_raster, &raster, sizeof(raster));
    int source[2]={vgc->stage_console_lp,vgc->stage_cable_rc};
    int dest[2]={vgc->stage_y_console,vgc->stage_y_cable};
    for(int i=0;i<2;i++) {
        ChainStage *s=&vgc->sig_chain.stages[source[i]];
        chain_update_params(&vgc->sig_chain,dest[i],s->params,s->params_size);
        chain_set_stage_enabled(&vgc->sig_chain,dest[i],vgc->source_separated && s->enabled && !s->bypass);
    }
    ChainStage *ghost = &vgc->sig_chain.stages[vgc->stage_ghosting];
    bool reflect_y = vgc->source_separated && ghost->enabled && !ghost->bypass;
    chain_update_params(&vgc->sig_chain, vgc->stage_y_ghost, ghost->params, ghost->params_size);
    chain_set_stage_enabled(&vgc->sig_chain, vgc->stage_y_ghost, reflect_y);
    vgc->sig_chain.stages[vgc->stage_yc_route].ro[1] = reflect_y ? CBR_AUX3 : CBR_AUX2;
    chain_set_stage_enabled(&vgc->sig_chain,vgc->stage_yc_route,vgc->source_separated);
    GpuModulatorParams p = {0};
    p.count = (uint32_t)vgc->raster_fmt.total_samples;
    p.mode = 3;
    p.phase = vgc->demod_phase - vgc->signal_phase_base * 2.0f * (float)M_PI / 12.0f;
    p.dp = vgc->demod_dp;
    p.param_a = 1;
    p.samples_per_line = (uint32_t)vgc->raster_fmt.samples_per_line;
    p.line_phase_inc = (float)(SIGNAL_PICTURE_DOT * vgc->signal_fmt.samples_per_pixel); /* the picture's first sample */
    p.burst_reference = demod_burst_reference(vgc);
    chain_update_params(&vgc->sig_chain, vgc->stage_chroma_demod, &p, sizeof(p));
}

static void update_signal_time(VideoGPUChain *vgc) {
    if (vgc->stage_rf >= 0) {
        GpuRFParams *p = (GpuRFParams *)vgc->sig_chain.stages[vgc->stage_rf].params;
        p->frame_seed = vgc->signal_frame_counter;
        p->sample_rate = signal_format_sample_rate_hz(&vgc->signal_fmt);
        p->full_line_samples = (uint32_t)signal_format_full_line(&vgc->signal_fmt);
        p->hum_hz = vgc->signal_fmt.region == SIGNAL_REGION_PAL ? 50.0f : 60.0f;
        double t = (double)vgc->signal_frame_counter * signal_region_frame_ms(vgc->signal_fmt.region) / 1000.0;
        p->hum_phase = (float)(fmod(t * p->hum_hz, 1.0) * 2.0 * M_PI);
    }
    vgc->signal_frame_counter++;
}

bool video_gpu_process(VideoGPUChain *vgc, SDL_GPUDevice *gpu,
                       const float *waveform, float *rgb_out)
{
    vgc->source_separated=false; // Externally supplied waveform is composite.
    update_signal_time(vgc);
    const SignalFormat *fmt = &vgc->signal_fmt;
    int total_samples = fmt->total_samples;
    Uint32 upload_bytes = (Uint32)(total_samples * sizeof(float));

    if (upload_bytes > vgc->sig_chain.buf_size) {
        fprintf(stderr, "video_gpu_process: waveform too large (%u > %u)\n",
                upload_bytes, vgc->sig_chain.buf_size);
        return false;
    }

    update_demod_params(vgc);

    /* ---- ALL GPU work in ONE command buffer, ONE fence ---- */
    SDL_GPUCommandBuffer *master_cmd = SDL_AcquireGPUCommandBuffer(gpu);
    if (!master_cmd) return false;

    /* ---- 1. Upload waveform (copy pass in shared cmd) ---- */
    if (!chain_upload_input_cmd(&vgc->sig_chain, gpu, master_cmd, waveform, upload_bytes) ||
        !vhs_gpu_frame(&vgc->vhs, &vgc->sig_chain, gpu, master_cmd, vgc->signal_frame_counter - 1,
                       -vgc->raster_sync_level)) {
        SDL_SubmitGPUCommandBuffer(master_cmd);
        fprintf(stderr, "video_gpu_process: waveform upload failed\n");
        return false;
    }

    /* ---- 2. Run the chain (compute passes in shared cmd) ---- */
    if (!chain_run_cmd(&vgc->sig_chain, master_cmd)) {
        vgc->deflection_cache_valid = false;
        SDL_SubmitGPUCommandBuffer(master_cmd);
        fprintf(stderr, "video_gpu_process: chain_run failed\n");
        return false;
    }

    /* Matrix decode and the RGB-domain post stages are registered in the
     * chain and have already run by this point. No manual chroma block is
     * left here anymore. */

    /* Submit compute work. If rgb_out is needed (CPU download), wait for
     * the fence. Otherwise fire-and-forget — the CRT render pass will
     * read from tex_beam after the compute finishes (GPU-side dependency). */
    if (rgb_out && vgc->buf_rgb) {
        SDL_GPUFence *fence = SDL_SubmitGPUCommandBufferAndAcquireFence(master_cmd);
        bool ready = fence && SDL_WaitForGPUFences(gpu, true, &fence, 1);
        if (fence) {
            SDL_ReleaseGPUFence(gpu, fence);
        }
        if (!ready) { vgc->deflection_cache_valid = false; return false; }
    } else {
        if (!SDL_SubmitGPUCommandBuffer(master_cmd)) {
            vgc->deflection_cache_valid = false;
            return false;
        }
    }

    /* ---- 8. Download RGB to CPU if requested ---- */
    if (rgb_out && vgc->buf_rgb) {
        if (!gpu_buffer_download(gpu, vgc->buf_rgb, rgb_out, vgc->rgb_size)) {
            fprintf(stderr, "video_gpu_process: RGB download failed\n");
            return false;
        }
    }

    return true;
}

/* ============================================================================
 * Beam profile configuration
 * ============================================================================ */

bool video_gpu_set_beam_params(VideoGPUChain *vgc, SDL_GPUDevice *gpu,
                               int out_w, int out_h,
                               int rows_per_scanline,
                               float sigma_narrow, float sigma_wide)
{
    if (out_w <= 0 || out_h <= 0 || rows_per_scanline <= 0) return false;

    Uint32 deflect_needed = (Uint32)(out_w * out_h * 4 * sizeof(float));
    if (deflect_needed != vgc->deflection_size) {
        vgc->deflection_cache_valid=false;
        if (vgc->buf_deflection_x) {
            SDL_ReleaseGPUBuffer(gpu, vgc->buf_deflection_x);
            vgc->buf_deflection_x = NULL;
        }
        if (vgc->buf_deflection_y) {
            SDL_ReleaseGPUBuffer(gpu, vgc->buf_deflection_y);
            vgc->buf_deflection_y = NULL;
        }
        vgc->buf_deflection_x = gpu_buffer_create(gpu, deflect_needed, GPU_BUF_READWRITE);
        vgc->buf_deflection_y = gpu_buffer_create(gpu, deflect_needed, GPU_BUF_READWRITE);
        if (!vgc->buf_deflection_x || !vgc->buf_deflection_y) {
            fprintf(stderr,
                    "video_gpu_set_beam_params: failed to create deflection buffers (%u bytes each)\n",
                    deflect_needed);
            return false;
        }
        vgc->deflection_size = deflect_needed;
    }

    // A second decay state costs memory/bandwidth only when a tail is enabled.
    Uint32 history_needed=(Uint32)(out_w*out_h*8)*(vgc->tail_weight>0 ? 2u : 1u);
    if (history_needed!=vgc->phosphor_history_size) {
        if (vgc->buf_phosphor_history) SDL_ReleaseGPUBuffer(gpu,vgc->buf_phosphor_history);
        vgc->buf_phosphor_history=gpu_buffer_create(gpu,history_needed,GPU_BUF_READWRITE);
        if (!vgc->buf_phosphor_history) { vgc->phosphor_history_size=0; return false; }
        vgc->phosphor_history_size=history_needed;
        vgc->temporal_history_valid=false;
    }
    /* Reallocate RGBA16F output buffer if size changed (8 bytes per pixel). */
    Uint32 needed = (Uint32)(out_w * out_h * 8);  /* 4x float16 = 8 bytes/pixel */
    if (needed != vgc->beam_rgba_size || out_w != vgc->beam_out_w || out_h != vgc->beam_out_h) {
        if (vgc->buf_beam_rgba) {
            SDL_ReleaseGPUBuffer(gpu, vgc->buf_beam_rgba);
            vgc->buf_beam_rgba = NULL;
        }
        vgc->buf_beam_rgba = gpu_buffer_create(gpu, needed, GPU_BUF_READWRITE);
        if (!vgc->buf_beam_rgba) {
            fprintf(stderr, "video_gpu_set_beam_params: failed to create RGBA buffer (%u bytes)\n", needed);
            return false;
        }
        vgc->beam_rgba_size = needed;

        /* Previous frame buffer for temporal blend (same size). */
        if (vgc->buf_beam_prev) {
            SDL_ReleaseGPUBuffer(gpu, vgc->buf_beam_prev);
            vgc->buf_beam_prev = NULL;
        }
        vgc->buf_beam_prev = gpu_buffer_create(gpu, needed, GPU_BUF_READWRITE);
        vgc->temporal_history_valid = false;
        vgc->smoothing_history_valid = false;
        if (!vgc->buf_beam_prev) return false;

        /* Storage texture: compute writes here, CRT shader samples it.
         * Zero-copy path — no CPU download/upload needed. */
        if (vgc->tex_beam) {
            SDL_ReleaseGPUTexture(gpu, vgc->tex_beam);
            vgc->tex_beam = NULL;
        }
        SDL_GPUTextureCreateInfo tci = {0};
        tci.type = SDL_GPU_TEXTURETYPE_2D;
        tci.format = SDL_GPU_TEXTUREFORMAT_R16G16B16A16_FLOAT;
        tci.width = out_w;
        tci.height = out_h;
        tci.layer_count_or_depth = 1;
        tci.num_levels = 1;
        tci.usage = SDL_GPU_TEXTUREUSAGE_COMPUTE_STORAGE_WRITE
                  | SDL_GPU_TEXTUREUSAGE_SAMPLER;
        vgc->tex_beam = SDL_CreateGPUTexture(gpu, &tci);
        if (vgc->tex_beam) {
            LOGV("Beam storage texture: %dx%d RGBA16F (zero-copy)\n", out_w, out_h);
        }
    }

    vgc->beam_out_w = out_w;
    vgc->beam_out_h = out_h;
    vgc->beam_rows_per_scanline = rows_per_scanline;
    vgc->beam_sigma_narrow = sigma_narrow;
    vgc->beam_sigma_wide = sigma_wide;
    /* beam_h_blur_sigma is set by the preset via gpu_cb_update_beam_params;
     * left at its current value here. If still zero after init (first call,
     * before any preset apply), seed a safe non-zero fallback so the blur
     * kernel radius is positive. */
    if (vgc->beam_h_blur_sigma <= 0.0f) vgc->beam_h_blur_sigma = 1.0f;

    post_pipeline_install_deflection_typed(vgc);
    if (vgc->stage_deflection >= 0) {
        chain_set_stage_enabled(&vgc->sig_chain, vgc->stage_deflection, true);
    }
    if (vgc->stage_beam_output >= 0) {
        chain_set_stage_enabled(&vgc->sig_chain, vgc->stage_beam_output,
                                vgc->buf_beam_rgba != NULL);
    }

    return true;
}

bool video_gpu_get_beam_size(const VideoGPUChain *vgc, int *out_w, int *out_h)
{
    if (!vgc->buf_beam_rgba) return false;
    if (out_w) *out_w = vgc->beam_out_w;
    if (out_h) *out_h = vgc->beam_out_h;
    return true;
}

SDL_GPUTexture *video_gpu_get_beam_texture(const VideoGPUChain *vgc)
{
    return vgc->tex_beam;
}

/* Dispatch temporal_blit.comp: blend cur+prev beam buffers → storage texture.
 * Also copies cur→prev for next frame. All GPU-side, no CPU roundtrip. */
static bool dispatch_temporal_blit(VideoGPUChain *vgc, SDL_GPUCommandBuffer *cmd)
{
    if (!vgc->buf_beam_rgba || !vgc->tex_beam || !vgc->buf_phosphor_history) return false;
    if (!vgc->sig_chain.pipeline_loaded[CHAIN_KERNEL_TEMPORAL_BLIT]) return false;

    struct {
        uint32_t width;
        uint32_t height;
        float    blend_factor;
        float    blend_r;
        float    blend_g;
        float    blend_b;
        float    motion_threshold;
        uint32_t history_valid;
        float tail_r, tail_g, tail_b, tail_weight;
    } params;
    params.width = (uint32_t)vgc->beam_out_w;
    params.height = (uint32_t)vgc->beam_out_h;
    /* Display smoothing requires a valid previous raw frame. */
    params.blend_factor = (vgc->buf_beam_prev && vgc->smoothing_history_valid && vgc->elapsed_frames <= 1)
        ? vgc->temporal_blend : 0.0f;
    /* Zero disables decay; never read uninitialised history. */
    params.blend_r = vgc->temporal_history_valid ? powf(vgc->blend_r, fmaxf(1, vgc->elapsed_frames)) : 0.0f;
    params.blend_g = vgc->temporal_history_valid ? powf(vgc->blend_g, fmaxf(1, vgc->elapsed_frames)) : 0.0f;
    params.blend_b = vgc->temporal_history_valid ? powf(vgc->blend_b, fmaxf(1, vgc->elapsed_frames)) : 0.0f;
    /* Optional display-domain smoothing, not receiver comb filtering. */
    params.motion_threshold = vgc->motion_threshold > 0.0f
        ? vgc->motion_threshold : 0.08f;

    params.history_valid = vgc->temporal_history_valid ? 1u : 0u;
    params.tail_r=powf(vgc->tail_r,fmaxf(1,vgc->elapsed_frames));
    params.tail_g=powf(vgc->tail_g,fmaxf(1,vgc->elapsed_frames));
    params.tail_b=powf(vgc->tail_b,fmaxf(1,vgc->elapsed_frames));
    params.tail_weight=vgc->phosphor_history_size>=2*vgc->beam_rgba_size ? vgc->tail_weight : 0;
    /* History is read/written only by its own pixel, so no cross-thread race. */
    SDL_GPUStorageTextureReadWriteBinding tex_rw[1] = {0};
    tex_rw[0].texture = vgc->tex_beam;

    SDL_GPUStorageBufferReadWriteBinding history_rw = {0};
    history_rw.buffer = vgc->buf_phosphor_history;
    SDL_GPUComputePass *pass = SDL_BeginGPUComputePass(
        cmd, tex_rw, 1, &history_rw, 1);
    if (!pass) return false;

    SDL_BindGPUComputePipeline(pass,
        vgc->sig_chain.pipelines[CHAIN_KERNEL_TEMPORAL_BLIT].pipeline);

    SDL_GPUBuffer *ro[] = {
        vgc->buf_beam_rgba,
        vgc->buf_beam_prev ? vgc->buf_beam_prev : vgc->buf_beam_rgba
    };
    SDL_BindGPUComputeStorageBuffers(pass, 0, ro, 2);

    SDL_PushGPUComputeUniformData(cmd, 0, &params, sizeof(params));

    uint32_t gx = (params.width + 15) / 16;
    uint32_t gy = (params.height + 15) / 16;
    SDL_DispatchGPUCompute(pass, gx, gy, 1);
    SDL_EndGPUComputePass(pass);

    /* Copy cur → prev for next frame (buffer-to-buffer, same cmd). */
    vgc->smoothing_history_valid = false;
    if (vgc->buf_beam_prev && vgc->temporal_blend > 0.0f) {
        SDL_GPUCopyPass *copy = SDL_BeginGPUCopyPass(cmd);
        if (copy) {
            SDL_GPUBufferLocation src = { .buffer = vgc->buf_beam_rgba, .offset = 0 };
            SDL_GPUBufferLocation dst = { .buffer = vgc->buf_beam_prev, .offset = 0 };
            SDL_CopyGPUBufferToBuffer(copy, &src, &dst, vgc->beam_rgba_size, false);
            vgc->smoothing_history_valid = true;
            SDL_EndGPUCopyPass(copy);
        }
    }

    vgc->temporal_history_valid = true;
    return true;
}

/* Dispatch video_amp.comp: per-channel RGB FIR lowpass.
 * Reads from buf_rgb, writes to buf_rgb2, then swaps so buf_rgb
 * always points to the latest result for downstream consumers. */
static bool dispatch_video_amp(VideoGPUChain *vgc, SDL_GPUCommandBuffer *cmd)
{
    if (!vgc->vamp_enabled || !vgc->buf_rgb || !vgc->buf_rgb2) return false;
    if (!vgc->sig_chain.pipeline_loaded[CHAIN_KERNEL_VIDEO_AMP]) return false;

    const DecodeWindow *w = &vgc->window;
    int total_pixels = (int)decode_window_samples(w);

    /* Uniform params matching video_amp.comp.glsl layout. */
    /* std140 layout: float array elements are padded to 16 bytes each.
     * taps[0] at offset 16, taps[1] at offset 32, etc. */
    struct {
        uint32_t total_pixels;
        uint32_t samples_per_line;
        uint32_t tap_count;
        uint32_t _pad0;           /* pad to 16-byte boundary before array */
        float    taps[32][4];     /* .rgb: independent gun filters */
        float    velocity_mod;
        float    asym_rise_fall;
        float    vertical_smear;
        float    _pad1;
        float    trace[4];        /* the unblanked raster (decode_window.glsl) */
    } amp_params;

    memset(&amp_params, 0, sizeof(amp_params));
    amp_params.total_pixels    = (uint32_t)total_pixels;
    amp_params.samples_per_line = (uint32_t)w->width;
    amp_params.tap_count       = (uint32_t)vgc->vamp_tap_count;
    memcpy(amp_params.taps, vgc->vamp_taps, sizeof(amp_params.taps));
    const TVDisplayParams *tv = vgc->chain ? &vgc->chain->tv : NULL;
    amp_params.velocity_mod    = tv ? tv->velocity_mod   : 0.0f;
    amp_params.asym_rise_fall  = tv ? tv->asym_rise_fall : 0.0f;
    amp_params.vertical_smear  = tv ? tv->vertical_smear : 0.0f;
    video_gpu_scanned_trace(vgc, amp_params.trace);

    SDL_GPUStorageBufferReadWriteBinding rw[1] = {0};
    rw[0].buffer = vgc->buf_rgb2;  /* output */

    SDL_GPUComputePass *pass = SDL_BeginGPUComputePass(cmd, NULL, 0, rw, 1);
    if (!pass) return false;

    SDL_BindGPUComputePipeline(pass,
        vgc->sig_chain.pipelines[CHAIN_KERNEL_VIDEO_AMP].pipeline);

    /* Readonly: RGB input from matrix decode. */
    SDL_GPUBuffer *ro[] = { vgc->buf_rgb };
    SDL_BindGPUComputeStorageBuffers(pass, 0, ro, 1);

    SDL_PushGPUComputeUniformData(cmd, 0, &amp_params, sizeof(amp_params));

    int dispatch_x = (total_pixels + 255) / 256;
    SDL_DispatchGPUCompute(pass, dispatch_x, 1, 1);
    SDL_EndGPUComputePass(pass);

    /* Swap buffers so buf_rgb always has the latest result. */
    SDL_GPUBuffer *tmp = vgc->buf_rgb;
    vgc->buf_rgb = vgc->buf_rgb2;
    vgc->buf_rgb2 = tmp;

    return true;
}

/* Horizontal spot spread in linear emitted current, at signal resolution. */
static bool dispatch_h_blur_rgb(VideoGPUChain *vgc, SDL_GPUCommandBuffer *cmd)
{
    if (!vgc->buf_rgb || !vgc->buf_rgb2) return false;
    if (!vgc->sig_chain.pipeline_loaded[CHAIN_KERNEL_H_BLUR_RGB]) return false;

    struct {
        uint32_t signal_w, num_lines, radius;
        float growth;
        float weights[36], wide_weights[36], over_weights[36]; /* std140 vec4[9], symmetric halves */
    } params = {0};
    params.signal_w = (uint32_t)vgc->window.width;
    params.num_lines = (uint32_t)vgc->window.lines;
    float sigma=fmaxf(vgc->beam_h_blur_sigma,0.5f);
    params.growth=vgc->chain ? fmaxf(vgc->chain->tv.beam_spot_growth,0) : 0;
    float wide=sigma*(1+params.growth);
    float over=sigma*(1+params.growth*4);
    params.radius=(uint32_t)fminf(ceilf(3*over),32);
    for(int k=0;k<3;k++) {
        float *w=k==2 ? params.over_weights : k ? params.wide_weights : params.weights;
        float s=k==2 ? over : k ? wide : sigma, sum=1; w[0]=1;
        for(uint32_t i=1;i<=params.radius;i++) {
            w[i]=expf(-(float)(i*i)/(2*s*s)); sum+=2*w[i];
        }
        for(uint32_t i=0;i<=params.radius;i++) w[i]/=sum;
    }

    /* Both buffers as readwrite (same as pointwise pattern). */
    SDL_GPUStorageBufferReadWriteBinding rw_bindings[2] = {0};
    rw_bindings[0].buffer = vgc->buf_gun_current;
    rw_bindings[1].buffer = vgc->buf_rgb2;

    SDL_GPUComputePass *pass = SDL_BeginGPUComputePass(
        cmd, NULL, 0, rw_bindings, 2);
    if (!pass) return false;

    SDL_BindGPUComputePipeline(pass,
        vgc->sig_chain.pipelines[CHAIN_KERNEL_H_BLUR_RGB].pipeline);

    SDL_PushGPUComputeUniformData(cmd, 0, &params, sizeof(params));

    /* 1D dispatch per scanline: X = signal samples, Y = scanlines. */
    uint32_t groups_x = (params.signal_w + 255) / 256;
    SDL_DispatchGPUCompute(pass, groups_x, params.num_lines, 1);
    SDL_EndGPUComputePass(pass);

    return true;
}

/* Dispatch beam_profile.comp: RGB float -> RGBA16F with scanline structure.
 * Reads from buf_rgb2 + the precomputed landing maps, writes to
 * buf_beam_rgba. */
static bool dispatch_beam_profile(VideoGPUChain *vgc, SDL_GPUCommandBuffer *cmd)
{
    if (!vgc->buf_beam_rgba || !vgc->buf_rgb2 ||
        !vgc->buf_deflection_x || !vgc->buf_deflection_y) return false;
    if (!vgc->sig_chain.pipeline_loaded[CHAIN_KERNEL_BEAM]) return false;

    struct {
        uint32_t width;             /* decode window samples per row */
        uint32_t out_w;
        uint32_t out_h;
        uint32_t rows_per_scanline;
        float    sigma_narrow;
        float    sigma_wide;
        uint32_t frame_counter;
        float    hum_bar_amplitude;
        float    bloom_gamma;
        float gamma, gamma_r, gamma_g, gamma_b;
        uint32_t monitor_model, lines;
        int32_t picture_x, picture_row;
        uint32_t picture_w, picture_h;
        float lines_per_row;        /* raster lines an output row spans at the face's centre */
    } beam_params;

    const TVDisplayParams *tv = vgc->chain ? &vgc->chain->tv : NULL;
    const DecodeWindow *w = &vgc->window;
    beam_params.monitor_model = tv && tv->monitor_model==1;
    beam_params.lines = (uint32_t)w->lines;
    beam_params.picture_x = w->picture_x;
    beam_params.picture_row = w->picture_row;
    beam_params.picture_w = (uint32_t)w->picture_w;
    beam_params.picture_h = (uint32_t)w->picture_h;
    /* The face shows the active field's lines, enlarged by the overscan and
     * the vertical size as the deflection map does (post_pipeline.c). */
    float zoom = tv && tv->overscan > 0.001f ? fmaxf(1.0f - 2.0f * tv->overscan, 0.2f) : 1.0f;
    float v_size = tv && tv->v_size > 0.01f ? tv->v_size : 1.0f;
    beam_params.lines_per_row = w->active_lines * zoom / v_size / (float)vgc->beam_out_h;
    beam_params.gamma = tv && tv->gamma > 0 ? tv->gamma : 2.2f;
    beam_params.gamma_r = tv ? tv->phosphor_gamma_offset_r : 0;
    beam_params.gamma_g = tv ? tv->phosphor_gamma_offset_g : 0;
    beam_params.gamma_b = tv ? tv->phosphor_gamma_offset_b : 0;
    beam_params.width             = (uint32_t)w->width;
    beam_params.out_w             = (uint32_t)vgc->beam_out_w;
    beam_params.out_h             = (uint32_t)vgc->beam_out_h;
    beam_params.rows_per_scanline = (uint32_t)vgc->beam_rows_per_scanline;
    beam_params.sigma_narrow      = vgc->beam_sigma_narrow;
    beam_params.sigma_wide        = vgc->beam_sigma_wide;
    /* Cable shield damage → three EMI phenomena scale with (1-shield)×length:
     *   1. Broadband noise (grain in dark areas)
     *   2. 60 Hz mains hum pickup → adds to hum bar (rolling brightness band)
     *   3. RF interference (nearby electronics: CB, AM, switching supplies)
     * At shield=0 (no shield) + 10m cable: +0.04 noise, +0.13 hum, +0.8 RF. */
    float shield_hum = 0.0f;
    if (vgc->chain) {
        float poor = 1.0f - vgc->chain->cable.shield_effectiveness;
        float len = vgc->chain->cable.length_meters;
        shield_hum   = poor * len * 0.013f;
    }
    beam_params.frame_counter     = vgc->beam_frame_counter++;
    /* PSU hum from the NES + cable shield EMI pickup both drive the hum bar. */
    float psu_contribution = vgc->chain ? vgc->chain->console_psu_hum * 5.0f : 0.0f;
    beam_params.hum_bar_amplitude = (tv ? tv->hum_bar_amplitude : 0.0f)
                                    + psu_contribution + shield_hum;
    beam_params.bloom_gamma       = tv ? (tv->bloom_gamma > 0.0f ? tv->bloom_gamma : 1.5f) : 1.5f;

    /* Diagnostic: print beam params every 300 frames. */
    if (vgc->beam_frame_counter % 300 == 1) {
        LOGV("BEAM: sigma=[%.2f %.2f] hum=%.3f\n",
             beam_params.sigma_narrow, beam_params.sigma_wide,
             beam_params.hum_bar_amplitude);
    }

    /* beam_profile: RGB input + 2 landing maps -> RGBA16F output. */
    SDL_GPUStorageBufferReadWriteBinding rw_bindings[1] = {0};
    rw_bindings[0].buffer = vgc->buf_beam_rgba;

    SDL_GPUComputePass *pass = SDL_BeginGPUComputePass(
        cmd, NULL, 0, rw_bindings, 1);
    if (!pass) return false;

    SDL_BindGPUComputePipeline(pass,
        vgc->sig_chain.pipelines[CHAIN_KERNEL_BEAM].pipeline);

    SDL_GPUBuffer *ro[] = {
        beam_params.monitor_model ? vgc->buf_rgb : vgc->buf_rgb2,
        vgc->buf_deflection_x,
        vgc->buf_deflection_y
    };
    SDL_BindGPUComputeStorageBuffers(pass, 0, ro, 3);

    SDL_PushGPUComputeUniformData(cmd, 0, &beam_params, sizeof(beam_params));

    /* 2D dispatch: 16x16 workgroups. */
    uint32_t groups_x = ((uint32_t)vgc->beam_out_w + 15) / 16;
    uint32_t groups_y = ((uint32_t)vgc->beam_out_h + 15) / 16;
    SDL_DispatchGPUCompute(pass, groups_x, groups_y, 1);
    SDL_EndGPUComputePass(pass);

    return true;
}

bool video_gpu_download_beam(VideoGPUChain *vgc, SDL_GPUDevice *gpu,
                             uint8_t *rgba_out)
{
    if (!vgc->buf_beam_rgba || !rgba_out) return false;
    Uint32 bytes = (Uint32)(vgc->beam_out_w * vgc->beam_out_h * 8);  /* float16x4 */
    return gpu_buffer_download(gpu, vgc->buf_beam_rgba, rgba_out, bytes);
}

/* ============================================================================
 * Query
 * ============================================================================ */

SDL_GPUBuffer *video_gpu_get_rgb_buffer(const VideoGPUChain *vgc)
{
    return vgc->buf_rgb;
}

/* ============================================================================
 * Per-preset temporal state reset
 * ============================================================================ */

void video_gpu_reset_temporal_state(VideoGPUChain *vgc, SDL_GPUDevice *gpu)
{
    if (!vgc || !gpu) return;

    if (vgc->buf_receiver) {
        float zeros[314 * 4] = {0};
        gpu_buffer_upload(gpu, vgc->buf_receiver, zeros,
                          (Uint32)(vgc->raster_fmt.lines + 2) * 4 * sizeof(float));
    }

    if (vgc->buf_crt_load) {
        size_t bytes = ((size_t)vgc->window.dots * vgc->window.lines + vgc->window.lines + 1) * sizeof(float);
        void *zeros = calloc(1, bytes);
        if (zeros) {
            gpu_buffer_upload(gpu, vgc->buf_crt_load, zeros, (Uint32)bytes);
            free(zeros);
        }
    }

    /* Beam dispatch counter — resets per-frame noise phase + dot-crawl
     * field index so the next preset starts from zero. */
    vgc->deflection_cache_valid=false;
    vgc->beam_frame_counter = 0;
    vgc->signal_frame_counter = 0;
    vgc->temporal_history_valid = false;
    vgc->smoothing_history_valid = false;

    /* Carry buffer — holds AGC running-gain and RC-filter prefix-scan
     * inter-block state, both of which are time-integrators that can
     * accumulate history from the previous preset (e.g. a dim rolling
     * gain that takes seconds to recover). Zero it so the next preset
     * starts from a neutral baseline. */
    SignalChain *sc = &vgc->sig_chain;
    if (sc->carry_buf && sc->carry_size > 0) {
        void *zeros = calloc(1, sc->carry_size);
        if (zeros) {
            gpu_buffer_upload(gpu, sc->carry_buf, zeros, sc->carry_size);
            free(zeros);
        }
    }

    /* Aux buffers — chroma C, I raw, Q raw, filtered I/Q. Lines of the
     * new frame overwrite them anyway, but the FIR stages mirror-pad
     * at line edges so stale samples near the edge can leak into the
     * new frame's filtered output for one frame. Cheap to zero. */
    for (int i = 0; i < CHAIN_MAX_AUX_BUFS; i++) {
        if (sc->aux[i] && sc->aux_size > 0) {
            void *zeros = calloc(1, sc->aux_size);
            if (zeros) {
                gpu_buffer_upload(gpu, sc->aux[i], zeros, sc->aux_size);
                free(zeros);
            }
        }
    }

    /* PAL decoder scratch buffers. The first line of each frame writes
     * through directly, but zeroing here keeps any debug capture or
     * partial-frame inspection from seeing stale data after a preset
     * swap. */
    if (vgc->buf_pal_v && vgc->signal_size > 0) {
        void *zeros = calloc(1, vgc->signal_size);
        if (zeros) {
            gpu_buffer_upload(gpu, vgc->buf_pal_v, zeros, vgc->signal_size);
            free(zeros);
        }
    }
    if (vgc->buf_pal_u && vgc->signal_size > 0) {
        void *zeros = calloc(1, vgc->signal_size);
        if (zeros) {
            gpu_buffer_upload(gpu, vgc->buf_pal_u, zeros, vgc->signal_size);
            free(zeros);
        }
    }
    if (vgc->buf_deflection_x && vgc->deflection_size > 0) {
        void *zeros = calloc(1, vgc->deflection_size);
        if (zeros) {
            gpu_buffer_upload(gpu, vgc->buf_deflection_x, zeros, vgc->deflection_size);
            free(zeros);
        }
    }
    if (vgc->buf_deflection_y && vgc->deflection_size > 0) {
        void *zeros = calloc(1, vgc->deflection_size);
        if (zeros) {
            gpu_buffer_upload(gpu, vgc->buf_deflection_y, zeros, vgc->deflection_size);
            free(zeros);
        }
    }
}

/* ============================================================================
 * Cleanup
 * ============================================================================ */

void video_gpu_destroy(VideoGPUChain *vgc, SDL_GPUDevice *gpu)
{
    if (!vgc) return;

    /* Destroy the signal chain (frees ping-pong, aux, carry, tap buffers,
     * and all chain-owned pipelines). */
    chain_destroy(&vgc->sig_chain, gpu);
    vhs_gpu_destroy(&vgc->vhs, gpu);

    /* Release DAC pipeline (not owned by chain). */
    gpu_pipeline_destroy(gpu, &vgc->pipe_dac);
    gpu_pipeline_destroy(gpu, &vgc->pipe_encoder);
    if (vgc->pixels_transfer) { SDL_ReleaseGPUTransferBuffer(gpu, vgc->pixels_transfer); vgc->pixels_transfer = NULL; }
    if (vgc->buf_pixels) { SDL_ReleaseGPUBuffer(gpu, vgc->buf_pixels); vgc->buf_pixels = NULL; }
    if (vgc->buf_ramp) { SDL_ReleaseGPUBuffer(gpu, vgc->buf_ramp); vgc->buf_ramp = NULL; }
    vgc->ramp_uploaded = false;

    /* Release DAC-specific buffers. */
    if (vgc->buf_crt_load) SDL_ReleaseGPUBuffer(gpu, vgc->buf_crt_load);
    if (vgc->buf_gun_current) SDL_ReleaseGPUBuffer(gpu, vgc->buf_gun_current);
    if (vgc->buf_receiver_measurements) SDL_ReleaseGPUBuffer(gpu, vgc->buf_receiver_measurements);
    if (vgc->buf_agc_gains) SDL_ReleaseGPUBuffer(gpu, vgc->buf_agc_gains);
    if (vgc->buf_receiver) { SDL_ReleaseGPUBuffer(gpu, vgc->buf_receiver); vgc->buf_receiver = NULL; }
    if (vgc->indices_transfer) { SDL_ReleaseGPUTransferBuffer(gpu, vgc->indices_transfer); vgc->indices_transfer = NULL; }
    if (vgc->buf_indices)      { SDL_ReleaseGPUBuffer(gpu, vgc->buf_indices);      vgc->buf_indices = NULL; }
    if (vgc->buf_signal_table) { SDL_ReleaseGPUBuffer(gpu, vgc->buf_signal_table); vgc->buf_signal_table = NULL; }
    if (vgc->buf_signal_table_alt) { SDL_ReleaseGPUBuffer(gpu, vgc->buf_signal_table_alt); vgc->buf_signal_table_alt = NULL; }
    if (vgc->buf_rgb)          { SDL_ReleaseGPUBuffer(gpu, vgc->buf_rgb);          vgc->buf_rgb = NULL; }
    if (vgc->buf_rgb2)         { SDL_ReleaseGPUBuffer(gpu, vgc->buf_rgb2);         vgc->buf_rgb2 = NULL; }
    if (vgc->buf_pal_v)        { SDL_ReleaseGPUBuffer(gpu, vgc->buf_pal_v);        vgc->buf_pal_v = NULL; }
    if (vgc->buf_pal_u)        { SDL_ReleaseGPUBuffer(gpu, vgc->buf_pal_u);        vgc->buf_pal_u = NULL; }
    if (vgc->buf_deflection_x) { SDL_ReleaseGPUBuffer(gpu, vgc->buf_deflection_x); vgc->buf_deflection_x = NULL; }
    if (vgc->buf_deflection_y) { SDL_ReleaseGPUBuffer(gpu, vgc->buf_deflection_y); vgc->buf_deflection_y = NULL; }
    if (vgc->buf_beam_rgba)    { SDL_ReleaseGPUBuffer(gpu, vgc->buf_beam_rgba);    vgc->buf_beam_rgba = NULL; }
    if (vgc->buf_beam_prev)    { SDL_ReleaseGPUBuffer(gpu, vgc->buf_beam_prev);    vgc->buf_beam_prev = NULL; }
    if(vgc->buf_osd) { SDL_ReleaseGPUBuffer(gpu,vgc->buf_osd); vgc->buf_osd=NULL; }
    free(vgc->osd_cache); vgc->osd_cache=NULL;
    if(vgc->buf_rf_carrier) { SDL_ReleaseGPUBuffer(gpu,vgc->buf_rf_carrier); vgc->buf_rf_carrier=NULL; }
    if (vgc->buf_phosphor_history) { SDL_ReleaseGPUBuffer(gpu, vgc->buf_phosphor_history); vgc->buf_phosphor_history = NULL; }
    if (vgc->tex_beam)         { SDL_ReleaseGPUTexture(gpu, vgc->tex_beam);        vgc->tex_beam = NULL; }

    vgc->chain = NULL;
}

/* ============================================================================
 * Signal table upload (for DAC shader)
 * ============================================================================ */

bool video_gpu_upload_signal_table(VideoGPUChain *vgc, SDL_GPUDevice *gpu,
                                    const float *table, const float *table_alt,
                                    int entries, int stride) {
    Uint32 table_bytes = (Uint32)(entries * stride * sizeof(float));

    if (!vgc->buf_signal_table) {
        vgc->buf_signal_table = gpu_buffer_create(gpu, table_bytes, GPU_BUF_READONLY);
        if (!vgc->buf_signal_table) return false;
    }
    if (!gpu_buffer_upload(gpu, vgc->buf_signal_table, table, table_bytes))
        return false;

    /* PAL alt table: allocated lazily when a non-NULL alt is supplied
     * (NTSC presets pass NULL). Released if caller subsequently passes
     * NULL so switching back to NTSC frees the buffer. */
    if (table_alt) {
        if (!vgc->buf_signal_table_alt) {
            vgc->buf_signal_table_alt = gpu_buffer_create(
                gpu, table_bytes, GPU_BUF_READONLY);
            if (!vgc->buf_signal_table_alt) return false;
        }
        if (!gpu_buffer_upload(gpu, vgc->buf_signal_table_alt,
                               table_alt, table_bytes))
            return false;
    } else if (vgc->buf_signal_table_alt) {
        SDL_ReleaseGPUBuffer(gpu, vgc->buf_signal_table_alt);
        vgc->buf_signal_table_alt = NULL;
    }
    return true;
}

/* ============================================================================
 * Full GPU path: DAC + signal chain (zero CPU signal work)
 * ============================================================================ */

bool video_gpu_process_full(VideoGPUChain *vgc, SDL_GPUDevice *gpu,
                             const uint16_t *idx_fb,
                             int phase_base, int phase_line_adv, int frame_field,
                             float *rgb_out) {
    (void)frame_field; /* phase_base already includes the frame's clock phase */
    if (!vgc || !idx_fb || !vgc->pipe_dac.pipeline || !vgc->buf_signal_table) return false;
    const SignalFormat *fmt = &vgc->signal_fmt;
    if (fmt->region == SIGNAL_REGION_PAL && !vgc->buf_signal_table_alt) return false;
    vgc->source_separated=vgc->chain->connection==VIDEO_CONN_SVIDEO;
    const Uint32 bytes = 256 * 240 * sizeof(uint16_t);
    if (!vgc->buf_indices) {
        vgc->indices_size = bytes;
        vgc->buf_indices = gpu_buffer_create(gpu, bytes, GPU_BUF_READONLY);
    }
    if (!vgc->indices_transfer) {
        SDL_GPUTransferBufferCreateInfo info = {.usage=SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD, .size=bytes};
        vgc->indices_transfer = SDL_CreateGPUTransferBuffer(gpu, &info);
    }
    if (!vgc->buf_indices || !vgc->indices_transfer) return false;
    void *mapped = SDL_MapGPUTransferBuffer(gpu, vgc->indices_transfer, true);
    if (!mapped) return false;
    memcpy(mapped, idx_fb, bytes);
    SDL_UnmapGPUTransferBuffer(gpu, vgc->indices_transfer);
    SDL_GPUCommandBuffer *cmd = SDL_AcquireGPUCommandBuffer(gpu);
    if (!cmd) return false;
    SDL_GPUCopyPass *copy = SDL_BeginGPUCopyPass(cmd);
    if (!copy) { SDL_CancelGPUCommandBuffer(cmd); return false; }
    SDL_GPUTransferBufferLocation source = {.transfer_buffer=vgc->indices_transfer, .offset=0};
    SDL_GPUBufferRegion target = {.buffer=vgc->buf_indices, .offset=0, .size=bytes};
    SDL_UploadToGPUBuffer(copy, &source, &target, false);
    SDL_EndGPUCopyPass(copy);
    /* An RGB PPU writes the decode window directly: the picture, the
     * backdrop around it on the lines and dots the 2C02 draws it, and black
     * elsewhere. */
    const DecodeWindow *w = &vgc->window;
    struct {
        uint32_t samples_per_pixel, samples_per_line, phase_base, phase_line_adv;
        uint32_t phase_field_adv, frame_field, use_alt_table, source_mode;
        float rgb_rows[3][4];
        uint32_t backdrop_entry, window_dots, window_lines, window_width;
        int32_t start_dot, picture_dot, picture_row; uint32_t region;
    } params = {(uint32_t)fmt->samples_per_pixel, (uint32_t)fmt->samples_per_line,
        (uint32_t)((phase_base % 12 + 12) % 12),
        (uint32_t)((phase_line_adv % 12 + 12) % 12), 0, 0,
        fmt->region == SIGNAL_REGION_PAL ? 1u : 0u, vgc->source_separated ? 1u : 0u, {{0}},
        vgc->backdrop_entry & 0x1ffu, (uint32_t)w->dots, (uint32_t)w->lines, (uint32_t)w->width,
        w->start_dot, w->picture_dot, w->picture_row, (uint32_t)fmt->region};
    bool source_rgb=!video_connection_uses_signal_decode(vgc->chain->connection);
    if(source_rgb) {
        params.source_mode=2;
        for(int c=0;c<3;c++) {
            memcpy(params.rgb_rows[c],vgc->color_matrix[c],3*sizeof(float));
            params.rgb_rows[c][3]=vgc->color_bias[c];
        }
    }
    SDL_GPUStorageBufferReadWriteBinding rw[2] = {{.buffer=source_rgb ? vgc->buf_rgb : vgc->sig_chain.buf[0]}, {.buffer=vgc->sig_chain.aux[3]}};
    SDL_GPUComputePass *pass = SDL_BeginGPUComputePass(cmd, NULL, 0, rw, 2);
    if (!pass) { SDL_CancelGPUCommandBuffer(cmd); return false; }
    SDL_GPUBuffer *inputs[] = {vgc->buf_indices, vgc->buf_signal_table,
        vgc->buf_signal_table_alt ? vgc->buf_signal_table_alt : vgc->buf_signal_table};
    SDL_BindGPUComputePipeline(pass, vgc->pipe_dac.pipeline);
    SDL_BindGPUComputeStorageBuffers(pass, 0, inputs, 3);
    SDL_PushGPUComputeUniformData(cmd, 0, &params, sizeof(params));
    if (source_rgb) SDL_DispatchGPUCompute(pass, (uint32_t)(w->dots + 255) / 256, (uint32_t)w->lines, 1);
    else SDL_DispatchGPUCompute(pass, 1, 240, 1);
    SDL_EndGPUComputePass(pass);
    vgc->signal_phase_base = phase_base;
    vgc->signal_line_phase = phase_line_adv;
    update_signal_time(vgc);
    vgc->demod_line_phase = phase_line_adv * 2.0f * (float)M_PI / 12.0f;
    update_demod_params(vgc);
    vgc->sig_chain.current_buf = 0;
    if (!vhs_gpu_frame(&vgc->vhs, &vgc->sig_chain, gpu, cmd, vgc->signal_frame_counter - 1,
                       -vgc->raster_sync_level) ||
        !chain_run_cmd(&vgc->sig_chain, cmd)) {
        vgc->deflection_cache_valid = false;
        SDL_CancelGPUCommandBuffer(cmd);
        return false;
    }
    if (!SDL_SubmitGPUCommandBuffer(cmd)) {
        vgc->deflection_cache_valid = false;
        return false;
    }
    return !rgb_out || gpu_buffer_download(gpu, vgc->buf_rgb, rgb_out, vgc->rgb_size);
}

/* ============================================================================
 * RGB console path: encoder IC source + signal chain
 * ============================================================================ */

bool video_gpu_process_rgb(VideoGPUChain *vgc, SDL_GPUDevice *gpu, const VideoRGBSource *src) {
    const bool linear = src && src->code_bits == 10;
    if (!vgc || !src || !src->pixels || (!linear && !src->ramp) || !vgc->pipe_encoder.pipeline) return false;
    if (src->width <= 0 || src->width > 1024 || src->lines <= 0 || src->lines > 240 ||
        src->top_line < 0 || src->top_line + src->lines > 240 ||
        src->spp_num <= 0 || src->spp_den <= 0 ||
        (src->code_bits != 0 && src->code_bits != 10) || (!linear && (src->ramp_n <= 0 || src->ramp_n > 64)))
        return false;
    const SignalFormat *fmt = &vgc->signal_fmt;
    vgc->source_separated = vgc->chain->connection == VIDEO_CONN_SVIDEO;

    /* Pixel codes: sized for the widest picture on first use. */
    const Uint32 max_bytes = 1024 * 240 * sizeof(uint32_t);
    const Uint32 bytes = (Uint32)(src->width * src->lines) * sizeof(uint32_t);
    if (!vgc->buf_pixels) {
        vgc->pixels_size = max_bytes;
        vgc->buf_pixels = gpu_buffer_create(gpu, max_bytes, GPU_BUF_READONLY);
    }
    if (!vgc->pixels_transfer) {
        SDL_GPUTransferBufferCreateInfo info = {.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD, .size = max_bytes};
        vgc->pixels_transfer = SDL_CreateGPUTransferBuffer(gpu, &info);
    }
    if (!vgc->buf_ramp) vgc->buf_ramp = gpu_buffer_create(gpu, 64 * sizeof(float), GPU_BUF_READONLY);
    if (!vgc->buf_pixels || !vgc->pixels_transfer || !vgc->buf_ramp) return false;

    /* The DAC ramp changes with the console, not the frame. */
    float ramp[64] = {0};
    if (!linear) memcpy(ramp, src->ramp, (size_t)src->ramp_n * sizeof(float));
    if (!vgc->ramp_uploaded || memcmp(ramp, vgc->ramp_cache, sizeof(ramp)) != 0) {
        if (!gpu_buffer_upload(gpu, vgc->buf_ramp, ramp, sizeof(ramp))) return false;
        memcpy(vgc->ramp_cache, ramp, sizeof(ramp));
        vgc->ramp_uploaded = true;
    }

    void *mapped = SDL_MapGPUTransferBuffer(gpu, vgc->pixels_transfer, true);
    if (!mapped) return false;
    memcpy(mapped, src->pixels, bytes);
    SDL_UnmapGPUTransferBuffer(gpu, vgc->pixels_transfer);
    SDL_GPUCommandBuffer *cmd = SDL_AcquireGPUCommandBuffer(gpu);
    if (!cmd) return false;
    SDL_GPUCopyPass *copy = SDL_BeginGPUCopyPass(cmd);
    if (!copy) { SDL_CancelGPUCommandBuffer(cmd); return false; }
    SDL_GPUTransferBufferLocation source = {.transfer_buffer = vgc->pixels_transfer, .offset = 0};
    SDL_GPUBufferRegion target = {.buffer = vgc->buf_pixels, .offset = 0, .size = bytes};
    SDL_UploadToGPUBuffer(copy, &source, &target, false);
    SDL_EndGPUCopyPass(copy);

    /* Encoder chroma band as a baseband low-pass: a windowed sinc whose
     * length follows the cutoff, capped where the shader's loop stays cheap. */
    float rate = signal_format_sample_rate_hz(fmt);
    float cut = src->chroma_bw_hz > 0 ? src->chroma_bw_hz / rate : 0.0f;
    float luma_cut = src->luma_bw_hz > 0 ? src->luma_bw_hz / rate : 0.0f;
    float trap_depth = fminf(fmaxf(src->luma_trap, 0.0f), 1.0f);
    float trap_cut = 1.0e6f / rate; /* half-width; narrower needs more taps than the loop allows */
    float narrowest = 0.0f;
    if (cut > 0) narrowest = cut;
    if (luma_cut > 0 && (narrowest == 0 || luma_cut < narrowest)) narrowest = luma_cut;
    if (trap_depth > 0 && (narrowest == 0 || trap_cut < narrowest)) narrowest = trap_cut;
    uint32_t taps = 0;
    if (narrowest > 0) {
        int n = (int)(2.0f / narrowest) | 1;
        if (n < 5) n = 5;
        if (n > 63) n = 63;
        taps = (uint32_t)n;
    }
    bool source_rgb = !video_connection_uses_signal_decode(vgc->chain->connection);
    /* RGB goes straight to the decode window, the picture in black. */
    const DecodeWindow *w = &vgc->window;
    struct {
        uint32_t width, lines, samples_per_line, spp_num;
        uint32_t spp_den, top_line, source_mode, taps;
        float phase_base, phase_line_adv, chroma_cut, setup;
        float luma_cut, trap_cut, trap_depth;
        uint32_t code_bits;
        float rgb_rows[3][4];
        uint32_t window_width, window_lines;
        int32_t picture_x, picture_row;
    } params = {
        (uint32_t)src->width, (uint32_t)src->lines, (uint32_t)fmt->samples_per_line, (uint32_t)src->spp_num,
        (uint32_t)src->spp_den, (uint32_t)src->top_line,
        source_rgb ? 2u : (vgc->source_separated ? 1u : 0u), taps,
        (float)((src->phase_base % 12 + 12) % 12), (float)((src->phase_line_adv % 12 + 12) % 12),
        cut, src->setup, luma_cut, trap_cut, trap_depth, linear ? 10u : 0u, {{0}},
        (uint32_t)w->width, (uint32_t)w->lines, w->picture_x, w->picture_row};
    if (source_rgb) {
        for (int c = 0; c < 3; c++) {
            memcpy(params.rgb_rows[c], vgc->color_matrix[c], 3 * sizeof(float));
            params.rgb_rows[c][3] = vgc->color_bias[c];
        }
    }
    SDL_GPUStorageBufferReadWriteBinding rw[2] = {
        {.buffer = source_rgb ? vgc->buf_rgb : vgc->sig_chain.buf[0]}, {.buffer = vgc->sig_chain.aux[3]}};
    SDL_GPUComputePass *pass = SDL_BeginGPUComputePass(cmd, NULL, 0, rw, 2);
    if (!pass) { SDL_CancelGPUCommandBuffer(cmd); return false; }
    SDL_GPUBuffer *inputs[] = {vgc->buf_pixels, vgc->buf_ramp};
    SDL_BindGPUComputePipeline(pass, vgc->pipe_encoder.pipeline);
    SDL_BindGPUComputeStorageBuffers(pass, 0, inputs, 2);
    SDL_PushGPUComputeUniformData(cmd, 0, &params, sizeof(params));
    uint32_t total = source_rgb ? (uint32_t)decode_window_samples(w) : (uint32_t)fmt->samples_per_line * 240u;
    SDL_DispatchGPUCompute(pass, (total + 255u) / 256u, 1, 1);
    SDL_EndGPUComputePass(pass);

    /* An encoder IC's raster: -40 IRE sync, 40 IRE peak-to-peak sine burst,
     * and its pedestal over the whole active line: the console's border is
     * black at setup, as its picture's black is. */
    vgc->raster_sync_level = -0.4f;
    vgc->raster_burst_amp = 0.2f;
    vgc->raster_burst_sine = true;
    for (int k = 0; k < 12; k++) vgc->backdrop[k] = vgc->gray_backdrop[k] = src->setup;
    vgc->signal_phase_base = src->phase_base;
    vgc->signal_line_phase = src->phase_line_adv;
    update_signal_time(vgc);
    vgc->demod_line_phase = src->phase_line_adv * 2.0f * (float)M_PI / 12.0f;
    update_demod_params(vgc);
    vgc->sig_chain.current_buf = 0;
    if (!vhs_gpu_frame(&vgc->vhs, &vgc->sig_chain, gpu, cmd, vgc->signal_frame_counter - 1,
                       -vgc->raster_sync_level) ||
        !chain_run_cmd(&vgc->sig_chain, cmd)) {
        vgc->deflection_cache_valid = false;
        SDL_CancelGPUCommandBuffer(cmd);
        return false;
    }
    if (!SDL_SubmitGPUCommandBuffer(cmd)) {
        vgc->deflection_cache_valid = false;
        return false;
    }
    return true;
}

bool video_gpu_download_window_rgb(VideoGPUChain *vgc, SDL_GPUDevice *gpu, float *window_rgb) {
    return vgc && window_rgb && vgc->buf_rgb && gpu_buffer_download(gpu, vgc->buf_rgb, window_rgb, vgc->rgb_size);
}
