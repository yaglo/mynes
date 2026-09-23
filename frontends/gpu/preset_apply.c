#include "debug_server.h"
/*
 * preset_apply.c -- Preset application, OSD menu callbacks, overlay compositing
 */
#include "preset_apply.h"
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <math.h>
#include <time.h>
#include <unistd.h>
#include <errno.h>
#include <stdlib.h>
#include <limits.h>
#include <sys/stat.h>
#include "ppu/ppu.h"
#include "preset_json.h"
#include "config.h"
#include "crt_color.h"

/* File-static context pointer, set by preset_ctx_init(). */
static PresetCtx *g_ctx = NULL;

/* Forward declaration. */
static void rebuild_color_matrix(PresetCtx *ctx);
static void rebuild_signal_filters(PresetCtx *ctx, float y_cutoff,
                                   float c_cutoff, float ringing,
                                   int min_y_taps, int min_c_taps);
static void preset_apply_cpu_state_ex(PresetCtx *ctx, const PhysicalPreset *p,
                                      bool preserve_live_region);

/* Forward declarations for callbacks used during preset_apply. */
static void gpu_cb_update_beam_params(void);
static void gpu_cb_update_rc_params(void);

/* ============================================================================
 * Preset application -- drives SignalPrecompute + VideoChain + AudioChain
 * ============================================================================ */

static void apply_audio_preset(PresetCtx *ctx,const PhysicalPreset *p) {
    /* --- AudioChain --- */
    audio_chain_init_preset(ctx->audio_chain, p->console_variant,
                            p->speaker_type, ctx->region);
    float audio_length=fmaxf(p->audio_cable_length_m,0.0f);
    float audio_cap=p->audio_cable.capacitance_per_m>0 ? p->audio_cable.capacitance_per_m : 67e-12f;
    ctx->audio_chain->cable.capacitance=audio_length*audio_cap;
    ctx->audio_chain->cable.resistance=75.0f + audio_length*fmaxf(p->audio_cable.resistance_per_m,0.0f)
        + fmaxf(p->audio_cable.connector_resistance,0.0f);
    if (p->audio_hum_frequency > 0) ctx->audio_chain->psu_hum.frequency = p->audio_hum_frequency;
    ctx->audio_chain->psu_hum.harmonic_2 = p->audio_hum_harmonic_2;
    ctx->audio_chain->psu_hum.harmonic_3 = p->audio_hum_harmonic_3;
    ctx->audio_chain->psu_hum.amplitude=fmaxf(0,p->audio_psu_hum_amplitude);
    ctx->audio_chain->psu_hum.enabled=p->audio_psu_hum_amplitude>0;
    ctx->audio_chain->noise_floor.amplitude=fmaxf(0,p->audio_noise_floor);
    ctx->audio_chain->noise_floor.enabled=p->audio_noise_floor>0;
    ctx->audio_chain->amp_saturation.drive=fmaxf(1,p->audio_saturation_drive);
    ctx->audio_chain->amp_saturation.enabled=p->audio_saturation_drive>1.01f;
    audio_chain_prepare(ctx->audio_chain);
}

/* CPU-side portion of a preset apply — populates SignalPrecompute,
 * VideoChain, and AudioChain from the preset JSON. Idempotent; safe to
 * call before the GPU chain has been initialised (used at startup so
 * video_gpu_init / audio_gpu_init see the correct FIR tap counts and
 * audio params). Does NOT touch GPU state. */
static void preset_apply_cpu_state_ex(PresetCtx *ctx, const PhysicalPreset *p,
                                      bool preserve_live_region) {
    int old_region = signal_region_normalize(ctx->region);
    int new_region = preserve_live_region
                     ? old_region
                     : signal_region_normalize(p->region);
    bool region_changed = (new_region != old_region);

    ctx->region = new_region;
    ctx->osd_region_sel = new_region;
    ctx->region_switched = region_changed;
    if (region_changed) {
        /* Regenerate both NTSC and PAL tables under the new region.
         * The GPU-side rebuild happens in preset_apply_gpu_push so
         * all the transient state settles in the right order. */
        signal_precompute_init(ctx->sig_state, new_region);
    }

    /* --- VideoChain --- */
    video_chain_init_preset(ctx->video_chain, p->connection,
                            p->comb_type, new_region);
    ctx->video_chain->tv = p->tv;
    TVDisplayParams *tv = &ctx->video_chain->tv;
    tv->beam_fwhm_min = video_beam_sigma(tv, false) * 2.354820045f;
    tv->beam_fwhm_max = video_beam_sigma(tv, true) * 2.354820045f;
    if (tv->video_recovery_us <= 0.0f) tv->video_recovery_us = 18.0f;
    /* Zero-initialized preset fields (HPOS/VPOS/HSIZE/VSIZE) must default to
     * the identity transform (centered raster filling the tube). Older presets
     * don't set these, so we fix them up after the struct copy. */
    if (ctx->video_chain->tv.h_size < 0.01f) ctx->video_chain->tv.h_size = 1.0f;
    if (ctx->video_chain->tv.v_size < 0.01f) ctx->video_chain->tv.v_size = 1.0f;
    ctx->video_chain->cable = p->video_cable;
    ctx->video_chain->rf = p->rf;
    ctx->video_chain->vhs = p->vhs;
    // Expose effective defaults in the OSD, not a misleading zero value.
    RFModulatorParams *rf=&ctx->video_chain->rf;
    rf->enabled=p->connection==VIDEO_CONN_RF;
    if(rf->carrier_level_dbm==0) rf->carrier_level_dbm=-20;
    if(rf->noise_floor_dbm==0) rf->noise_floor_dbm=-70;
    if(rf->mod_bandwidth<=0) rf->mod_bandwidth=4e6f;
    if(rf->agc_attack_ms<=0) {
        rf->agc_attack_ms=-signal_region_frame_ms(new_region)/logf(.9f);
        rf->agc_release_ms=-signal_region_frame_ms(new_region)/logf(.98f);
    }
    if(ctx->video_chain->vhs.drift_ms<=0) ctx->video_chain->vhs.drift_ms=180;
    if(ctx->video_chain->vhs.luma_bandwidth<=0) ctx->video_chain->vhs.luma_bandwidth=2.5e6f;
    if(ctx->video_chain->vhs.chroma_bandwidth<=0) ctx->video_chain->vhs.chroma_bandwidth=.35e6f;
    ctx->video_chain->console_coupling_R = p->console_coupling_R>0 ? p->console_coupling_R : 75.0f;
    ctx->video_chain->console_coupling_C = p->console_coupling_C;
    ctx->video_chain->console_amp_bw = p->console_amp_bw;
    ctx->video_chain->console_phase_distortion_ns = p->console_phase_distortion_ns;
    ctx->video_chain->console_psu_hum = p->console_psu_hum;
    ctx->video_chain->comb_notch_depth = p->comb_notch_depth;

    /* --- SignalPrecompute: FIR taps from TV bandwidth --- */
    float actual_sample_rate = signal_region_sample_rate_hz(new_region);
    float y_cutoff = p->tv.luma_bandwidth / actual_sample_rate;
    float c_cutoff = p->tv.chroma_bandwidth / actual_sample_rate;
    if (y_cutoff < 0.005f) y_cutoff = 0.005f;
    if (y_cutoff > 0.200f) y_cutoff = 0.200f;
    if (c_cutoff < 0.005f) c_cutoff = 0.005f;
    if (c_cutoff > 0.200f) c_cutoff = 0.200f;

    /* At preset-load time keep the old "clean Y" default (37-tap min +
     * notch) — cheap-TV fringing is opt-in via the OSD slider, not a
     * side effect of loading a preset written before the slider existed.
     * The gpu_cb_redesign_firs callback (triggered by OSD edits) uses
     * relaxed minimums to allow explicit overrides. */
    rebuild_signal_filters(ctx, y_cutoff, c_cutoff, p->tv.fir_ringing,
                           37, 21);

    /* --- SignalPrecompute: brightness/contrast/chroma_gain from preset --- */
    ctx->sig_state->brightness = p->brightness;  /* 0.0 is a valid default */
    ctx->sig_state->contrast = (p->contrast > 0.0f) ? p->contrast : 1.0f;
    ctx->sig_state->chroma_gain = p->chroma_gain;

    /* --- SignalPrecompute: color matrix from preset TV params --- */
    rebuild_color_matrix(ctx);

    apply_audio_preset(ctx,p);
}

void preset_apply_cpu_state(PresetCtx *ctx, const PhysicalPreset *p) {
    preset_apply_cpu_state_ex(ctx, p, false);
}

/* GPU-side portion of a preset apply — pushes the CPU state prepared
 * by preset_apply_cpu_state() into all the live GPU buffers, uniforms,
 * and stage params. Safe no-op when gpu_video_enabled is false (e.g.
 * during pre-init ordering). Re-pushes everything so it's idempotent. */
void preset_apply_gpu_push(PresetCtx *ctx) {
    /* --- Push ALL updated params to GPU chain --- */
    if (*ctx->gpu_video_enabled) {
        VideoGPUChain *vgc = ctx->video_gpu_chain;

        /* Region flip — need a full GPU-chain rebuild because the
         * buffer sizing and per-stage dispatch dims both depend on
         * samples_per_line, which is region-specific. Done before any
         * other push so subsequent calls see a freshly-built chain. */
        if (ctx->region_switched && ctx->shader_dir) {
            SDL_WaitForGPUIdle(ctx->gpu);
            if (!video_gpu_rebuild_for_region(
                    vgc, ctx->gpu, ctx->video_chain, ctx->shader_dir,
                    ctx->sig_state->fir_y, ctx->sig_state->fir_y_n,
                    ctx->sig_state->fir_c, ctx->sig_state->fir_c_n,
                    ctx->sig_state->fir_q, ctx->sig_state->fir_q_n)) {
                fprintf(stderr, "preset_apply: region rebuild failed — "
                                "GPU video disabled\n");
                *ctx->gpu_video_enabled = false;
                ctx->region_switched = false;
                return;
            }
            /* Re-upload signal tables for the new region (both base
             * and — for PAL — the alt-line table). */
            const float *alt = (ctx->sig_state->region == SIGNAL_REGION_PAL)
                               ? (const float *)ctx->sig_state->table_alt
                               : NULL;
            video_gpu_upload_signal_table(vgc, ctx->gpu,
                                          (const float *)ctx->sig_state->table,
                                          alt,
                                          SIG_TABLE_ENTRIES, SIG_TABLE_STRIDE);
            /* After a region rebuild, refresh the carrier constants from
             * the live region. In this frontend dp remains 2*pi/12 for
             * both regions, but deriving it from the region helpers keeps
             * the code honest about which carrier family is active. */
            float fsc = signal_region_subcarrier_hz(ctx->sig_state->region);
            float fsample = signal_region_sample_rate_hz(ctx->sig_state->region);
            float dp = 2.0f * (float)M_PI * fsc / fsample;
            video_gpu_set_demod(vgc, 0.0f, dp);
        }

        /* FIR taps (luma + per-channel chroma bandwidth). */
        video_gpu_update_fir_taps(vgc, ctx->gpu,
                                  ctx->sig_state->fir_y, ctx->sig_state->fir_y_n,
                                  ctx->sig_state->fir_c, ctx->sig_state->fir_c_n,
                                  ctx->sig_state->fir_q, ctx->sig_state->fir_q_n);

        /* Color matrix (hue, saturation, color temp, brightness, contrast). */
        video_gpu_set_color_matrix(vgc,
                                   ctx->sig_state->color_matrix,
                                   ctx->sig_state->color_bias);

        /* Beam params (sigma, spot size, black floor). */
        gpu_cb_update_beam_params();

        /* RC filter params (console LP, cable RC alpha). */
        gpu_cb_update_rc_params();

        /* Re-init the entire GPU chain to pick up comb_type, connection type,
         * video amp bandwidth, and stage enable changes.
         *
         * Note: a true full video_gpu_destroy + video_gpu_init on every
         * preset change invalidates pointers held by tap_mgr / chain_vis /
         * debug_srv into the signal chain stages, which breaks those
         * downstream consumers. The partial reinit updates stage params
         * in place without touching topology — stage enables set at init
         * remain fixed for the session. */
        video_gpu_reinit_stages(vgc, ctx->video_chain);

        /* Per-preset temporal state — zero beam counter + previous-
         * frame beam buffer + AGC/RC carry + chroma aux buffers so no
         * integrator leaks state from the outgoing preset. */
        video_gpu_reset_temporal_state(vgc, ctx->gpu);
    }

    /* CPU-side transient state that lives outside the GPU chain but
     * still holds per-frame smoothing — the HV-sag controller in the
     * render context integrates over ~100 ms per sample, so without
     * this reset it can slowly fade away from a bright scene on the
     * previous preset over many frames. */
    if (ctx->render_ctx) {
        ctx->render_ctx->hv_sag_state = 0.0f;
    }
    /* Consume the region-switch trigger so subsequent applies don't
     * fire another expensive rebuild. Callers that monitor this flag
     * (main.c → tap_mgr rebuild) must read it BEFORE calling the
     * next preset-apply function. */
    ctx->region_switched = false;
}

/* Back-compat entry point: the OSD preset-pick path calls this and
 * expects full apply (CPU state + GPU push) in one shot. Startup calls
 * the two halves separately with GPU init sandwiched between them. */
void preset_apply(PresetCtx *ctx, const PhysicalPreset *p) {
    preset_apply_cpu_state_ex(ctx, p, true);
    preset_apply_gpu_push(ctx);
}

bool preset_set_region(PresetCtx *ctx, int new_region) {
    if (new_region != SIGNAL_REGION_NTSC && new_region != SIGNAL_REGION_PAL)
        return false;
    if (new_region == ctx->region) {
        ctx->region_switched = false;
        return false;
    }

    /* 1. Regenerate signal tables for the new region. */
    signal_precompute_init(ctx->sig_state, new_region);

    /* 2. Update the VideoChain's format descriptor so the GPU
     *    rebuild picks up the new samples_per_line / pixel. All the
     *    other TV parameters stay the same as they were — we are
     *    only toggling the region, not reloading a preset. */
    signal_format_init(&ctx->video_chain->signal_fmt, new_region);

    /* 3. Rebuild FIR taps at the new sample rate. The preset carries
     *    bandwidths in Hz; the normalised cutoff changes with region. */
    float fsample = signal_region_sample_rate_hz(new_region);
    float y_cutoff = ctx->video_chain->tv.luma_bandwidth / fsample;
    float c_cutoff = ctx->video_chain->tv.chroma_bandwidth / fsample;
    if (y_cutoff < 0.005f) y_cutoff = 0.005f;
    if (y_cutoff > 0.200f) y_cutoff = 0.200f;
    if (c_cutoff < 0.005f) c_cutoff = 0.005f;
    if (c_cutoff > 0.200f) c_cutoff = 0.200f;
    rebuild_signal_filters(ctx, y_cutoff, c_cutoff,
                           ctx->video_chain->tv.fir_ringing, 5, 5);

    /* 4. Switch the decode matrix to match the new region.
     *    rebuild_color_matrix picks base_ntsc vs base_pal off
     *    sp->region, which we just updated. Without this call the
     *    matrix stays at the old region's coefficients and PAL
     *    decodes with YIQ (looks "just like NTSC" — sky goes
     *    purple instead of blue). */
    ctx->audio_chain->psu_hum.frequency = (new_region == SIGNAL_REGION_PAL) ? 50.0f : 60.0f;
    audio_chain_prepare(ctx->audio_chain);

    ctx->region = new_region;
    ctx->osd_region_sel = new_region;
    rebuild_color_matrix(ctx);

    /* 5. Ask preset_apply_gpu_push to do the rebuild on its next
     *    step — cheaper than duplicating the teardown+upload code.
     *    gpu_push clears ctx->region_switched when it finishes, so
     *    return whether a real rebuild actually ran (always true at
     *    this point, since we took the early-exits above). */
    ctx->region_switched = true;
    preset_apply_gpu_push(ctx);
    /* Flag for the main loop (OSD-driven region flips can't destroy
     * tap_mgr directly — it's owned by main.c). */
    ctx->chain_rebuilt_flag = true;
    return true;
}

/* ============================================================================
 * Color matrix rebuild from current TV params + brightness/contrast
 * ============================================================================ */

static void rebuild_color_matrix(PresetCtx *ctx) {
    VideoChain *vc = ctx->video_chain;
    SignalPrecompute *sp = ctx->sig_state;

    crt_decoder_matrix(&vc->tv, sp->region == SIGNAL_REGION_PAL,
        sp->contrast, sp->brightness, sp->chroma_gain, sp->color_matrix, sp->color_bias);

    if (*ctx->gpu_video_enabled) {
        video_gpu_set_color_matrix(ctx->video_gpu_chain,
                                   sp->color_matrix, sp->color_bias);
    }
}

static void rebuild_signal_filters(PresetCtx *ctx, float y_cutoff,
                                   float c_cutoff, float ringing,
                                   int min_y_taps, int min_c_taps) {
    VideoChain *vc = ctx->video_chain;
    SignalPrecompute *sp = ctx->sig_state;

    int y_taps = sp->fir_y_n;
    int c_taps = sp->fir_c_n;

    if (y_taps <= 0) y_taps = ((int)(0.45f / y_cutoff) | 1);
    if (c_taps <= 0) c_taps = ((int)(0.45f / c_cutoff) | 1);

    if (y_taps < min_y_taps) y_taps = min_y_taps;
    if (y_taps > 63) y_taps = 63;
    if (!(y_taps & 1)) y_taps++;
    if (c_taps < min_c_taps) c_taps = min_c_taps;
    if (c_taps > 63) c_taps = 63;
    if (!(c_taps & 1)) c_taps++;

    sp->fir_y_n = y_taps;
    sp->fir_c_n = c_taps;

    /* Q-channel cutoff: chroma_q_bandwidth if set, else match I (cheap
     * equi-band decoder — the historical default). Q taps sized from
     * Q cutoff when the preset asked for a narrower Q. */
    float sample_rate = signal_region_sample_rate_hz(sp->region);
    float q_bw = vc->tv.chroma_q_bandwidth;
    float q_cutoff = (q_bw > 1.0f) ? (q_bw / sample_rate) : c_cutoff;
    if (q_cutoff < 0.005f) q_cutoff = 0.005f;
    if (q_cutoff > 0.200f) q_cutoff = 0.200f;

    int q_taps = sp->fir_q_n;
    if (q_taps <= 0) q_taps = ((int)(0.45f / q_cutoff) | 1);
    /* Narrower Q needs more taps for the same transition width, so the
     * tap count tracks 1/q_cutoff. Clamp to the same ranges as I. */
    if (q_cutoff < c_cutoff) {
        int required = ((int)(0.45f / q_cutoff)) | 1;
        if (q_taps < required) q_taps = required;
    }
    if (q_taps < min_c_taps) q_taps = min_c_taps;
    if (q_taps > 63) q_taps = 63;
    if (!(q_taps & 1)) q_taps++;
    sp->fir_q_n = q_taps;

    signal_design_fir_ex(sp->fir_y, sp->fir_y_n, y_cutoff, ringing);

    /* Subcarrier notch in the luma FIR — removes the 3.58 MHz pattern
     * from Y to reduce cross-luma (dot crawl). It does not remove
     * luma leaking into the separate chroma decoder. Depth is taken
     * from tv.luma_notch_depth so presets can DISABLE it (set to 0) for
     * consumer-TV looks. Default 0.95 removes 95% of the residual carrier amplitude
     * after the lowpass (26 dB additional rejection). Skipped entirely below 23 taps — a short FIR can't
     * host a sharp notch and the attempt just attenuates luma broadly. */
    float subcarrier_norm = 1.0f / 12.0f;  /* NTSC/PAL: 12 samples per cycle */
    float notch_depth = vc->tv.luma_notch_depth;
    // NTSC line combs cannot separate PAL alternating V. Use horizontal
    // separation plus the PAL delay line until a PAL comb topology exists.
    if(vc->signal_fmt.region==SIGNAL_REGION_PAL && video_chain_comb_mode_separates(vc->comb_type))
        notch_depth=fmaxf(notch_depth,0.95f);
    if (notch_depth < 0.0f) notch_depth = 0.0f;
    if (notch_depth > 1.0f) notch_depth = 1.0f;
    if (video_chain_stage_active(vc, 6) && notch_depth > 0.01f && sp->fir_y_n >= 23) {
        signal_design_fir_notch(sp->fir_y, sp->fir_y_n, y_cutoff,
                                subcarrier_norm, notch_depth);
    }

    /* Sharpness runs on the recovered Y in its own GPU stage. Adding a
     * highpass here would bypass the receiver bandwidth/trap and feed
     * rejected chroma back into luminance. */
    signal_design_fir_ex(sp->fir_c, sp->fir_c_n, c_cutoff, ringing);
    signal_design_fir_ex(sp->fir_q, sp->fir_q_n, q_cutoff, ringing);
}

/* ============================================================================
 * OSD Menu callbacks (use g_ctx file-static pointer)
 * ============================================================================ */

static void gpu_cb_redesign_firs(void) {
    VideoChain *vc = g_ctx->video_chain;
    SignalPrecompute *sp = g_ctx->sig_state;
    float sample_rate = signal_region_sample_rate_hz(sp->region);
    float y_cutoff = vc->tv.luma_bandwidth / sample_rate;
    float c_cutoff = vc->tv.chroma_bandwidth / sample_rate;
    if (y_cutoff < 0.005f) y_cutoff = 0.005f;
    if (y_cutoff > 0.200f) y_cutoff = 0.200f;
    if (c_cutoff < 0.005f) c_cutoff = 0.005f;
    if (c_cutoff > 0.200f) c_cutoff = 0.200f;
    /* Clamp to sane minimums only. Short Y FIRs (< 23) can't form a
     * clean subcarrier notch — that's the regime that produces cheap-TV
     * cross-color fringing, which some presets want. */
    rebuild_signal_filters(g_ctx, y_cutoff, c_cutoff, vc->tv.fir_ringing,
                           5, 5);
    if (*g_ctx->gpu_video_enabled) {
        video_gpu_update_fir_taps(g_ctx->video_gpu_chain, g_ctx->gpu,
                                  sp->fir_y, sp->fir_y_n,
                                  sp->fir_c, sp->fir_c_n,
                                  sp->fir_q, sp->fir_q_n);
    }
}

static void gpu_cb_update_color_matrix(void) {
    rebuild_color_matrix(g_ctx);
}

static void gpu_cb_update_rc_params(void) {
    if (*g_ctx->gpu_video_enabled) {
        video_gpu_update_rc_params(g_ctx->video_gpu_chain);
    }
}

static void gpu_cb_update_beam_params(void) {
    if (!*g_ctx->gpu_video_enabled) return;
    VideoGPUChain *vgc = g_ctx->video_gpu_chain;
    TVDisplayParams *tv = &g_ctx->video_chain->tv;
    vgc->tail_weight=tv->persistence_tail_ms>0 ? fminf(1,fmaxf(0,tv->persistence_tail_weight)) : 0;
    int tail_region=vgc->signal_fmt.region;
    vgc->tail_r=signal_persistence_weight(tail_region,tv->persistence_tail_ms,tv->persistence_r);
    vgc->tail_g=signal_persistence_weight(tail_region,tv->persistence_tail_ms,tv->persistence_g);
    vgc->tail_b=signal_persistence_weight(tail_region,tv->persistence_tail_ms,tv->persistence_b);
    float sig_n = video_beam_sigma(tv, false);
    float sig_w = video_beam_sigma(tv, true);
    video_gpu_set_beam_params(vgc, g_ctx->gpu,
                              vgc->beam_out_w, vgc->beam_out_h,
                              vgc->beam_rows_per_scanline, sig_n, sig_w);
    /* Push horizontal blur sigma from TV params. */
    float spot = tv->beam_spot_size;
    if (spot < 1.0f) spot = 1.0f;
    vgc->beam_h_blur_sigma = spot;
    /* Push new physics parameters. */
    vgc->edge_focus = tv->edge_focus;
    vgc->velocity_dim = tv->velocity_dim;
    vgc->motion_threshold = tv->motion_threshold > 0.0f ? tv->motion_threshold : 0.08f;
    /* Frame-sampled exponential decay; channel factors scale lifetime.
     * PAL uses its own frame duration and zero remains a valid off value. */
    int region = vgc->signal_fmt.region;
    vgc->blend_r = signal_persistence_weight(region, tv->persistence_ms, tv->persistence_r);
    vgc->blend_g = signal_persistence_weight(region, tv->persistence_ms, tv->persistence_g);
    vgc->blend_b = signal_persistence_weight(region, tv->persistence_ms, tv->persistence_b);

}

static void gpu_cb_reinit_stages(void) {
    /* Connection type affects which stages are active. Reinit applies the
     * new connection to stage enables, re-derives the notch FIR (S-Video
     * paths use notch, composite uses comb), and updates RF stage params. */
    g_ctx->video_chain->rf.enabled=g_ctx->video_chain->connection==VIDEO_CONN_RF;
    if (*g_ctx->gpu_video_enabled) {
        video_gpu_reinit_stages(g_ctx->video_gpu_chain, g_ctx->video_chain);
        gpu_cb_redesign_firs();  /* S-Video vs composite → different notch */
    }
}

static void gpu_cb_apply_comb(void) {
    /* Comb type changes the comb filter mode and the `comb_active` flag.
     * The comb stage reads its mode from uniform params on each dispatch,
     * and `video_gpu_comb_active()` reads the current mode, so param
     * update is sufficient — no stage enable changes needed. */
    if (*g_ctx->gpu_video_enabled) {
        video_gpu_reinit_stages(g_ctx->video_gpu_chain, g_ctx->video_chain);
    }
}

static void gpu_cb_mask_alignment(void) {
    g_ctx->config->gpu_mask_alignment=g_ctx->render_ctx->mask_alignment;
    mynes_config_save(g_ctx->config);
}

static void gpu_cb_hdr_gain_mode(void) {
    g_ctx->config->gpu_hdr_gain_mode=g_ctx->render_ctx->hdr_gain_mode;
    mynes_config_save(g_ctx->config);
}

static void gpu_cb_panel_primaries(void) {
    g_ctx->config->gpu_panel_primaries=g_ctx->render_ctx->panel_primaries;
    mynes_config_save(g_ctx->config);
}

static void gpu_cb_lab(void) {
    GPURenderCtx *r=g_ctx->render_ctx; MynesConfig *c=g_ctx->config;
    c->gpu_lab_split=r->lab_split;
    for (int i=0;i<3;i++) { c->gpu_lab_gap[i]=(int)lroundf(r->lab_gap[i]*100); c->gpu_lab_gain[i]=(int)lroundf(r->lab_gain[i]*100); }
    c->gpu_lab_fill=(int)lroundf(r->lab_fill*100);
    mynes_config_save(c);
}

static void gpu_cb_panel_subpixels(void) {
    g_ctx->config->gpu_panel_subpixels=g_ctx->render_ctx->panel_subpixels;
    mynes_config_save(g_ctx->config);
}

static void gpu_cb_room_reflections(void) {
    g_ctx->config->gpu_room_reflections=g_ctx->render_ctx->room_reflections_enabled;
    mynes_config_save(g_ctx->config);
    g_ctx->render_ctx->room_reflections_notice=true;
}

void preset_toggle_room_reflections(void) {
    g_ctx->render_ctx->room_reflections_enabled ^= 1;
    gpu_cb_room_reflections();
}

static void gpu_cb_console_reset(void) {
    g_ctx->console_reset_requested = true;
}
static void gpu_cb_fullscreen(void) {
    if(!gpu_output_toggle_fullscreen(g_ctx->render_ctx->window,true))
        fprintf(stderr,"Native fullscreen: %s\n",SDL_GetError());
}

static void gpu_cb_display_bypass(void) {
    g_ctx->render_ctx->display_bypass = g_ctx->display_bypass != 0;
}

static void gpu_cb_audio_backend(void) {
    if (!*g_ctx->gpu_audio_enabled) *g_ctx->use_gpu_audio = 0;
}

static void gpu_cb_audio_prepare(void) {
    AudioChain *ac = g_ctx->audio_chain;
    ac->amp_saturation.enabled = ac->amp_saturation.drive > 1.01f;
    ac->psu_hum.enabled = ac->psu_hum.amplitude > 0;
    ac->noise_floor.enabled = ac->noise_floor.amplitude > 0;
    audio_chain_prepare(ac);
}

static void gpu_cb_apply_region(void) {
    /* The OSD widget cycles osd_region_sel in place; we route that
     * through preset_set_region so the GPU chain, signal tables, and
     * FIRs all land on the new region atomically. Early-exit when the
     * selector matches the live region so re-opening the OSD doesn't
     * thrash the pipeline. */
    if (!g_ctx) return;
    int sel = g_ctx->osd_region_sel;
    if (sel != SIGNAL_REGION_NTSC && sel != SIGNAL_REGION_PAL) {
        g_ctx->osd_region_sel = g_ctx->region;
        return;
    }
    if (sel == g_ctx->region) return;
    preset_set_region(g_ctx, sel);
}

/* ============================================================================
 * Presets scanned from presets/ directory
 * ============================================================================ */

#define PRESET_MAX 63   /* leaves slot 0 in the menu for the "save" action */
static char preset_names[PRESET_MAX][128];
static char preset_paths[PRESET_MAX][512];
static int  preset_count = 0;
static bool preset_user[PRESET_MAX];
static uint32_t catalog_revision = 1;
static PhysicalPreset preset_baseline;
static PhysicalPreset preset_capture_live(void);
static PhysicalPreset preset_loaded;   /* scratch space for loading */
static void preset_refresh_menu(void);

static void preset_remember_active(void) {
    if (!g_ctx || !g_ctx->config) return;
    int index=*g_ctx->current_preset;
    const char *path=index>=0 && index<preset_count ? preset_paths[index] : "";
    const char *base=strrchr(path,'/');
    mynes_config_set_last_preset(g_ctx->config,base ? base+1 : path);
    mynes_config_save(g_ctx->config);
}

static bool preset_load_by_index_mode(int idx, bool preserve_live_region) {
    if (idx < 0 || idx >= preset_count) return false;
    PhysicalPreset candidate;
    if (preset_json_load(&candidate, preset_paths[idx])) {
        preset_loaded = candidate;
        *g_ctx->current_preset = idx;
        printf("Preset: %s — %s\n",
               preset_loaded.name[0] ? preset_loaded.name : preset_names[idx],
               preset_loaded.description[0] ? preset_loaded.description : "");
        if (preserve_live_region) {
            preset_apply(g_ctx, &preset_loaded);
        } else {
            preset_apply_cpu_state(g_ctx, &preset_loaded);
            preset_apply_gpu_push(g_ctx);
        }
    } else {
        fprintf(stderr, "Failed to load preset: %s\n", preset_paths[idx]);
        return false;
    }
    preset_baseline = preset_capture_live();
    preset_remember_active();
    if (preserve_live_region) g_ctx->preset_notice_until=SDL_GetTicks()+3000;
    return true;
}

const char *preset_cycle_notice(void) {
    if (!g_ctx || SDL_GetTicks()>=g_ctx->preset_notice_until) return NULL;
    return preset_display_name(*g_ctx->current_preset);
}

/* OSD action callbacks (OSD needs void(void) function pointers). One per
 * possible preset slot — they all fan out into preset_load_by_index. */
#define PRESET_ACTION(n) static void gpu_action_preset_##n(void) { preset_load_by_index_mode(n, true); }
PRESET_ACTION(0)  PRESET_ACTION(1)  PRESET_ACTION(2)  PRESET_ACTION(3)
PRESET_ACTION(4)  PRESET_ACTION(5)  PRESET_ACTION(6)  PRESET_ACTION(7)
PRESET_ACTION(8)  PRESET_ACTION(9)  PRESET_ACTION(10) PRESET_ACTION(11)
PRESET_ACTION(12) PRESET_ACTION(13) PRESET_ACTION(14) PRESET_ACTION(15)
PRESET_ACTION(16) PRESET_ACTION(17) PRESET_ACTION(18) PRESET_ACTION(19)
PRESET_ACTION(20) PRESET_ACTION(21) PRESET_ACTION(22) PRESET_ACTION(23)
PRESET_ACTION(24) PRESET_ACTION(25) PRESET_ACTION(26) PRESET_ACTION(27)
PRESET_ACTION(28) PRESET_ACTION(29) PRESET_ACTION(30) PRESET_ACTION(31)
PRESET_ACTION(32) PRESET_ACTION(33) PRESET_ACTION(34) PRESET_ACTION(35)
PRESET_ACTION(36) PRESET_ACTION(37) PRESET_ACTION(38) PRESET_ACTION(39)
PRESET_ACTION(40) PRESET_ACTION(41) PRESET_ACTION(42) PRESET_ACTION(43)
PRESET_ACTION(44) PRESET_ACTION(45) PRESET_ACTION(46) PRESET_ACTION(47)
PRESET_ACTION(48) PRESET_ACTION(49) PRESET_ACTION(50) PRESET_ACTION(51)
PRESET_ACTION(52) PRESET_ACTION(53) PRESET_ACTION(54) PRESET_ACTION(55)
PRESET_ACTION(56) PRESET_ACTION(57) PRESET_ACTION(58) PRESET_ACTION(59)
PRESET_ACTION(60) PRESET_ACTION(61) PRESET_ACTION(62)
#undef PRESET_ACTION

static void (*preset_actions[PRESET_MAX])(void) = {
    gpu_action_preset_0,  gpu_action_preset_1,  gpu_action_preset_2,  gpu_action_preset_3,
    gpu_action_preset_4,  gpu_action_preset_5,  gpu_action_preset_6,  gpu_action_preset_7,
    gpu_action_preset_8,  gpu_action_preset_9,  gpu_action_preset_10, gpu_action_preset_11,
    gpu_action_preset_12, gpu_action_preset_13, gpu_action_preset_14, gpu_action_preset_15,
    gpu_action_preset_16, gpu_action_preset_17, gpu_action_preset_18, gpu_action_preset_19,
    gpu_action_preset_20, gpu_action_preset_21, gpu_action_preset_22, gpu_action_preset_23,
    gpu_action_preset_24, gpu_action_preset_25, gpu_action_preset_26, gpu_action_preset_27,
    gpu_action_preset_28, gpu_action_preset_29, gpu_action_preset_30, gpu_action_preset_31,
    gpu_action_preset_32, gpu_action_preset_33, gpu_action_preset_34, gpu_action_preset_35,
    gpu_action_preset_36, gpu_action_preset_37, gpu_action_preset_38, gpu_action_preset_39,
    gpu_action_preset_40, gpu_action_preset_41, gpu_action_preset_42, gpu_action_preset_43,
    gpu_action_preset_44, gpu_action_preset_45, gpu_action_preset_46, gpu_action_preset_47,
    gpu_action_preset_48, gpu_action_preset_49, gpu_action_preset_50, gpu_action_preset_51,
    gpu_action_preset_52, gpu_action_preset_53, gpu_action_preset_54, gpu_action_preset_55,
    gpu_action_preset_56, gpu_action_preset_57, gpu_action_preset_58, gpu_action_preset_59,
    gpu_action_preset_60, gpu_action_preset_61, gpu_action_preset_62,
};

/* Slot 0 is the "Save current as preset…" action; slots 1..preset_count
 * are the actual preset-load actions. So the menu submenu_count is
 * (1 + preset_count). */
static OSDMenuItem menu_presets[PRESET_MAX + 1];

/* Index of the "Presets" sub-menu inside menu_video[]/preset_menu_root[]
 * — used to bump submenu_count after a save so the new preset shows up
 * without restarting. Set during preset_ctx_init. */
static int menu_presets_video_idx = -1;

/* Forward declarations — actual storage lives further down. The save
 * action callback (also further down) needs to update these tables. */
static OSDMenuItem menu_video[15];
OSDMenuItem preset_menu_root[PRESET_MENU_ROOT_MAX]; /* defined below; declared
                                    * here so the save callback can update it. */

/* The "Presets" submenu grows when a user preset is saved. Its root slot can
 * move when feature submenus are inserted, so find it by table pointer. */
static void preset_menu_sync_presets_count(void) {
    for (int i = 0; i < preset_menu_root_count; i++)
        if (preset_menu_root[i].submenu == menu_presets)
            preset_menu_root[i].submenu_count = 1 + preset_count;
}

/* Public entry points used by main.c. */
int  preset_load_index(int idx) {
    if (idx < 0 || idx >= preset_count) return -1;
    return preset_load_by_index_mode(idx, true) ? idx : -1;
}
int  preset_load_index_exact(int idx) {
    if (idx < 0 || idx >= preset_count) return -1;
    return preset_load_by_index_mode(idx, false) ? idx : -1;
}
int  preset_total_count(void) { return preset_count; }
const char *preset_display_name(int idx) {
    if (idx < 0 || idx >= preset_count) return "";
    return preset_names[idx];
}
int preset_find_by_slug(const char *needle) {
    if (!needle || !*needle) return -1;
    for (int i = 0; i < preset_count; i++) {
        if (strstr(preset_paths[i], needle) || strstr(preset_names[i], needle))
            return i;
    }
    return -1;
}

int preset_register_file(const char *path) {
    char resolved[PATH_MAX],other[PATH_MAX];
    PhysicalPreset p;
    if(!realpath(path,resolved) || strlen(resolved)>=sizeof(preset_paths[0]) || !preset_json_load(&p,resolved)) return -1;
    for(int i=0;i<preset_count;i++)
        if(realpath(preset_paths[i],other) && strcmp(resolved,other)==0) return i;
    if(preset_count>=PRESET_MAX) return -1;
    int index=preset_count++;
    snprintf(preset_paths[index],sizeof(preset_paths[index]),"%s",resolved);
    snprintf(preset_names[index],sizeof(preset_names[index]),"%s",p.name[0] ? p.name : "External preset");
    preset_user[index]=false; // It can be duplicated, never overwritten/deleted by library actions.
    preset_refresh_menu();
    return index;
}

/* Slugify "Studio PVM" → "studio_pvm" for safe filenames. */
static void slugify(const char *src, char *dst, int dst_sz) {
    int j = 0;
    for (int i = 0; src[i] && j + 1 < dst_sz; i++) {
        char c = src[i];
        if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
        if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) {
            dst[j++] = c;
        } else if (c == ' ' || c == '_' || c == '-') {
            dst[j++] = '_';
        }
        /* drop everything else (apostrophes, parens, etc.) */
    }
    while (j > 0 && dst[j - 1] == '_') j--;
    dst[j] = '\0';
    if (!*dst) snprintf(dst, dst_sz, "preset");
}

static PhysicalPreset preset_capture_live(void) {
    PhysicalPreset p = preset_loaded;
    p.connection         = g_ctx->video_chain->connection;
    p.comb_type          = g_ctx->video_chain->comb_type;
    p.comb_notch_depth   = g_ctx->video_chain->comb_notch_depth;
    p.tv                 = g_ctx->video_chain->tv;
    p.video_cable        = g_ctx->video_chain->cable;
    p.rf                 = g_ctx->video_chain->rf;
    p.vhs                = g_ctx->video_chain->vhs;
    p.console_coupling_R = g_ctx->video_chain->console_coupling_R;
    p.console_coupling_C = g_ctx->video_chain->console_coupling_C;
    p.console_amp_bw     = g_ctx->video_chain->console_amp_bw;
    p.console_phase_distortion_ns = g_ctx->video_chain->console_phase_distortion_ns;
    p.console_psu_hum    = g_ctx->video_chain->console_psu_hum;
    p.brightness         = g_ctx->sig_state->brightness;
    p.contrast           = g_ctx->sig_state->contrast;
    p.chroma_gain        = g_ctx->sig_state->chroma_gain;
    p.region             = signal_region_normalize(g_ctx->region);
    AudioChain *ac = g_ctx->audio_chain;
    p.audio_saturation_drive = ac->amp_saturation.drive;
    p.audio_psu_hum_amplitude = ac->psu_hum.amplitude;
    p.audio_hum_frequency = ac->psu_hum.frequency;
    p.audio_hum_harmonic_2 = ac->psu_hum.harmonic_2;
    p.audio_hum_harmonic_3 = ac->psu_hum.harmonic_3;
    p.audio_noise_floor = ac->noise_floor.amplitude;

    return p;
}

static void gpu_cb_audio_setup(void) {
    PhysicalPreset p=preset_capture_live();
    apply_audio_preset(g_ctx,&p);
}

/* Save the live VideoChain + signal state as a JSON preset under
 * ~/.config/mynes/presets/<slug>_custom_<timestamp>.json.
 * Writes the resulting path into out_path. Returns true on success. */
static bool preset_save_user(char *out_path, int out_path_sz) {
    if (!g_ctx) return false;

    /* Compose a PhysicalPreset from the currently-loaded preset's
     * metadata + the live tunable state. */
    PhysicalPreset p = preset_capture_live();

    /* Strip any prior "(custom …)" suffix so successive saves don't
     * accumulate timestamps in the name. */
    char base_name[128];
    snprintf(base_name, sizeof(base_name), "%s",
             preset_loaded.name[0] ? preset_loaded.name : "Untitled");
    char *paren = strstr(base_name, " (custom");
    if (paren) *paren = '\0';

    /* Decorate the saved preset's display name + description so it's
     * obvious in the OSD list. */
    time_t now = time(NULL);
    struct tm tmv;
    localtime_r(&now, &tmv);
    char ts_pretty[24], ts_filename[24];
    strftime(ts_pretty,   sizeof(ts_pretty),   "%Y-%m-%d %H:%M", &tmv);
    strftime(ts_filename, sizeof(ts_filename), "%Y%m%d_%H%M%S", &tmv);
    /* The precisions keep the timestamp and the closing quote inside the
     * fixed-size name/description fields even for the longest inputs. */
    snprintf(p.name, sizeof(p.name), "%.90s (custom %s)", base_name, ts_pretty);
    if (preset_loaded.description[0]) {
        snprintf(p.description, sizeof(p.description),
                 "Customised from \"%.127s\" — %.360s",
                 base_name, preset_loaded.description);
    } else {
        snprintf(p.description, sizeof(p.description),
                 "User-saved preset");
    }

    /* Resolve + create ~/.config/mynes/presets/. */
    char dir[MYNES_PATH_MAX];
    mynes_user_presets_dir(dir, sizeof(dir));
    if (!mynes_mkdir_p(dir)) {
        fprintf(stderr, "preset_save_user: cannot create %s\n", dir);
        return false;
    }

    char slug[64];
    slugify(base_name, slug, sizeof(slug));
    if (snprintf(out_path, out_path_sz, "%s/%s_custom_%s.json",
                 dir, slug, ts_filename) >= (int)out_path_sz) {
        fprintf(stderr, "preset_save_user: path too long under %s\n", dir);
        return false;
    }

    if (!preset_json_save(&p, out_path)) {
        fprintf(stderr, "preset_save_user: write failed for %s\n", out_path);
        return false;
    }
    return true;
}

/* OSD action: save current state as a new preset, then add it to the
 * in-memory list so it shows in the Presets submenu without a restart. */
static void gpu_action_save_preset(void) {
    char path[MYNES_PATH_MAX];
    if (!preset_save_user(path, sizeof(path))) return;

    fprintf(stderr, "Saved preset → %s\n", path);

    if (preset_count >= PRESET_MAX) return;  /* in-menu cap reached */

    /* Derive the menu label from the just-written filename (sans .json). */
    const char *base = strrchr(path, '/');
    const char *display = base ? base + 1 : path;
    char name_buf[128];
    snprintf(name_buf, sizeof(name_buf), "%.127s", display);
    char *dot = strrchr(name_buf, '.');
    if (dot) *dot = '\0';

    int slot = preset_count;
    strncpy(preset_names[slot], name_buf, sizeof(preset_names[0]) - 1);
    preset_names[slot][sizeof(preset_names[0]) - 1] = '\0';
    strncpy(preset_paths[slot], path, sizeof(preset_paths[0]) - 1);
    preset_paths[slot][sizeof(preset_paths[0]) - 1] = '\0';
    /* Slot 0 of menu_presets is the Save action; preset entries start at 1.
     * MI_ACTION is defined further down the file, so build the item by
     * hand here. */
    OSDMenuItem mi = {0};
    mi.label  = preset_names[slot];
    mi.type   = OSD_MI_ACTION;
    mi.action = preset_actions[slot];
    menu_presets[1 + slot] = mi;
    preset_user[slot] = true;
    preset_count++;
    catalog_revision++;

    /* Bump the Presets submenu count so the OSD picks up the new entry. */
    if (menu_presets_video_idx >= 0)
        menu_video[menu_presets_video_idx].submenu_count = 1 + preset_count;
    preset_menu_sync_presets_count();
}

uint32_t preset_catalog_revision(void) { return catalog_revision; }
int preset_active_index(void) { return g_ctx ? *g_ctx->current_preset : -1; }
bool preset_is_user(int index) { return index >= 0 && index < preset_count && preset_user[index]; }
bool preset_is_modified(void) {
    if (!g_ctx || preset_active_index() < 0) return true;
    PhysicalPreset p = preset_capture_live();
    size_t start=offsetof(PhysicalPreset,connection);
    return memcmp((const char *)&p+start,(const char *)&preset_baseline+start,sizeof(p)-start)!=0;
}

static void preset_refresh_menu(void) {
    for(int i=0;i<preset_count;i++) {
        OSDMenuItem mi={0}; mi.label=preset_names[i]; mi.type=OSD_MI_ACTION; mi.action=preset_actions[i];
        menu_presets[i+1]=mi;
    }
    if(menu_presets_video_idx>=0) menu_video[menu_presets_video_idx].submenu_count=preset_count+1;
    preset_menu_sync_presets_count();
    catalog_revision++;
    preset_remember_active();
}

static bool preset_write_atomic(const PhysicalPreset *p, const char *path) {
    char temporary[600];
    if(snprintf(temporary,sizeof(temporary),"%s.tmp.XXXXXX",path)>=(int)sizeof(temporary)) return false;
    int fd=mkstemp(temporary); if(fd<0) return false; close(fd);
    bool ok=preset_json_save(p,temporary) && rename(temporary,path)==0;
    if(!ok) unlink(temporary);
    return ok;
}

bool preset_manage(uint32_t op, int index, uint32_t revision,
                   const char *name, char *error, size_t error_size) {
#define REJECT(message) do { snprintf(error,error_size,"%s",message); return false; } while(0)
    if(!g_ctx) REJECT("Preset registry is unavailable.");
    if(revision!=catalog_revision) REJECT("The preset list changed. Please try again.");
    if(op<1 || op>5) REJECT("Unknown preset operation.");
    if(op!=3 && (index<0 || index>=preset_count)) REJECT("Preset no longer exists.");
    if(op==1) {
        if(preset_load_index(index)<0) REJECT("Could not load the preset file.");
        return true;
    }
    if(op!=3 && !preset_is_user(index)) REJECT("Bundled presets are read-only. Save a copy first.");
    if(op==2 && index!=preset_active_index()) REJECT("Only the active preset can be saved.");
    if(op==3 || op==4) {
        if(!name || !name[0] || strlen(name)>100) REJECT("Use a preset name of 1 to 100 UTF-8 bytes.");
        for(const unsigned char *c=(const unsigned char *)name;*c;c++) if(*c<32) REJECT("Preset names cannot contain control characters.");
    }
    if(op==5) {
        if(unlink(preset_paths[index])!=0) REJECT("Could not delete the preset file.");
        int active=preset_active_index();
        for(int i=index;i<preset_count-1;i++) {
            memcpy(preset_names[i],preset_names[i+1],sizeof(preset_names[i]));
            memcpy(preset_paths[i],preset_paths[i+1],sizeof(preset_paths[i]));
            preset_user[i]=preset_user[i+1];
        }
        preset_count--;
        *g_ctx->current_preset=active==index ? -1 : active-(active>index);
        preset_refresh_menu(); return true;
    }
    PhysicalPreset p=preset_capture_live();
    if(op==4 && !preset_json_load(&p,preset_paths[index])) REJECT("Could not read the preset file.");
    if(op==3 || op==4) snprintf(p.name,sizeof(p.name),"%s",name);
    if(op==3) {
        if(preset_count>=PRESET_MAX) REJECT("The preset library is full.");
        char dir[MYNES_PATH_MAX],slug[64],path[512];
        mynes_user_presets_dir(dir,sizeof(dir));
        if(!mynes_mkdir_p(dir)) REJECT("Could not create the user preset directory.");
        slugify(name,slug,sizeof(slug));
        if(snprintf(path,sizeof(path),"%s/%s_XXXXXX.json",dir,slug)>=(int)sizeof(path)) REJECT("Preset path is too long.");
        int fd=mkstemps(path,5); if(fd<0) REJECT("Could not create the preset file."); close(fd);
        if(!preset_write_atomic(&p,path)) { unlink(path); REJECT("Could not save the preset file."); }
        index=preset_count++; preset_user[index]=true;
        snprintf(preset_paths[index],sizeof(preset_paths[index]),"%s",path);
        *g_ctx->current_preset=index; preset_loaded=p; preset_baseline=p;
    } else {
        if(!preset_write_atomic(&p,preset_paths[index])) REJECT("Could not save the preset file.");
        if(index==preset_active_index()) {
            if(op==2) { preset_loaded=p; preset_baseline=p; }
            else snprintf(preset_loaded.name,sizeof(preset_loaded.name),"%s",p.name);
        }
    }
    snprintf(preset_names[index],sizeof(preset_names[index]),"%s",p.name);
    preset_refresh_menu(); return true;
#undef REJECT
}

/* ============================================================================
 * OSD Menu tables (initialized at runtime by preset_ctx_init)
 * ============================================================================ */

/* Leaf menus — organized by signal chain stage.
 * menu_presets is declared above (sized by PRESET_MAX) for the
 * dynamically-scanned preset actions. */
static OSDMenuItem menu_dac[4];           /* Stage 1: DAC / connection / phase */
static OSDMenuItem menu_console[5];       /* Stage 2: console output */
static OSDMenuItem menu_cable[9];         /* Stage 3: cable transmission */
static OSDMenuItem menu_comb[6];          /* Stage 5: separation + display smoothing */
static OSDMenuItem menu_chroma[8];        /* Stage 6-7: chroma demod */
static OSDMenuItem menu_luma[8];          /* Stage 8: luma processing */
static OSDMenuItem menu_color_decode[13]; /* Stage 9: matrix decode */
static OSDMenuItem menu_video_amp[48];     /* Stage 10: video amplifier */
static OSDMenuItem menu_beam[48];         /* Stage 11: electron beam */
static OSDMenuItem menu_phosphor[48];     /* Stage 12: phosphor screen */
static OSDMenuItem menu_glass[48];        /* Stage 13: CRT glass + service geometry */
static OSDMenuItem menu_env[48];           /* Stage 14: environment */
static OSDMenuItem menu_audio_chain[16];
static OSDMenuItem menu_rf[7],menu_vhs[15];

/* Mid-level submenus. menu_video[] + preset_menu_root[] are forward-
 * declared near the top of this file so the save action can reach them. */
static OSDMenuItem menu_audio_top[3];
static OSDMenuItem menu_picture[9], menu_tube[5];
static OSDMenuItem menu_diagnostics[1],menu_display[PRESET_MENU_DISPLAY_MAX],menu_lab[8];
static int  menu_display_count = 8;
static OSDMenuItem menu_game[PRESET_MENU_GAME_MAX];
static int  menu_game_count = 0;
int         preset_menu_root_count = 9;

/* Frontend features (input, saves, pacing) extend the OSD without editing
 * the tables above. Appending re-points the parent submenu so counts stay
 * in sync; the "Game" submenu is inserted before "Reset console" on first use. */
bool preset_menu_root_append(OSDMenuItem item) {
    if (preset_menu_root_count >= PRESET_MENU_ROOT_MAX) return false;
    /* Keep the reset action last so the menu ends with the destructive item. */
    preset_menu_root[preset_menu_root_count] = preset_menu_root[preset_menu_root_count - 1];
    preset_menu_root[preset_menu_root_count - 1] = item;
    preset_menu_root_count++;
    return true;
}
bool preset_menu_game_append(OSDMenuItem item) {
    if (menu_game_count >= PRESET_MENU_GAME_MAX) return false;
    menu_game[menu_game_count++] = item;
    for (int i = 0; i < preset_menu_root_count; i++)
        if (preset_menu_root[i].submenu == menu_game) {
            preset_menu_root[i].submenu_count = menu_game_count;
            return true;
        }
    OSDMenuItem sub = {0};
    sub.label = "Game"; sub.type = OSD_MI_SUBMENU;
    sub.submenu = menu_game; sub.submenu_count = menu_game_count;
    /* Put the game controls first: they are what a player opens the menu for. */
    if (preset_menu_root_count >= PRESET_MENU_ROOT_MAX) return false;
    memmove(&preset_menu_root[1], &preset_menu_root[0],
            sizeof(preset_menu_root[0]) * preset_menu_root_count);
    preset_menu_root[0] = sub;
    preset_menu_root_count++;
    return true;
}
bool preset_menu_display_append(OSDMenuItem item) {
    if (menu_display_count >= PRESET_MENU_DISPLAY_MAX) return false;
    menu_display[menu_display_count++] = item;
    for (int i = 0; i < preset_menu_root_count; i++)
        if (preset_menu_root[i].submenu == menu_display)
            preset_menu_root[i].submenu_count = menu_display_count;
    return true;
}

/* Helper to populate an OSDMenuItem. */
static OSDMenuItem make_item(const char *label, OSDMenuItemType type,
                              void *target, float step, float min_val, float max_val,
                              void (*on_change)(void),
                              const OSDMenuItem *submenu, int submenu_count,
                              void (*action)(void), const char *format) {
    OSDMenuItem m;
    m.label = label;
    m.type = type;
    m.target = target;
    m.step = step;
    m.min_val = min_val;
    m.max_val = max_val;
    m.on_change = on_change;
    m.submenu = submenu;
    m.submenu_count = submenu_count;
    m.action = action;
    m.format = format;
    return m;
}

#define MI_FLOAT(lbl, tgt, stp, lo, hi, cb, fmt) \
    make_item(lbl, OSD_MI_FLOAT, tgt, stp, lo, hi, cb, NULL, 0, NULL, fmt)
#define MI_INT(lbl, tgt, stp, lo, hi, cb, fmt) \
    make_item(lbl, OSD_MI_INT, tgt, stp, lo, hi, cb, NULL, 0, NULL, fmt)
#define MI_CYCLIC(lbl, tgt, lo, hi, cb, fmt) \
    make_item(lbl, OSD_MI_INT_CYCLIC, tgt, 1.0f, lo, hi, cb, NULL, 0, NULL, fmt)
#define MI_TOGGLE(lbl, tgt, cb) \
    make_item(lbl, OSD_MI_TOGGLE, tgt, 1.0f, 0.0f, 1.0f, cb, NULL, 0, NULL, NULL)
#define MI_SUB(lbl, arr, cnt) \
    make_item(lbl, OSD_MI_SUBMENU, NULL, 0,0,0, NULL, arr, cnt, NULL, NULL)
#define MI_ACTION(lbl, fn) \
    make_item(lbl, OSD_MI_ACTION, NULL, 0,0,0, NULL, NULL, 0, fn, NULL)

void preset_ctx_init(PresetCtx *ctx) {
    g_ctx = ctx;
    int n;

    VideoChain *vc = ctx->video_chain;
    AudioChain *ac = ctx->audio_chain;
    SignalPrecompute *sp = ctx->sig_state;

    /* Seed the OSD region selector from whatever region got picked
     * by startup (iNES header flag, --pal flag, or default NTSC). */
    ctx->osd_region_sel = ctx->region;

    /* ================================================================
     * Presets — scanned from presets/ AND ~/.config/mynes/presets/.
     * Slot 0 of menu_presets is reserved for the "Save current..."
     * action; scanned presets occupy slots 1..preset_count.
     * ================================================================ */
    /* Never borrow a different checkout's presets from the process cwd.
     * CMake places resources beside bin/; app bundles may put them in base. */
    char bundled_dir[1024] = {0};
    const char *base = SDL_GetBasePath();
    const char *relative[] = {"../presets", "presets", "../Resources/presets", "../../presets"};
    bool found = false;
    for (size_t i=0; base && i<sizeof(relative)/sizeof(*relative); i++) {
        snprintf(bundled_dir,sizeof(bundled_dir),"%s%s",base,relative[i]);
        struct stat info;
        if (stat(bundled_dir,&info)==0 && S_ISDIR(info.st_mode)) { found=true; break; }
    }
    preset_count = found ? preset_json_scan_dir(bundled_dir,
        preset_names, preset_paths, PRESET_MAX) : 0;
    if (found) fprintf(stderr,"GPU preset library: %s\n",bundled_dir);
    else fprintf(stderr,"GPU preset library missing beside executable: %s\n",base ? base : "unknown");
    int shipped = preset_count;
    /* Present the four actively tuned references first; keep older/user
     * profiles addressable without renaming their persistent identifiers. */
    const char *references[]={"sony_pvm_14l2","jvc_d_series_2000","toshiba_14af43","stass_favourite"};
    int destination=0;
    for(unsigned r=0;r<sizeof(references)/sizeof(references[0]);r++) {
        for(int i=destination;i<shipped;i++) if(strcmp(preset_names[i],references[r])==0) {
            char name[128],path[512];
            memcpy(name,preset_names[i],sizeof(name));memcpy(path,preset_paths[i],sizeof(path));
            memmove(preset_names[destination+1],preset_names[destination],(i-destination)*sizeof(preset_names[0]));
            memmove(preset_paths[destination+1],preset_paths[destination],(i-destination)*sizeof(preset_paths[0]));
            memcpy(preset_names[destination],name,sizeof(name));memcpy(preset_paths[destination],path,sizeof(path));
            destination++;break;
        }
    }

    {
        char user_dir[MYNES_PATH_MAX];
        mynes_user_presets_dir(user_dir, sizeof(user_dir));
        if (preset_count < PRESET_MAX) {
            /* Scan into the tail of the same arrays. */
            int tail = PRESET_MAX - preset_count;
            int extra = preset_json_scan_dir(user_dir,
                (char (*)[128])preset_names[preset_count],
                (char (*)[512])preset_paths[preset_count], tail);
            preset_count += extra;
        }
    }

    for (int i=0;i<preset_count;i++) {
        preset_user[i] = i >= shipped;
        PhysicalPreset metadata;
        if(preset_json_load(&metadata,preset_paths[i]) && metadata.name[0])
            snprintf(preset_names[i],sizeof(preset_names[i]),"%s",metadata.name);
    }

    menu_presets[0] = MI_ACTION("Save current as preset...",
                                gpu_action_save_preset);
    for (int i = 0; i < preset_count; i++)
        menu_presets[1 + i] = MI_ACTION(preset_names[i], preset_actions[i]);
    fprintf(stderr, "Found %d preset(s) (%d shipped, %d user)\n",
            preset_count, shipped, preset_count - shipped);

    /* ================================================================
     * Stage 1: DAC — connection type and subcarrier phase
     * ================================================================ */
    n = 0;
    menu_dac[n++] = MI_CYCLIC("Region",        &ctx->osd_region_sel, 0.0f, 1.0f, gpu_cb_apply_region, "NTSC|PAL");
    menu_dac[n++] = MI_CYCLIC("Connection",    &vc->connection, 0.0f, (float)(VIDEO_CONN_COUNT-1), gpu_cb_reinit_stages, "RF|Composite|S-Video|Component|RGB|Direct");
    menu_dac[n++] = MI_CYCLIC("Base phase",    &sp->phase_base,       0.0f, 11.0f, gpu_cb_update_color_matrix, "%d / 12");
    menu_dac[n++] = MI_INT("Line advance",     &sp->phase_line_adv,   1.0f, -12.0f, 12.0f, gpu_cb_update_color_matrix, "%+d");
    const int menu_dac_count=n;

    /* ================================================================
     * Stage 2: Console output — coupling, amp bandwidth, PSU
     * ================================================================ */
    n = 0;
    /* Output R: NES output impedance resistor on motherboard. Adds to total
     * series resistance → affects cable bandwidth via R*C time constant. */
    menu_console[n++] = MI_FLOAT("Output R",    &vc->console_coupling_R, 5.0f, 10.0f, 200.0f, gpu_cb_update_rc_params, "%.0f");
    /* PSU hum: AC supply leaking into video via poor regulation. */
    menu_console[n++] = MI_FLOAT("PSU hum",     &vc->console_psu_hum,    0.01f, 0.0f, 0.30f, gpu_cb_update_rc_params, "%.3f");
    menu_console[n++] = MI_FLOAT("Video bandwidth", &vc->console_amp_bw, 0.25e6f, 1e6f, 12e6f, gpu_cb_update_rc_params, "%.0f");
    menu_console[n++] = MI_FLOAT("PPU phase RC (ns)", &vc->console_phase_distortion_ns, 1, 0, 60, gpu_cb_update_rc_params, "%.0f");
    const int menu_console_count=n;

    /* ================================================================
     * Stage 3: Cable — transmission line parameters
     * ================================================================ */
    n = 0;
    menu_cable[n++] = MI_FLOAT("Length (m)",    &vc->cable.length_meters,       0.25f, 0.5f, 10.0f, gpu_cb_update_rc_params, "%.2f");
    menu_cable[n++] = MI_FLOAT("R/m",           &vc->cable.resistance_per_m,    0.1f, 0.0f, 5.0f, gpu_cb_update_rc_params, "%.2f");
    menu_cable[n++] = MI_FLOAT("C/m (F)",      &vc->cable.capacitance_per_m,   5e-12f, 10e-12f, 200e-12f, gpu_cb_update_rc_params, "%.1e");
    menu_cable[n++] = MI_FLOAT("Connector R",   &vc->cable.connector_resistance, 0.1f, 0.0f, 5.0f, gpu_cb_update_rc_params, "%.2f");
    menu_cable[n++] = MI_FLOAT("Shield eff",    &vc->cable.shield_effectiveness, 0.05f, 0.0f, 1.0f, gpu_cb_update_rc_params, "%.2f");
    menu_cable[n++] = MI_FLOAT("Ghost level",   &vc->cable.ghost_level,          0.01f, 0.0f, 0.20f, gpu_cb_update_rc_params, "%.2f");
    menu_cable[n++] = MI_INT("Ghost delay",     &vc->cable.ghost_delay,          2.0f, 0.0f, 40.0f, gpu_cb_update_rc_params, "%d");
    menu_cable[n++] = MI_FLOAT("Termination (ohm)", &vc->cable.impedance, 5, 10, 300, gpu_cb_update_rc_params, "%.0f");
    const int menu_cable_count=n;

    /* ================================================================
     * Stage 5: Y/C separation and optional display-frame smoothing
     * ================================================================ */
    n = 0;
    /* 0=NONE, 1=1LINE, 2=2LINE, 3=3LINE, 4=BYPASS */
    menu_comb[n++] = MI_CYCLIC("NTSC comb",        &vc->comb_type,  0.0f, 4.0f, gpu_cb_apply_comb, "Notch|2 lines (1H)|Adaptive 3|3 lines (2H)|Bypass");
    /* Fraction of extracted C; zero selects the legacy topology default.
     * This is not a horizontal trap and unity does not guarantee perfect
     * separation. Applies only when a line-comb topology is selected. */
    menu_comb[n++] = MI_FLOAT("Comb C (0=auto)",     &vc->comb_notch_depth, 0.05f, 0.0f, 1.0f, gpu_cb_reinit_stages, "%.2f");
    menu_comb[n++] = MI_FLOAT("Temporal blend",     &ctx->video_gpu_chain->temporal_blend, 0.05f, 0.0f, 0.5f, gpu_cb_update_beam_params, "%.2f");
    menu_comb[n++] = MI_FLOAT("Motion threshold",   &vc->tv.motion_threshold, 0.01f, 0.0f, 0.30f, gpu_cb_update_beam_params, "%.2f");
    menu_comb[n++] = MI_FLOAT("H AFC (ms, 0=auto)", &vc->tv.h_afc_tau_ms, 0.1f, 0.0f, 10.0f, gpu_cb_update_rc_params, "%.2f");
    const int menu_comb_count=n;

    /* ================================================================
     * Stage 6-7: Chroma demodulation
     * ================================================================ */
    n = 0;
    menu_chroma[n++] = MI_FLOAT("I BW (wide)",    &vc->tv.chroma_bandwidth, 50000.0f,  100000.0f, 2000000.0f, gpu_cb_redesign_firs, "%.0f");
    /* Q bandwidth: NTSC spec = 0.5 MHz (narrower than 1.3 MHz I). Zero
     * here means "equi-band with I", which matches cheap consumer sets
     * and the pre-split pipeline default. */
    menu_chroma[n++] = MI_FLOAT("Q BW (0=I)",  &vc->tv.chroma_q_bandwidth, 50000.0f, 0.0f, 2000000.0f, gpu_cb_redesign_firs, "%.0f");
    menu_chroma[n++] = MI_INT("I taps",            &sp->fir_c_n, 2.0f, 5.0f, 63.0f, gpu_cb_redesign_firs, "%d");
    menu_chroma[n++] = MI_INT("Q taps",            &sp->fir_q_n, 2.0f, 5.0f, 63.0f, gpu_cb_redesign_firs, "%d");
    menu_chroma[n++] = MI_FLOAT("Chroma gain",     &sp->chroma_gain,  0.10f, 0.0f, 5.0f, gpu_cb_update_color_matrix, "%.2f");
    menu_chroma[n++] = MI_CYCLIC("Demod rotate",   &sp->demod_rotate, -12.0f, 12.0f, gpu_cb_update_color_matrix, "%+d / 12");
    menu_chroma[n++] = MI_FLOAT("Color killer",    &vc->tv.color_killer, 0.01f, 0.0f, 0.30f, gpu_cb_update_color_matrix, "%.2f");
    /* Chroma FIR window ringing: 0=Hamming (smooth), 1=rect (Gibbs overshoot). */
    menu_chroma[n++] = MI_FLOAT("Ringing",         &vc->tv.fir_ringing, 0.05f, 0.0f, 1.0f, gpu_cb_redesign_firs, "%.2f");
    const int menu_chroma_count=n;

    /* ================================================================
     * Stage 8: Luma processing
     * ================================================================ */
    n = 0;
    /* Luma BW below ~1.5 MHz causes heavy ringing (long sinc tail).
     * Luma taps below 23 can't form a clean subcarrier notch (12-sample
     * period), which is exactly the regime that produces visible dot
     * crawl + cross-color fringing — desirable for consumer-TV looks. */
    menu_luma[n++] = MI_FLOAT("Luma BW",      &vc->tv.luma_bandwidth,   100000.0f, 1500000.0f, 6000000.0f, gpu_cb_redesign_firs, "%.0f");
    menu_luma[n++] = MI_INT("Luma taps",       &sp->fir_y_n, 2.0f, 5.0f, 63.0f, gpu_cb_redesign_firs, "%d");
    menu_luma[n++] = MI_FLOAT("Sharpness",    &vc->tv.luma_peaking, 0.05f, 0.0f, 1.0f, gpu_cb_redesign_firs, "%.2f");
    /* Horizontal trap — reduces cross-luma; 0 = off.
     * Requires Luma taps ≥ 23 to take effect. */
    menu_luma[n++] = MI_FLOAT("Notch depth",  &vc->tv.luma_notch_depth, 0.05f, 0.0f, 1.0f, gpu_cb_redesign_firs, "%.2f");
    menu_luma[n++] = MI_FLOAT("Brightness",    &sp->brightness,     0.02f, -1.0f, 1.0f, gpu_cb_update_color_matrix, "%+.2f");
    menu_luma[n++] = MI_FLOAT("Contrast",      &sp->contrast,       0.02f, 0.1f, 3.0f, gpu_cb_update_color_matrix, "%.2f");
    menu_luma[n++] = MI_FLOAT("Aperture max dB (0=auto)", &vc->tv.aperture_max_db, 0.5f, 0.0f, 12.0f, gpu_cb_redesign_firs, "%.1f");
    const int menu_luma_count=n;

    /* ================================================================
     * Stage 9: Color decode — YIQ to RGB matrix
     * ================================================================ */
    n = 0;
    menu_color_decode[n++] = MI_FLOAT("Hue",         &vc->tv.hue_offset,        2.0f, -45.0f, 45.0f, gpu_cb_update_color_matrix, "%+.1f");
    menu_color_decode[n++] = MI_FLOAT("Saturation",  &vc->tv.saturation,        0.05f, 0.0f, 3.0f, gpu_cb_update_color_matrix, "%.2f");
    menu_color_decode[n++] = MI_FLOAT("Color temp",  &vc->tv.color_temperature, 100.0f, 3200.0f, 9300.0f, gpu_cb_update_color_matrix, "%.0f");
    menu_color_decode[n++] = MI_FLOAT("R drive",     &vc->tv.r_drive,           0.02f, 0.5f, 1.5f, gpu_cb_update_color_matrix, "%.2f");
    menu_color_decode[n++] = MI_FLOAT("G drive",     &vc->tv.g_drive,           0.02f, 0.5f, 1.5f, gpu_cb_update_color_matrix, "%.2f");
    menu_color_decode[n++] = MI_FLOAT("B drive",     &vc->tv.b_drive,           0.02f, 0.5f, 1.5f, gpu_cb_update_color_matrix, "%.2f");
    menu_color_decode[n++] = MI_FLOAT("R cutoff",    &vc->tv.r_cutoff,          0.005f, -0.1f, 0.1f, gpu_cb_update_color_matrix, "%+.3f");
    menu_color_decode[n++] = MI_FLOAT("G cutoff",    &vc->tv.g_cutoff,          0.005f, -0.1f, 0.1f, gpu_cb_update_color_matrix, "%+.3f");
    menu_color_decode[n++] = MI_FLOAT("B cutoff",    &vc->tv.b_cutoff,          0.005f, -0.1f, 0.1f, gpu_cb_update_color_matrix, "%+.3f");
    menu_color_decode[n++] = MI_FLOAT("R-Y gain offset", &vc->tv.decoder_red_gain, 0.02f, -0.5f, 0.5f, gpu_cb_update_color_matrix, "%+.2f");
    menu_color_decode[n++] = MI_FLOAT("B-Y gain offset", &vc->tv.decoder_blue_gain, 0.02f, -0.5f, 0.5f, gpu_cb_update_color_matrix, "%+.2f");
    menu_color_decode[n++] = MI_CYCLIC("Phosphor primaries", &vc->tv.phosphor_gamut, 0, 3, gpu_cb_update_color_matrix, "709 legacy|525 nominal|625 nominal|FW900 NIDL");
    const int menu_color_decode_count=n;

    /* ================================================================
     * Stage 10: Video amplifier — per-gun bandwidth + gamma
     * ================================================================ */
    n = 0;
    menu_video_amp[n++] = MI_FLOAT("R bandwidth",  &vc->tv.r_bandwidth,  100000.0f, 2000000.0f, 10000000.0f, gpu_cb_reinit_stages, "%.0f");
    menu_video_amp[n++] = MI_FLOAT("G bandwidth",  &vc->tv.g_bandwidth,  100000.0f, 2000000.0f, 10000000.0f, gpu_cb_reinit_stages, "%.0f");
    menu_video_amp[n++] = MI_FLOAT("B bandwidth",  &vc->tv.b_bandwidth,  100000.0f, 2000000.0f, 10000000.0f, gpu_cb_reinit_stages, "%.0f");
    menu_video_amp[n++] = MI_FLOAT("Gamma",        &vc->tv.gamma,        0.02f, 1.5f, 2.8f, gpu_cb_update_color_matrix, "%.2f");
    menu_video_amp[n++] = MI_FLOAT("Edge derivative", &vc->tv.velocity_mod, 0.01f, 0.0f, 1.0f, gpu_cb_update_beam_params, "%.3f");
    menu_video_amp[n++] = MI_FLOAT("Rise/fall asymmetry", &vc->tv.asym_rise_fall, 0.01f, 0.0f, 1.0f, gpu_cb_update_beam_params, "%.3f");
    menu_video_amp[n++] = MI_FLOAT("Vertical smear", &vc->tv.vertical_smear, 0.01f, 0.0f, 1.0f, gpu_cb_update_beam_params, "%.3f");
    menu_video_amp[n++] = MI_CYCLIC("Bandwidth definition", &vc->tv.rgb_bandwidth_3db, 0, 1, gpu_cb_reinit_stages, "Legacy cutoff|-3 dB");
    const int menu_video_amp_count=n;

    /* ================================================================
     * Stage 11: Electron beam — spot profile, bloom, convergence, jitter
     * ================================================================ */
    n = 0;
    menu_beam[n++] = MI_CYCLIC("Tube model", &vc->tv.monitor_model, 0, 1, gpu_cb_update_color_matrix, "240-line TV|FW900 + scaler");
    menu_beam[n++] = MI_FLOAT("Dark FWHM (lines)", &vc->tv.beam_fwhm_min, 0.02f, 0.0f, 2.35f, gpu_cb_update_beam_params, "%.2f");
    menu_beam[n++] = MI_FLOAT("White FWHM (lines)", &vc->tv.beam_fwhm_max, 0.02f, 0.0f, 2.35f, gpu_cb_update_beam_params, "%.2f");
    menu_beam[n++] = MI_FLOAT("Spot size",         &vc->tv.beam_spot_size,       0.5f, 1.0f, 16.0f, gpu_cb_update_beam_params, "%.1f");
    menu_beam[n++] = MI_FLOAT("Bloom gamma",       &vc->tv.bloom_gamma,          0.1f, 1.0f, 3.0f, gpu_cb_update_beam_params, "%.1f");
    menu_beam[n++] = MI_FLOAT("Edge focus",        &vc->tv.edge_focus,           0.02f, 0.0f, 0.5f, gpu_cb_update_beam_params, "%.2f");
    menu_beam[n++] = MI_FLOAT("Velocity dim",      &vc->tv.velocity_dim,         0.02f, 0.0f, 0.3f, gpu_cb_update_beam_params, "%.2f");
    /* Edge-blanking / burst-lock controls — applied at CPU waveform
     * generation time (waveform_apply_beam_edges). No GPU re-init
     * needed, so the cheap update-beam-params callback is enough. */
    menu_beam[n++] = MI_FLOAT("Edge fade px",      &vc->tv.beam_edge_fade,       0.1f, 0.0f, 4.0f, gpu_cb_update_beam_params, "%.1f");
    menu_beam[n++] = MI_FLOAT("Edge overshoot",    &vc->tv.beam_edge_overshoot,  0.02f, 0.0f, 0.5f, gpu_cb_update_beam_params, "%.2f");
    menu_beam[n++] = MI_FLOAT("Burst drift °",     &vc->tv.burst_lock_drift,     1.0f, -30.0f, 30.0f, gpu_cb_update_beam_params, "%+.0f");
    menu_beam[n++] = MI_FLOAT("Burst drift px",    &vc->tv.burst_lock_drift_width, 0.5f, 0.0f, 24.0f, gpu_cb_update_beam_params, "%.1f");
    /* Causal video-amplifier supply loading and DC-restoration recovery. */
    menu_beam[n++] = MI_FLOAT("Video rail sag", &vc->tv.beam_current_load,    0.01f, 0.0f, 0.30f, gpu_cb_update_beam_params, "%.2f");
    menu_beam[n++] = MI_FLOAT("Horizontal streaks", &vc->tv.video_black_droop, 0.01f, 0.0f, 0.5f, gpu_cb_update_beam_params, "%.2f");
    menu_beam[n++] = MI_FLOAT("Recovery (us)", &vc->tv.video_recovery_us, 2.0f, 1.0f, 100.0f, gpu_cb_update_beam_params, "%.0f");
    /* Convergence offsets move gun landing positions before the mask. */
    menu_beam[n++] = MI_FLOAT("Static convergence", &vc->tv.convergence_static,  0.05f, 0.0f, 2.0f, gpu_cb_update_beam_params, "%.2f");
    menu_beam[n++] = MI_FLOAT("Edge convergence", &vc->tv.convergence_dynamic,  0.02f, 0.0f, 2.0f, gpu_cb_update_beam_params, "%.2f");
    menu_beam[n++] = MI_FLOAT("Conv R X",          &vc->tv.conv_r_x,            0.5f, -10.0f, 10.0f, gpu_cb_update_beam_params, "%+.1f");
    menu_beam[n++] = MI_FLOAT("Conv R Y",          &vc->tv.conv_r_y,            0.5f, -5.0f, 5.0f, gpu_cb_update_beam_params, "%+.1f");
    menu_beam[n++] = MI_FLOAT("Conv B X",          &vc->tv.conv_b_x,            0.5f, -10.0f, 10.0f, gpu_cb_update_beam_params, "%+.1f");
    menu_beam[n++] = MI_FLOAT("Conv B Y",          &vc->tv.conv_b_y,            0.5f, -5.0f, 5.0f, gpu_cb_update_beam_params, "%+.1f");
    menu_beam[n++] = MI_FLOAT("H jitter",          &vc->tv.h_jitter,             0.01f, 0.0f, 1.0f, gpu_cb_update_beam_params, "%.3f");
    menu_beam[n++] = MI_FLOAT("V jitter",          &vc->tv.v_jitter,             0.01f, 0.0f, 1.0f, gpu_cb_update_beam_params, "%.3f");
    menu_beam[n++] = MI_FLOAT("Hum bar",           &vc->tv.hum_bar_amplitude,    0.01f, 0.0f, 0.20f, gpu_cb_update_beam_params, "%.2f");
    menu_beam[n++] = MI_FLOAT("Interference jitter",   &vc->tv.rf_interference,      0.5f, 0.0f, 5.0f, gpu_cb_update_beam_params, "%.1f");
    menu_beam[n++] = MI_FLOAT("Geometry warp",     &vc->tv.geometry_warp,        0.2f, 0.0f, 5.0f, gpu_cb_update_beam_params, "%.1f");
    menu_beam[n++] = MI_FLOAT("Top band shift",    &vc->tv.top_band_shift,       0.2f, -12.0f, 12.0f, gpu_cb_update_beam_params, "%+.1f");
    menu_beam[n++] = MI_FLOAT("Top edge skew",     &vc->tv.top_edge_skew,        0.2f, -12.0f, 12.0f, gpu_cb_update_beam_params, "%+.1f");
    menu_beam[n++] = MI_FLOAT("Top band start",    &vc->tv.top_band_start,       1.0f, 0.0f, 239.0f, gpu_cb_update_beam_params, "%.0f");
    menu_beam[n++] = MI_FLOAT("Top band end",      &vc->tv.top_band_end,         1.0f, 0.0f, 239.0f, gpu_cb_update_beam_params, "%.0f");
    menu_beam[n++] = MI_FLOAT("Top edge width",    &vc->tv.top_edge_width,       0.01f, 0.01f, 0.30f, gpu_cb_update_beam_params, "%.2f");
    menu_beam[n++] = MI_FLOAT("Load focus change",   &vc->tv.focus_breathing,      0.02f, 0.0f, 0.3f, gpu_cb_update_beam_params, "%.2f");
    menu_beam[n++] = MI_FLOAT("Scanline wobble",   &vc->tv.scanline_wobble,      0.05f, 0.0f, 1.0f, gpu_cb_update_beam_params, "%.2f");
    menu_beam[n++] = MI_FLOAT("H spot growth", &vc->tv.beam_spot_growth, 0.05f, 0, 1, gpu_cb_update_beam_params, "%.2f");
    menu_beam[n++] = MI_FLOAT("Corner astigmatism", &vc->tv.corner_astigmatism, 0.01f, 0.0f, 1.0f, gpu_cb_update_beam_params, "%.3f");
    menu_beam[n++] = MI_FLOAT("Legacy focus (FWHM=0)", &vc->tv.beam_sharpness, 0.05f, 0.0f, 2.0f, gpu_cb_update_beam_params, "%.3f");
    menu_beam[n++] = MI_FLOAT("Legacy dark height", &vc->tv.beam_height_min, 0.05f, 0.0f, 2.0f, gpu_cb_update_beam_params, "%.3f");
    menu_beam[n++] = MI_FLOAT("Legacy white height", &vc->tv.beam_height_max, 0.05f, 0.0f, 2.0f, gpu_cb_update_beam_params, "%.3f");
    const int menu_beam_count=n;

    /* ================================================================
     * Stage 12: Phosphor screen — mask, persistence, subpixel
     * ================================================================ */
    n = 0;
    menu_phosphor[n++] = MI_CYCLIC("Mask type",      &vc->tv.mask_type, 0.0f, 2.0f, gpu_cb_update_beam_params, "Shadow|Grille|Slot");
    menu_phosphor[n++] = MI_FLOAT("Mask triads (0=pixels)", &vc->tv.mask_triads, 10.0f, 0.0f, 2000.0f, NULL, "%.0f");
    menu_phosphor[n++] = MI_FLOAT("Pitch (triads=0)",   &vc->tv.mask_pitch_px,       0.5f, 1.0f, 20.0f, gpu_cb_update_beam_params, "%.1f");
    menu_phosphor[n++] = MI_FLOAT("Mask strength",   &vc->tv.mask_strength,       0.05f, 0.0f, 1.0f, gpu_cb_update_beam_params, "%.2f");
    /* Lifetimes are estimated exponential constants, not a universal P22 specification. */
    menu_phosphor[n++] = MI_FLOAT("Persistence ms",  &vc->tv.persistence_ms,      1.0f, 0.0f, 100.0f, gpu_cb_update_beam_params, "%.1f");
    menu_phosphor[n++] = MI_FLOAT("Persist R",       &vc->tv.persistence_r,       0.02f, 0.0f, 2.0f, gpu_cb_update_beam_params, "%.2f");
    menu_phosphor[n++] = MI_FLOAT("Persist G",       &vc->tv.persistence_g,       0.02f, 0.0f, 2.0f, gpu_cb_update_beam_params, "%.2f");
    menu_phosphor[n++] = MI_FLOAT("Persist B",       &vc->tv.persistence_b,       0.02f, 0.0f, 2.0f, gpu_cb_update_beam_params, "%.2f");
    menu_phosphor[n++] = MI_CYCLIC("Phosphor order",       &vc->tv.subpixel_layout,     0.0f, 2.0f, gpu_cb_update_beam_params, "RGB (legacy)|RGB|BGR");
    menu_phosphor[n++] = MI_FLOAT("Face height mm", &vc->tv.face_height_mm, 1.0f, 0.0f, 600.0f, gpu_cb_update_beam_params, "%.1f");
    menu_phosphor[n++] = MI_CYCLIC("Damper wires", &vc->tv.damper_wires, 0.0f, 2.0f, gpu_cb_update_beam_params, "None|One|Two");
    menu_phosphor[n++] = MI_FLOAT("Damper 1 height", &vc->tv.damper_y1, 0.005f, 0.0f, 1.0f, gpu_cb_update_beam_params, "%.3f");
    menu_phosphor[n++] = MI_FLOAT("Damper 2 height", &vc->tv.damper_y2, 0.005f, 0.0f, 1.0f, gpu_cb_update_beam_params, "%.3f");
    menu_phosphor[n++] = MI_FLOAT("Damper wire um", &vc->tv.damper_wire_um, 1.0f, 0.0f, 100.0f, gpu_cb_update_beam_params, "%.0f");
    menu_phosphor[n++] = MI_FLOAT("Slow decay ms", &vc->tv.persistence_tail_ms, 1.0f, 0.0f, 100.0f, gpu_cb_update_beam_params, "%.3f");
    menu_phosphor[n++] = MI_FLOAT("Slow decay energy", &vc->tv.persistence_tail_weight, 0.01f, 0.0f, 1.0f, gpu_cb_update_beam_params, "%.3f");
    menu_phosphor[n++] = MI_FLOAT("R gamma offset", &vc->tv.phosphor_gamma_offset_r, 0.01f, -0.5f, 0.5f, gpu_cb_update_beam_params, "%.3f");
    menu_phosphor[n++] = MI_FLOAT("G gamma offset", &vc->tv.phosphor_gamma_offset_g, 0.01f, -0.5f, 0.5f, gpu_cb_update_beam_params, "%.3f");
    menu_phosphor[n++] = MI_FLOAT("B gamma offset", &vc->tv.phosphor_gamma_offset_b, 0.01f, -0.5f, 0.5f, gpu_cb_update_beam_params, "%.3f");
    menu_phosphor[n++] = MI_FLOAT("Cross excitation", &vc->tv.secondary_scatter, 0.01f, 0.0f, 1.0f, gpu_cb_update_beam_params, "%.3f");
    menu_phosphor[n++] = MI_FLOAT("Drive color shift (generic)", &vc->tv.chromaticity_drive_shift, 0.01f, 0.0f, 1.0f, gpu_cb_update_beam_params, "%.3f");
    menu_phosphor[n++] = MI_FLOAT("Phosphor grain", &vc->tv.phosphor_grain, 0.01f, 0.0f, 0.3f, gpu_cb_update_beam_params, "%.3f");
    const int menu_phosphor_count=n;

    /* ================================================================
     * Stage 13: CRT glass — halation, tint, barrel distortion
     * ================================================================ */
    n = 0;
    menu_glass[n++] = MI_FLOAT("Halation",      &vc->tv.halation,    0.02f, 0.0f, 0.4f, gpu_cb_update_beam_params, "%.2f");
    menu_glass[n++] = MI_FLOAT("Halo width / height (0=auto)", &vc->tv.halation_sigma, 0.001f, 0.0f, 0.05f, gpu_cb_update_beam_params, "%.3f");
    menu_glass[n++] = MI_FLOAT("Halo tint R",   &vc->tv.halation_tint_r, 0.05f, 0.0f, 2.0f, gpu_cb_update_beam_params, "%.2f");
    menu_glass[n++] = MI_FLOAT("Halo tint G",   &vc->tv.halation_tint_g, 0.05f, 0.0f, 2.0f, gpu_cb_update_beam_params, "%.2f");
    menu_glass[n++] = MI_FLOAT("Halo tint B",   &vc->tv.halation_tint_b, 0.05f, 0.0f, 2.0f, gpu_cb_update_beam_params, "%.2f");
    menu_glass[n++] = MI_FLOAT("Glass tint",    &vc->tv.glass_tint,  0.02f, 0.0f, 2.0f, gpu_cb_update_beam_params, "%.2f");
    menu_glass[n++] = MI_FLOAT("Barrel H",      &vc->tv.barrel,      0.005f, 0.0f, 0.15f, gpu_cb_update_beam_params, "%.3f");
    menu_glass[n++] = MI_FLOAT("Barrel V",      &vc->tv.barrel_v,    0.005f, 0.0f, 0.15f, gpu_cb_update_beam_params, "%.3f");
    menu_glass[n++] = MI_FLOAT("Keystone",      &vc->tv.keystone,    0.005f, -0.1f, 0.1f, gpu_cb_update_beam_params, "%+.3f");
    menu_glass[n++] = MI_FLOAT("Rotation",      &vc->tv.rotation,    0.002f, -0.05f, 0.05f, gpu_cb_update_beam_params, "%+.3f");
    menu_glass[n++] = MI_FLOAT("Skew X",        &vc->tv.skew_x,      0.005f, -0.1f, 0.1f, gpu_cb_update_beam_params, "%+.3f");
    menu_glass[n++] = MI_FLOAT("Skew Y",        &vc->tv.skew_y,      0.005f, -0.1f, 0.1f, gpu_cb_update_beam_params, "%+.3f");
    menu_glass[n++] = MI_FLOAT("Size sag (+shrink)", &vc->tv.hv_sag, 0.02f, -0.5f, 0.5f, gpu_cb_update_beam_params, "%.2f");
    /* Service-menu raster controls (CRT HPOS/VPOS/HSIZE/VSIZE).
     * HSIZE/VSIZE < 1.0 shrinks the image inside the tube (you see the
     * physical beam edge); > 1.0 overscans off the visible tube face. */
    menu_glass[n++] = MI_FLOAT("H position",    &vc->tv.h_pos,       0.01f, -0.3f, 0.3f, gpu_cb_update_beam_params, "%+.2f");
    menu_glass[n++] = MI_FLOAT("V position",    &vc->tv.v_pos,       0.01f, -0.3f, 0.3f, gpu_cb_update_beam_params, "%+.2f");
    menu_glass[n++] = MI_FLOAT("H size",        &vc->tv.h_size,      0.02f, 0.5f, 1.5f, gpu_cb_update_beam_params, "%.2f");
    menu_glass[n++] = MI_FLOAT("V size",        &vc->tv.v_size,      0.02f, 0.5f, 1.5f, gpu_cb_update_beam_params, "%.2f");
    /* External room light is optional; retain the preset's strengths while off. */
    menu_glass[n++] = MI_TOGGLE("Room reflections (G)", &ctx->render_ctx->room_reflections_enabled, gpu_cb_room_reflections);
    menu_glass[n++] = MI_FLOAT("Glare amount",  &vc->tv.glass_glare,         0.001f, 0.0f, 1.00f, gpu_cb_update_beam_params, "%.3f");
    menu_glass[n++] = MI_FLOAT("Glare light X", &vc->tv.glass_glare_light_x, 0.05f, 0.0f, 1.00f, gpu_cb_update_beam_params, "%.2f");
    menu_glass[n++] = MI_FLOAT("Glare light Y", &vc->tv.glass_glare_light_y, 0.05f, 0.0f, 1.00f, gpu_cb_update_beam_params, "%.2f");
    menu_glass[n++] = MI_FLOAT("Glare size",    &vc->tv.glass_glare_size,    0.02f, 0.02f, 0.60f, gpu_cb_update_beam_params, "%.2f");
    menu_glass[n++] = MI_FLOAT("Glare temp (K)",&vc->tv.glass_glare_temp_k,  200.0f, 0.0f, 10000.0f, gpu_cb_update_beam_params, "%.0f");
    menu_glass[n++] = MI_FLOAT("Internal scatter", &vc->tv.glass_reflection, 0.02f, 0.0f, 1.0f, gpu_cb_update_beam_params, "%.3f");
    menu_glass[n++] = MI_FLOAT("Matte scatter", &vc->tv.antiglare_blur, 0.02f, 0.0f, 1.0f, gpu_cb_update_beam_params, "%.3f");
    menu_glass[n++] = MI_FLOAT("Overscan", &vc->tv.overscan, 0.005f, 0.0f, 0.1f, gpu_cb_update_beam_params, "%.3f");
    const int menu_glass_count=n;

    /* ================================================================
     * Stage 14: Environment — vignette, ambient, noise, output
     * ================================================================ */
    n = 0;
    menu_env[n++] = MI_FLOAT("Vignette",      &vc->tv.vignette,       0.02f, 0.0f, 0.4f, gpu_cb_update_beam_params, "%.2f");
    menu_env[n++] = MI_FLOAT("Ambient",        &vc->tv.ambient_light,  0.01f, 0.0f, 0.25f, gpu_cb_update_beam_params, "%.2f");
    menu_env[n++] = MI_FLOAT("Gun black level",    &vc->tv.black_floor,    0.005f, 0.0f, 0.10f, gpu_cb_update_beam_params, "%.3f");
    menu_env[n++] = MI_FLOAT("Receiver noise",    &vc->tv.noise_level,    0.005f, 0.0f, 0.10f, gpu_cb_update_beam_params, "%.3f");
    menu_env[n++] = MI_FLOAT("HDR emission gain",   &vc->tv.hdr_gain,       0.1f, 0.5f, 3.0f, gpu_cb_update_beam_params, "%.1f");
    menu_env[n++] = MI_FLOAT("EMI gradient", &vc->tv.emi_gradient, 0.02f, 0.0f, 1.0f, gpu_cb_update_beam_params, "%.3f");
    menu_env[n++] = MI_FLOAT("Purity error", &vc->tv.degauss_tint, 0.02f, 0.0f, 1.0f, gpu_cb_update_beam_params, "%.3f");
    menu_env[n++] = MI_FLOAT("Center cathode wear", &vc->tv.cathode_center_dim, 0.02f, 0.0f, 0.5f, gpu_cb_update_beam_params, "%.3f");
    menu_env[n++] = MI_FLOAT("Cathode R gain", &vc->tv.cathode_gain_r, 0.02f, 0.0f, 2.0f, gpu_cb_update_beam_params, "%.3f");
    menu_env[n++] = MI_FLOAT("Cathode G gain", &vc->tv.cathode_gain_g, 0.02f, 0.0f, 2.0f, gpu_cb_update_beam_params, "%.3f");
    menu_env[n++] = MI_FLOAT("Cathode B gain", &vc->tv.cathode_gain_b, 0.02f, 0.0f, 2.0f, gpu_cb_update_beam_params, "%.3f");
    menu_env[n++] = MI_FLOAT("APL bias drift", &vc->tv.apl_black_lift, 0.01f, 0.0f, 1.0f, gpu_cb_update_beam_params, "%.3f");
    menu_env[n++] = MI_FLOAT("Thermal tint (generic)", &vc->tv.thermal_dome_amount, 0.01f, 0.0f, 0.3f, gpu_cb_update_beam_params, "%.3f");
    menu_env[n++] = MI_FLOAT("Audio microphonics", &vc->tv.microphonic_amount, 0.01f, 0.0f, 1.0f, gpu_cb_update_beam_params, "%.3f");
    const int menu_env_count=n;

    /* ================================================================
     * APU analog (CPU-side DAC parameters)
     * ================================================================ */
    n = 0;
    /* ================================================================
     * Audio chain stages (GPU verification path)
     * ================================================================ */
    n = 0;
    menu_audio_chain[n++] = MI_FLOAT("Amp drive",       &ac->amp_saturation.drive,    0.1f, 1.0f, 6.0f, gpu_cb_audio_prepare, "%.1f");
    menu_audio_chain[n++] = MI_FLOAT("PSU hum amp",     &ac->psu_hum.amplitude,       0.001f, 0.0f, 0.05f, gpu_cb_audio_prepare, "%.3f");
    menu_audio_chain[n++] = MI_FLOAT("PSU hum freq",    &ac->psu_hum.frequency,       10.0f, 50.0f, 120.0f, gpu_cb_audio_prepare, "%.0f");
    menu_audio_chain[n++] = MI_FLOAT("PSU 2nd harm",    &ac->psu_hum.harmonic_2,      0.05f, 0.0f, 1.0f, gpu_cb_audio_prepare, "%.2f");
    menu_audio_chain[n++] = MI_FLOAT("PSU 3rd harm",    &ac->psu_hum.harmonic_3,      0.02f, 0.0f, 0.5f, gpu_cb_audio_prepare, "%.2f");
    menu_audio_chain[n++] = MI_FLOAT("Noise amp",       &ac->noise_floor.amplitude,   0.001f, 0.0f, 0.05f, gpu_cb_audio_prepare, "%.3f");
    menu_audio_chain[n++]=MI_CYCLIC("Console audio circuit", &preset_loaded.console_variant,0,3,gpu_cb_audio_setup,"Famicom|NES front|NES top|Dendy");
    menu_audio_chain[n++]=MI_CYCLIC("Speaker model", &preset_loaded.speaker_type,0,5,gpu_cb_audio_setup,"Small TV|Console TV|PVM|Arcade|Headphones|Famicom RF");
    menu_audio_chain[n++]=MI_FLOAT("Audio cable length m", &preset_loaded.audio_cable_length_m,.25f,0,20,gpu_cb_audio_setup,"%.2f");
    menu_audio_chain[n++]=MI_FLOAT("Audio cable C/m", &preset_loaded.audio_cable.capacitance_per_m,5e-12f,0,200e-12f,gpu_cb_audio_setup,"%.1e");
    menu_audio_chain[n++]=MI_FLOAT("Audio cable R/m", &preset_loaded.audio_cable.resistance_per_m,.1f,0,5,gpu_cb_audio_setup,"%.2f");
    menu_audio_chain[n++]=MI_FLOAT("Audio connector R", &preset_loaded.audio_cable.connector_resistance,.1f,0,5,gpu_cb_audio_setup,"%.2f");
    const int menu_audio_chain_count=n;

    n=0;
    menu_rf[n++]=MI_FLOAT("Carrier level dBm", &vc->rf.carrier_level_dbm, 1,-80,-5,gpu_cb_reinit_stages,"%.0f");
    menu_rf[n++]=MI_FLOAT("Noise floor dBm", &vc->rf.noise_floor_dbm, 1,-110,-30,gpu_cb_reinit_stages,"%.0f");
    menu_rf[n++]=MI_FLOAT("IF video edge Hz", &vc->rf.mod_bandwidth, 100000,1000000,6000000,gpu_cb_redesign_firs,"%.0f");
    menu_rf[n++]=MI_FLOAT("IF asymmetry", &vc->rf.if_asymmetry, .05f,0,1,gpu_cb_redesign_firs,"%.2f");
    menu_rf[n++]=MI_FLOAT("IF detuning Hz", &vc->rf.tuning_offset_hz, 10000,-1000000,1000000,gpu_cb_redesign_firs,"%.0f");
    menu_rf[n++]=MI_FLOAT("AGC attack ms", &vc->rf.agc_attack_ms, .1f,.01f,1000,gpu_cb_reinit_stages,"%.2f");
    menu_rf[n++]=MI_FLOAT("AGC release ms", &vc->rf.agc_release_ms, 1,1,1000,gpu_cb_reinit_stages,"%.1f");
    n=0;
    menu_vhs[n++]=MI_TOGGLE("NTSC composite/RF tape", &vc->vhs.enabled,gpu_cb_redesign_firs);
    menu_vhs[n++]=MI_FLOAT("Luma bandwidth Hz", &vc->vhs.luma_bandwidth, 100000,500000,3000000,gpu_cb_redesign_firs,"%.0f");
    menu_vhs[n++]=MI_FLOAT("Chroma bandwidth Hz", &vc->vhs.chroma_bandwidth, 10000,100000,600000,gpu_cb_redesign_firs,"%.0f");
    menu_vhs[n++]=MI_FLOAT("Chroma delay ns", &vc->vhs.chroma_delay_ns, 10,-1000,1000,gpu_cb_redesign_firs,"%.0f");
    menu_vhs[n++]=MI_FLOAT("Line timing error ns", &vc->vhs.timebase_ns, 5,0,300,gpu_cb_redesign_firs,"%.0f");
    menu_vhs[n++]=MI_FLOAT("Color phase error deg", &vc->vhs.chroma_phase_deg, .5f,0,20,gpu_cb_redesign_firs,"%.1f");
    menu_vhs[n++]=MI_FLOAT("Wideband noise", &vc->vhs.noise, .001f,0,.05f,gpu_cb_redesign_firs,"%.3f");
    menu_vhs[n++]=MI_FLOAT("Luma grain RMS", &vc->vhs.luma_noise_rms, .001f,0,.05f,gpu_cb_redesign_firs,"%.3f");
    menu_vhs[n++]=MI_FLOAT("Chroma noise RMS", &vc->vhs.chroma_noise_rms, .001f,0,.05f,gpu_cb_redesign_firs,"%.3f");
    menu_vhs[n++]=MI_FLOAT("Transport drift ms", &vc->vhs.drift_ms, 10,20,1000,gpu_cb_redesign_firs,"%.0f");
    menu_vhs[n++]=MI_FLOAT("Head switching ns", &vc->vhs.head_switch_ns, 20,0,700,gpu_cb_redesign_firs,"%.0f");
    menu_vhs[n++]=MI_FLOAT("Dropouts per second", &vc->vhs.dropout_rate, .1f,0,10,gpu_cb_redesign_firs,"%.1f");
    menu_vhs[n++]=MI_FLOAT("Dropout depth", &vc->vhs.dropout_depth, .05f,0,1,gpu_cb_redesign_firs,"%.2f");
    menu_vhs[n++]=MI_FLOAT("Playback luma peaking", &vc->vhs.luma_peaking, .05f,0,1,gpu_cb_redesign_firs,"%.2f");
    menu_vhs[n++]=MI_FLOAT("Playback luma tail", &vc->vhs.luma_smear, .05f,0,1,gpu_cb_redesign_firs,"%.2f");

    /* Everyday picture controls, then the physical chain and tube service controls. */
    menu_picture[0] = menu_luma[4];
    menu_picture[1] = menu_luma[5];
    menu_picture[2] = menu_color_decode[1];
    menu_picture[2].label = "Color (saturation)";
    menu_picture[3] = menu_color_decode[0];
    menu_picture[3].label = "Tint (hue)";
    menu_picture[4] = menu_luma[2];
    menu_picture[5] = menu_color_decode[2];
    menu_picture[5].label = "Color temperature";
    menu_picture[6] = menu_video_amp[3];
    menu_picture[7] = MI_FLOAT("HDR emission gain", &vc->tv.hdr_gain, 0.05f, 0.1f, 4.0f, NULL, "%.2fx");
    menu_picture[8] = MI_FLOAT("Room light", &vc->tv.ambient_light, 0.005f, 0, 0.3f, NULL, "%.3f");
    n = 0;
    menu_presets_video_idx = -1;
    menu_video[n++] = MI_SUB("PPU / connection", menu_dac, menu_dac_count);
    menu_video[n++] = MI_SUB("Console output", menu_console, menu_console_count);
    menu_video[n++] = MI_SUB("Cable", menu_cable, menu_cable_count);
    menu_video[n++] = MI_SUB("RF receiver",menu_rf,7);
    menu_video[n++] = MI_SUB("VHS recording / playback",menu_vhs,15);
    menu_video[n++] = MI_SUB("Y/C separation", menu_comb, menu_comb_count);
    menu_video[n++] = MI_SUB("Chroma decoder", menu_chroma, menu_chroma_count);
    menu_video[n++] = MI_SUB("Luma response", menu_luma, menu_luma_count);
    menu_video[n++] = MI_SUB("Color decoder", menu_color_decode, menu_color_decode_count);
    int video_count = n;
    menu_tube[0] = MI_SUB("Gun amplifiers", menu_video_amp, menu_video_amp_count);
    menu_tube[1] = MI_SUB("Beam / deflection", menu_beam, menu_beam_count);
    menu_tube[2] = MI_SUB("Mask / phosphor", menu_phosphor, menu_phosphor_count);
    menu_tube[3] = MI_SUB("Glass / geometry", menu_glass, menu_glass_count);
    menu_tube[4] = MI_SUB("Room / wear", menu_env, menu_env_count);
    menu_audio_top[0] = MI_CYCLIC("Processing", ctx->use_gpu_audio, 0, 1, gpu_cb_audio_backend, "CPU|GPU + fallback");
    menu_audio_top[1] = MI_FLOAT("Volume", &ctx->analog_controls->output_gain, 0.05f, 0, 3, NULL, "%.2fx");
    menu_audio_top[2] = MI_SUB("Analog character", menu_audio_chain, menu_audio_chain_count);
    menu_diagnostics[0] = MI_TOGGLE("Mask/glass bypass", &ctx->display_bypass, gpu_cb_display_bypass);
    preset_menu_root[0] = MI_SUB("Picture", menu_picture, sizeof(menu_picture)/sizeof(menu_picture[0]));
    preset_menu_root[1] = MI_SUB("Audio", menu_audio_top, 3);
    preset_menu_root[2] = MI_SUB("Presets", menu_presets, 1 + preset_count);
    preset_menu_root[3] = MI_SUB("Signal chain", menu_video, video_count);
    preset_menu_root[4] = MI_SUB("CRT / room", menu_tube, 5);
    preset_menu_root[5] = MI_SUB("Diagnostics", menu_diagnostics, 1);
    menu_display[0] = MI_CYCLIC("Mask sampling",&ctx->render_ctx->mask_alignment,0,1,gpu_cb_mask_alignment,"Panel pixels|CRT pitch");
    menu_display[1] = MI_CYCLIC("Panel subpixels",&ctx->render_ctx->panel_subpixels,0,2,gpu_cb_panel_subpixels,"Off|RGB stripe|BGR stripe");
    menu_display[2] = make_item("Native fullscreen",OSD_MI_ACTION,NULL,0,0,0,NULL,NULL,0,gpu_cb_fullscreen,NULL);
    menu_display[3] = MI_CYCLIC("Presentation",&ctx->render_ctx->presentation_mode,0,2,NULL,"Hold|BFI (high Hz)|60 Hz hold");
    menu_display[4] = MI_FLOAT("Dark refresh",&ctx->render_ctx->dark_frame_level,.05f,0,1,NULL,"%.2f");
    menu_display[5] = MI_CYCLIC("HDR gain",&ctx->render_ctx->hdr_gain_mode,0,1,gpu_cb_hdr_gain_mode,"Auto|Preset");
    menu_display[6] = MI_CYCLIC("Panel primaries",&ctx->render_ctx->panel_primaries,0,1,gpu_cb_panel_primaries,"sRGB|P3");
    menu_lab[0] = MI_CYCLIC("Split",&ctx->render_ctx->lab_split,0,2,gpu_cb_lab,"Off|Right half|Left half");
    menu_lab[1] = MI_FLOAT("Gap share R",&ctx->render_ctx->lab_gap[0],.05f,0,.5f,gpu_cb_lab,"%.2f");
    menu_lab[2] = MI_FLOAT("Gap share G",&ctx->render_ctx->lab_gap[1],.05f,0,.5f,gpu_cb_lab,"%.2f");
    menu_lab[3] = MI_FLOAT("Gap share B",&ctx->render_ctx->lab_gap[2],.05f,0,.5f,gpu_cb_lab,"%.2f");
    menu_lab[4] = MI_FLOAT("Gain R",&ctx->render_ctx->lab_gain[0],.05f,.5f,1.5f,gpu_cb_lab,"%.2f");
    menu_lab[5] = MI_FLOAT("Gain G",&ctx->render_ctx->lab_gain[1],.05f,.5f,1.5f,gpu_cb_lab,"%.2f");
    menu_lab[6] = MI_FLOAT("Gain B",&ctx->render_ctx->lab_gain[2],.05f,.5f,1.5f,gpu_cb_lab,"%.2f");
    menu_lab[7] = MI_FLOAT("Stripe fill",&ctx->render_ctx->lab_fill,.02f,.1f,.5f,gpu_cb_lab,"%.2f");
    menu_display[7] = MI_SUB("Subpixel lab",menu_lab,8);
    preset_menu_root[6] = MI_SUB("Host display",menu_display,menu_display_count);
    preset_menu_root[7] = MI_TOGGLE("Room reflections (G)", &ctx->render_ctx->room_reflections_enabled, gpu_cb_room_reflections);
    /* Reset stays last: preset_menu_root_append() inserts before it. */
    preset_menu_root[8] = make_item("Reset console (R)",OSD_MI_ACTION,NULL,0,0,0,NULL,NULL,0,gpu_cb_console_reset,NULL);
}

/* ============================================================================
 * Overlay compositing (chain vis only — OSD rendered in main.c)
 * ============================================================================ */

void preset_composite_overlays(PresetCtx *ctx) {
    const uint8_t (*pal)[3] = ctx->display_ppu->color_palette
                              ? ctx->display_ppu->color_palette
                              : ppu_palette_2c02;

    /* Chain visualiser overlay. */
    if (chain_vis_is_open(ctx->chain_vis)) {
        chain_vis_update(ctx->chain_vis, *ctx->current_preset);
        int ov_w, ov_h;
        const uint8_t *ov = chain_vis_get_overlay(ctx->chain_vis, &ov_w, &ov_h);
        if (ov) {
            uint8_t *rgb = ctx->display_ppu->framebuffer;
            uint16_t *idx = ctx->display_ppu->index_framebuffer;
            for (int i = 0; i < ov_w * ov_h; i++) {
                uint8_t pi = ov[i];
                if (pi == 0x0F) continue;
                pi &= 0x3F;
                idx[i] = pi;
                rgb[i*3+0] = pal[pi][0];
                rgb[i*3+1] = pal[pi][1];
                rgb[i*3+2] = pal[pi][2];
            }
        }
    }

    /* NOTE: OSD menu overlay is rendered in main.c, not here.
     * The osd_menu_* state is file-static per TU (declared static in osd.h),
     * so it must be checked and rendered in the same TU that manages
     * the menu (main.c handles M key -> osd_menu_open_root). */
}

/* Share OSD float targets/ranges/callbacks with the live editor. */
static void append_osd_controls(DebugControl *out,int *count,const OSDMenuItem *items,int n,const char *group) {
    for(int i=0;i<n;i++) {
        const OSDMenuItem *m=&items[i];
        if(m->submenu) append_osd_controls(out,count,m->submenu,m->submenu_count,m->label);
        else if(m->type==OSD_MI_FLOAT && m->target) {
            bool found=false;
            for(int j=0;j<*count;j++) if(out[j].value==m->target) { found=true; break; }
            if(!found && *count<DEBUG_MAX_CONTROLS)
                out[(*count)++]=(DebugControl){m->label,group,m->target,m->min_val,m->max_val,m->on_change};
        }
    }
}

/* The editor uses the same physical values and update paths as the OSD. */
void preset_register_debug_controls(PresetCtx *ctx, DebugServer *server) {
    TVDisplayParams *tv = &ctx->video_chain->tv;
    AudioChain *ac = ctx->audio_chain;
    DebugControl controls[DEBUG_MAX_CONTROLS] = {
        {"Luma bandwidth (Hz)", "Decoder", &tv->luma_bandwidth, 500000, 8000000, gpu_cb_redesign_firs},
        {"Chroma bandwidth (Hz)", "Decoder", &tv->chroma_bandwidth, 100000, 3000000, gpu_cb_redesign_firs},
        {"Q bandwidth (Hz, 0 = I)", "Decoder", &tv->chroma_q_bandwidth, 0, 2000000, gpu_cb_redesign_firs},
        {"Luma carrier rejection", "Decoder", &tv->luma_notch_depth, 0, 1, gpu_cb_redesign_firs},
        {"Saturation", "Decoder", &tv->saturation, 0, 2, gpu_cb_update_color_matrix},
        {"Hue (degrees)", "Decoder", &tv->hue_offset, -180, 180, gpu_cb_update_color_matrix},
        {"White point (K)", "Decoder", &tv->color_temperature, 3200, 9300, gpu_cb_update_color_matrix},
        {"R-Y gain offset", "Decoder", &tv->decoder_red_gain, -.5f, .5f, gpu_cb_update_color_matrix},
        {"B-Y gain offset", "Decoder", &tv->decoder_blue_gain, -.5f, .5f, gpu_cb_update_color_matrix},
        {"Horizontal spot growth", "Beam", &tv->beam_spot_growth, 0, 1, gpu_cb_update_beam_params},
        {"Dark FWHM (lines)", "Beam", &tv->beam_fwhm_min, 0.12f, 2.35f, gpu_cb_update_beam_params},
        {"White FWHM (lines)", "Beam", &tv->beam_fwhm_max, 0.12f, 2.35f, gpu_cb_update_beam_params},
        {"Video rail sag", "Beam", &tv->beam_current_load, 0, 0.3f, gpu_cb_update_beam_params},
        {"Horizontal streaks", "Beam", &tv->video_black_droop, 0, 0.5f, gpu_cb_update_beam_params},
        {"Recovery (us)", "Beam", &tv->video_recovery_us, 1, 100, gpu_cb_update_beam_params},
        {"Size sag (+shrink)", "Beam", &tv->hv_sag, -0.5f, 0.5f, gpu_cb_update_beam_params},
        {"Load focus change", "Beam", &tv->focus_breathing, 0, 0.3f, gpu_cb_update_beam_params},
        {"Persistence (ms)", "Phosphor", &tv->persistence_ms, 0, 100, gpu_cb_update_beam_params},
        {"Slow decay (ms)", "Phosphor", &tv->persistence_tail_ms, 0, 100, gpu_cb_update_beam_params},
        {"Slow decay energy", "Phosphor", &tv->persistence_tail_weight, 0, 1, gpu_cb_update_beam_params},
        {"Red lifetime scale", "Phosphor", &tv->persistence_r, 0, 1, gpu_cb_update_beam_params},
        {"Green lifetime scale", "Phosphor", &tv->persistence_g, 0, 1, gpu_cb_update_beam_params},
        {"Blue lifetime scale", "Phosphor", &tv->persistence_b, 0, 1, gpu_cb_update_beam_params},
        {"Mask triads across", "Phosphor", &tv->mask_triads, 0, 2000, NULL},
        {"Mask pitch (triads = 0)", "Phosphor", &tv->mask_pitch_px, 1, 12, NULL},
        {"HDR emission gain", "Phosphor", &tv->hdr_gain, 0.5f, 3, NULL},
        {"Mask strength", "Phosphor", &tv->mask_strength, 0, 1, NULL},
        {"Glass curvature", "Glass", &tv->barrel, 0, 0.1f, NULL},
        {"Halation", "Glass", &tv->halation, 0, 0.5f, NULL},
        {"Halo width / height", "Glass", &tv->halation_sigma, 0, 0.05f, NULL},
        {"Room light", "Glass", &tv->ambient_light, 0, 0.2f, NULL},
        {"Cable length (m)", "Connection", &ctx->video_chain->cable.length_meters, 0, 20, gpu_cb_update_rc_params},
        {"RF hum", "Connection", &ctx->video_chain->console_psu_hum, 0, 0.1f, gpu_cb_reinit_stages},
        {"RF video bandwidth (Hz)", "Connection", &ctx->video_chain->rf.mod_bandwidth, 1000000, 6000000, gpu_cb_redesign_firs},
        {"RF IF asymmetry", "Connection", &ctx->video_chain->rf.if_asymmetry, 0, 1, gpu_cb_redesign_firs},
        {"RF tuning offset (Hz)", "Connection", &ctx->video_chain->rf.tuning_offset_hz, -1000000, 1000000, gpu_cb_redesign_firs},
        {"RF carrier level (dBm)", "Connection", &ctx->video_chain->rf.carrier_level_dbm, -60, -5, gpu_cb_reinit_stages},
        {"RF noise floor (dBm)", "Connection", &ctx->video_chain->rf.noise_floor_dbm, -90, -30, gpu_cb_reinit_stages},
        {"Amplifier drive", "Audio", &ac->amp_saturation.drive, 1, 6, gpu_cb_audio_prepare},
        {"Mains hum", "Audio", &ac->psu_hum.amplitude, 0, 0.05f, gpu_cb_audio_prepare},
        {"Mains frequency (Hz)", "Audio", &ac->psu_hum.frequency, 50, 120, gpu_cb_audio_prepare},
        {"Second harmonic", "Audio", &ac->psu_hum.harmonic_2, 0, 1, gpu_cb_audio_prepare},
        {"Third harmonic", "Audio", &ac->psu_hum.harmonic_3, 0, 0.5f, gpu_cb_audio_prepare},
        {"Noise floor", "Audio", &ac->noise_floor.amplitude, 0, 0.05f, gpu_cb_audio_prepare},
    };
    int count=0;
    while(count<DEBUG_MAX_CONTROLS && controls[count].name) count++;
    append_osd_controls(controls,&count,preset_menu_root,preset_menu_root_count,"Picture");
    debug_server_set_controls(server, controls, count);
}
