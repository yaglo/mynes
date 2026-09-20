#include "debug_server.h"
/*
 * preset_apply.c -- Preset application, OSD menu callbacks, overlay compositing
 */
#include "preset_apply.h"
#include <stdio.h>
#include <string.h>
#include <math.h>
#include <time.h>
#include <unistd.h>
#include <errno.h>
#include "ppu/ppu.h"
#include "preset_json.h"
#include "config.h"

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
    /* Zero-initialized preset fields (HPOS/VPOS/HSIZE/VSIZE) must default to
     * the identity transform (centered raster filling the tube). Older presets
     * don't set these, so we fix them up after the struct copy. */
    if (ctx->video_chain->tv.h_size < 0.01f) ctx->video_chain->tv.h_size = 1.0f;
    if (ctx->video_chain->tv.v_size < 0.01f) ctx->video_chain->tv.v_size = 1.0f;
    /* Back-compat: presets that predate luma_notch_depth (0 in JSON) get
     * the PVM-clean default so they look unchanged. Explicitly setting
     * the field to a low value opens the subcarrier leak. */
    if (ctx->video_chain->tv.luma_notch_depth <= 0.0001f)
        ctx->video_chain->tv.luma_notch_depth = 0.95f;
    ctx->video_chain->cable = p->video_cable;
    ctx->video_chain->rf = p->rf;
    ctx->video_chain->console_coupling_R = p->console_coupling_R;
    ctx->video_chain->console_coupling_C = p->console_coupling_C;
    ctx->video_chain->console_amp_bw = p->console_amp_bw;
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
    ctx->sig_state->chroma_gain = (p->chroma_gain > 0.0f) ? p->chroma_gain : 1.30f;

    /* --- SignalPrecompute: color matrix from preset TV params --- */
    rebuild_color_matrix(ctx);

    /* --- AudioChain --- */
    audio_chain_init_preset(ctx->audio_chain, p->console_variant,
                            p->speaker_type, new_region);
    if (p->audio_psu_hum_amplitude > 0) {
        ctx->audio_chain->psu_hum.enabled = true;
        ctx->audio_chain->psu_hum.amplitude = p->audio_psu_hum_amplitude;
    }
    if (p->audio_noise_floor > 0) {
        ctx->audio_chain->noise_floor.enabled = true;
        ctx->audio_chain->noise_floor.amplitude = p->audio_noise_floor;
    }
    if (p->audio_saturation_drive > 1.01f) {
        ctx->audio_chain->amp_saturation.enabled = true;
        ctx->audio_chain->amp_saturation.drive = p->audio_saturation_drive;
    }
    audio_chain_prepare(ctx->audio_chain);
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
         * This is the nuclear option but ensures all preset params take effect.
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
    ctx->audio_chain->sample_rate = (new_region == SIGNAL_REGION_PAL)
                                    ? (float)AUDIO_PAL_CPU_CLOCK
                                    : (float)AUDIO_NTSC_CPU_CLOCK;
    ctx->audio_chain->psu_hum.frequency = (new_region == SIGNAL_REGION_PAL)
                                          ? 50.0f : 60.0f;
    if (ctx->audio_chain->decimation.enabled) {
        ctx->audio_chain->decimation.decimation_ratio =
            (int)(ctx->audio_chain->sample_rate
                  / (float)AUDIO_OUTPUT_SAMPLE_RATE);
    }
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

    float hue_rad = vc->tv.hue_offset * (float)M_PI / 180.0f;
    float sat = vc->tv.saturation;

    /* Physical decode matrices — one per region. Each matches the
     * chroma pair handed to Matrix Decode.
     *
     * NTSC:
     *   demod_rotate = 4 yields the familiar I/Q pair.
     *
     * PAL:
     *   demod_rotate = 3 plus the PAL correction stage in the GPU
     *   chain produce a stable (V, U) pair after odd-line U sign
     *   compensation and 1H V averaging, mirroring the PAL-CRT-style
     *   CPU decoder in src/nes/composite.h.
     *
     * The matrices are the textbook BT.601 YIQ->RGB (NTSC) and
     * BT.470 YUV->RGB (PAL), scaled x1.15 to match the mild decoder-IC
     * saturation boost used by the CPU path.
     *
     * The column convention here is (Y, chroma_col1, chroma_col2)
     * where chroma_col1/col2 are the decoder's two outputs:
     *   NTSC: col1 = I, col2 = Q
     *   PAL:  col1 = V, col2 = U
     *
     * So the PAL coefficients are a column-swapped YUV matrix:
     *   R = Y + 1.140 V + 0.000 U
     *   G = Y − 0.581 V − 0.395 U
     *   B = Y + 0.000 V + 2.032 U */
    static const float base_ntsc[3][3] = {
        { 1.0f,  1.1222f,  0.7391f },
        { 1.0f, -0.3192f, -0.7384f },
        { 1.0f, -1.2374f,  1.9058f },
    };
    static const float base_pal[3][3] = {
        { 1.0f,  1.311f,   0.000f  },   /* 1.140 * 1.15 */
        { 1.0f, -0.668f,  -0.454f  },   /* -0.581*1.15, -0.395*1.15 */
        { 1.0f,  0.000f,   2.337f  },   /* 2.032 * 1.15 */
    };
    const float (*base)[3] = (sp->region == SIGNAL_REGION_PAL)
                             ? base_pal : base_ntsc;
    float ch = cosf(hue_rad), sh = sinf(hue_rad);

    float temp_norm = (vc->tv.color_temperature - 6500.0f) / 3500.0f;
    float warm_r = 1.0f + temp_norm * 0.03f;
    float warm_g = 1.0f + temp_norm * 0.01f;
    float warm_b = 1.0f - temp_norm * 0.03f;

    float dr = vc->tv.r_drive * warm_r;
    float dg = vc->tv.g_drive * warm_g;
    float db = vc->tv.b_drive * warm_b;

    float con = sp->contrast;
    float bri = sp->brightness;

    /* Y column scaled by contrast; I/Q columns scaled by chroma_gain. */
    float cg = sp->chroma_gain;
    sp->color_matrix[0][0] = dr * con;
    sp->color_matrix[0][1] = dr * sat * cg * (base[0][1] * ch - base[0][2] * sh);
    sp->color_matrix[0][2] = dr * sat * cg * (base[0][1] * sh + base[0][2] * ch);
    sp->color_matrix[1][0] = dg * con;
    sp->color_matrix[1][1] = dg * sat * cg * (base[1][1] * ch - base[1][2] * sh);
    sp->color_matrix[1][2] = dg * sat * cg * (base[1][1] * sh + base[1][2] * ch);
    sp->color_matrix[2][0] = db * con;
    sp->color_matrix[2][1] = db * sat * cg * (base[2][1] * ch - base[2][2] * sh);
    sp->color_matrix[2][2] = db * sat * cg * (base[2][1] * sh + base[2][2] * ch);

    sp->color_bias[0] = vc->tv.r_cutoff + bri * dr;
    sp->color_bias[1] = vc->tv.g_cutoff + bri * dg;
    sp->color_bias[2] = vc->tv.b_cutoff + bri * db;

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
     * from Y so dot crawl + cross-color don't appear. Depth is taken
     * from tv.luma_notch_depth so presets can DISABLE it (set to 0) for
     * consumer-TV looks. Default 0.95 (effectively transparent ≈-26 dB
     * rejection). Skipped entirely below 23 taps — a short FIR can't
     * host a sharp notch and the attempt just attenuates luma broadly. */
    float subcarrier_norm = 1.0f / 12.0f;  /* NTSC/PAL: 12 samples per cycle */
    float notch_depth = vc->tv.luma_notch_depth;
    if (notch_depth < 0.0f) notch_depth = 0.0f;
    if (notch_depth > 1.0f) notch_depth = 1.0f;
    if (notch_depth > 0.01f && sp->fir_y_n >= 23) {
        signal_design_fir_notch(sp->fir_y, sp->fir_y_n, y_cutoff,
                                subcarrier_norm, notch_depth);
    }

    /* Luma peaking: TV "sharpness" control — boosts edges in the luma path. */
    signal_apply_peaking(sp->fir_y, sp->fir_y_n, y_cutoff, vc->tv.luma_peaking);
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
    /* Map TV beam params to GPU shader params.
     * beam_sharpness (0.1-1.0) → sigma_narrow base (0.15-0.35)
     * beam_height_min (0.0-1.0) → scales sigma_narrow (dark beam width)
     * beam_height_max (0.3-2.0) → sigma_wide (bright beam width)
     * Min=0: sharp narrow beam (visible gaps between scanlines on dark)
     * Min=1: wide narrow beam (gaps filled even on dark) */
    float sig_n = 0.35f - tv->beam_sharpness * 0.20f;
    sig_n *= (0.4f + tv->beam_height_min * 0.8f);  /* scale by height_min 0.4-1.2 */
    float sig_w = 0.30f + tv->beam_height_max * 0.33f;
    if (sig_n < 0.10f) sig_n = 0.10f;
    if (sig_w < 0.20f) sig_w = 0.20f;
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

static void gpu_cb_audio_prepare(void) {
    audio_chain_prepare(g_ctx->audio_chain);
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
    return true;
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
OSDMenuItem preset_menu_root[3];   /* defined below; declared here so the
                                    * save callback can update it. */

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
    p.console_coupling_R = g_ctx->video_chain->console_coupling_R;
    p.console_coupling_C = g_ctx->video_chain->console_coupling_C;
    p.console_amp_bw     = g_ctx->video_chain->console_amp_bw;
    p.console_psu_hum    = g_ctx->video_chain->console_psu_hum;
    p.brightness         = g_ctx->sig_state->brightness;
    p.contrast           = g_ctx->sig_state->contrast;
    p.chroma_gain        = g_ctx->sig_state->chroma_gain;
    p.region             = signal_region_normalize(g_ctx->region);

    return p;
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
    snprintf(p.name, sizeof(p.name), "%s (custom %s)", base_name, ts_pretty);
    if (preset_loaded.description[0]) {
        char prev_desc[400];
        snprintf(prev_desc, sizeof(prev_desc), "%s",
                 preset_loaded.description);
        snprintf(p.description, sizeof(p.description),
                 "Customised from \"%s\" — %s",
                 base_name, prev_desc);
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
    snprintf(out_path, out_path_sz, "%s/%s_custom_%s.json",
             dir, slug, ts_filename);

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
    snprintf(name_buf, sizeof(name_buf), "%s", display);
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
    preset_menu_root[2].submenu_count = 1 + preset_count;
}

uint32_t preset_catalog_revision(void) { return catalog_revision; }
int preset_active_index(void) { return g_ctx ? *g_ctx->current_preset : -1; }
bool preset_is_user(int index) { return index >= 0 && index < preset_count && preset_user[index]; }
bool preset_is_modified(void) {
    if (!g_ctx || preset_active_index() < 0) return true;
    PhysicalPreset p = preset_capture_live();
    return memcmp(&p.tv,&preset_baseline.tv,sizeof(p.tv)) ||
        memcmp(&p.video_cable,&preset_baseline.video_cable,sizeof(p.video_cable)) ||
        memcmp(&p.rf,&preset_baseline.rf,sizeof(p.rf)) ||
        p.connection != preset_baseline.connection || p.comb_type != preset_baseline.comb_type ||
        p.brightness != preset_baseline.brightness || p.contrast != preset_baseline.contrast ||
        p.chroma_gain != preset_baseline.chroma_gain || p.console_psu_hum != preset_baseline.console_psu_hum;
}

static void preset_refresh_menu(void) {
    for(int i=0;i<preset_count;i++) {
        OSDMenuItem mi={0}; mi.label=preset_names[i]; mi.type=OSD_MI_ACTION; mi.action=preset_actions[i];
        menu_presets[i+1]=mi;
    }
    if(menu_presets_video_idx>=0) menu_video[menu_presets_video_idx].submenu_count=preset_count+1;
    preset_menu_root[2].submenu_count=preset_count+1;
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
static OSDMenuItem menu_dac[6];           /* Stage 1: DAC / connection / phase */
static OSDMenuItem menu_console[5];       /* Stage 2: console output */
static OSDMenuItem menu_cable[8];         /* Stage 3: cable transmission */
static OSDMenuItem menu_comb[5];          /* Stage 5: comb filter + 3D comb */
static OSDMenuItem menu_chroma[6];        /* Stage 6-7: chroma demod */
static OSDMenuItem menu_luma[7];          /* Stage 8: luma processing */
static OSDMenuItem menu_color_decode[10]; /* Stage 9: matrix decode */
static OSDMenuItem menu_video_amp[5];     /* Stage 10: video amplifier */
static OSDMenuItem menu_beam[32];         /* Stage 11: electron beam */
static OSDMenuItem menu_phosphor[10];     /* Stage 12: phosphor screen */
static OSDMenuItem menu_glass[24];        /* Stage 13: CRT glass + service geometry */
static OSDMenuItem menu_env[6];           /* Stage 14: environment */
static OSDMenuItem menu_apu[7];
static OSDMenuItem menu_audio_chain[9];

/* Mid-level submenus. menu_video[] + preset_menu_root[] are forward-
 * declared near the top of this file so the save action can reach them. */
static OSDMenuItem menu_audio_top[3];
int         preset_menu_root_count = 3;

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
    preset_count = preset_json_scan_dir("presets",
        preset_names, preset_paths, PRESET_MAX);
    int shipped = preset_count;

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
    menu_dac[n++] = MI_CYCLIC("Connection",    &vc->connection, 0.0f, (float)(VIDEO_CONN_COUNT-1), gpu_cb_reinit_stages, "%d");
    menu_dac[n++] = MI_CYCLIC("Base phase",    &sp->phase_base,       0.0f, 11.0f, gpu_cb_update_color_matrix, "%d / 12");
    menu_dac[n++] = MI_INT("Line advance",     &sp->phase_line_adv,   1.0f, -12.0f, 12.0f, gpu_cb_update_color_matrix, "%+d");
    menu_dac[n++] = MI_INT("Field advance",    &sp->phase_field_adv,  1.0f, -12.0f, 12.0f, gpu_cb_update_color_matrix, "%+d");
    menu_dac[n++] = MI_INT("Num fields",       &sp->phase_num_fields, 1.0f,   1.0f, 12.0f, gpu_cb_update_color_matrix, "%d");

    /* ================================================================
     * Stage 2: Console output — coupling, amp bandwidth, PSU
     * ================================================================ */
    n = 0;
    /* Output R: NES output impedance resistor on motherboard. Adds to total
     * series resistance → affects cable bandwidth via R*C time constant. */
    menu_console[n++] = MI_FLOAT("Output R",    &vc->console_coupling_R, 5.0f, 10.0f, 200.0f, gpu_cb_update_rc_params, "%.0f");
    /* Coupling C: DC-blocking cap on NES output. At typical values (10µF with
     * 75Ω load) fc ≈ 0.2 Hz — effectively transparent at video frequencies.
     * Tunable for completeness; visible only at tiny values (< 100 nF). */
    menu_console[n++] = MI_FLOAT("Coupling C",  &vc->console_coupling_C, 1e-6f, 1e-6f, 100e-6f, gpu_cb_update_rc_params, "%.1e");
    /* PSU hum: AC supply leaking into video via poor regulation. */
    menu_console[n++] = MI_FLOAT("PSU hum",     &vc->console_psu_hum,    0.01f, 0.0f, 0.30f, gpu_cb_update_rc_params, "%.3f");
    /* Note: 2C02 video amp bandwidth is an intrinsic chip property (~6 MHz),
     * not tunable — it's fixed in the RC filter init. */

    /* ================================================================
     * Stage 3: Cable — transmission line parameters
     * ================================================================ */
    n = 0;
    menu_cable[n++] = MI_FLOAT("Length (m)",    &vc->cable.length_meters,       0.25f, 0.5f, 10.0f, gpu_cb_update_rc_params, "%.2f");
    menu_cable[n++] = MI_FLOAT("R/m",           &vc->cable.resistance_per_m,    0.1f, 0.0f, 5.0f, gpu_cb_update_rc_params, "%.2f");
    menu_cable[n++] = MI_FLOAT("C/m (pF)",      &vc->cable.capacitance_per_m,   5e-12f, 10e-12f, 200e-12f, gpu_cb_update_rc_params, "%.1e");
    menu_cable[n++] = MI_FLOAT("Connector R",   &vc->cable.connector_resistance, 0.1f, 0.0f, 5.0f, gpu_cb_update_rc_params, "%.2f");
    menu_cable[n++] = MI_FLOAT("Shield eff",    &vc->cable.shield_effectiveness, 0.05f, 0.0f, 1.0f, gpu_cb_update_rc_params, "%.2f");
    menu_cable[n++] = MI_FLOAT("Ghost level",   &vc->cable.ghost_level,          0.01f, 0.0f, 0.20f, gpu_cb_update_rc_params, "%.2f");
    menu_cable[n++] = MI_INT("Ghost delay",     &vc->cable.ghost_delay,          2.0f, 0.0f, 40.0f, gpu_cb_update_rc_params, "%d");

    /* ================================================================
     * Stage 5: Comb filter — Y/C separation + 3D comb
     * ================================================================ */
    n = 0;
    /* 0=NONE, 1=1LINE, 2=2LINE, 3=3LINE, 4=BYPASS */
    menu_comb[n++] = MI_CYCLIC("Comb type",        &vc->comb_type,  0.0f, 4.0f, gpu_cb_apply_comb, "%d");
    /* Notch depth: 0.0 = raw composite in Y (rainbow fringes, dot crawl)
     * → 1.0 = perfect Y/C separation (PVM look). Set to 0 in preset to
     * use the per-comb-type default (0.65 for 1-line, 0.85 for 2/3-line). */
    menu_comb[n++] = MI_FLOAT("Notch depth",        &vc->comb_notch_depth, 0.05f, 0.0f, 1.0f, gpu_cb_reinit_stages, "%.2f");
    menu_comb[n++] = MI_FLOAT("Temporal blend",     &ctx->video_gpu_chain->temporal_blend, 0.05f, 0.0f, 0.5f, gpu_cb_update_beam_params, "%.2f");
    menu_comb[n++] = MI_FLOAT("Motion threshold",   &vc->tv.motion_threshold, 0.01f, 0.0f, 0.30f, gpu_cb_update_beam_params, "%.2f");

    /* ================================================================
     * Stage 6-7: Chroma demodulation
     * ================================================================ */
    n = 0;
    menu_chroma[n++] = MI_FLOAT("I BW (wide)",    &vc->tv.chroma_bandwidth, 50000.0f,  300000.0f, 2000000.0f, gpu_cb_redesign_firs, "%.0f");
    /* Q bandwidth: NTSC spec = 0.5 MHz (narrower than 1.3 MHz I). Zero
     * here means "equi-band with I", which matches cheap consumer sets
     * and the pre-split pipeline default. */
    menu_chroma[n++] = MI_FLOAT("Q BW (narrow)",  &vc->tv.chroma_q_bandwidth, 50000.0f, 0.0f, 2000000.0f, gpu_cb_redesign_firs, "%.0f");
    menu_chroma[n++] = MI_INT("I taps",            &sp->fir_c_n, 2.0f, 5.0f, 63.0f, gpu_cb_redesign_firs, "%d");
    menu_chroma[n++] = MI_INT("Q taps",            &sp->fir_q_n, 2.0f, 5.0f, 63.0f, gpu_cb_redesign_firs, "%d");
    menu_chroma[n++] = MI_FLOAT("Chroma gain",     &sp->chroma_gain,  0.10f, 0.0f, 5.0f, gpu_cb_update_color_matrix, "%.2f");
    menu_chroma[n++] = MI_CYCLIC("Demod rotate",   &sp->demod_rotate, -12.0f, 12.0f, gpu_cb_update_color_matrix, "%+d / 12");
    menu_chroma[n++] = MI_FLOAT("Color killer",    &vc->tv.color_killer, 0.01f, 0.0f, 0.30f, gpu_cb_update_color_matrix, "%.2f");
    /* Chroma FIR window ringing: 0=Hamming (smooth), 1=rect (Gibbs overshoot). */
    menu_chroma[n++] = MI_FLOAT("Ringing",         &vc->tv.fir_ringing, 0.05f, 0.0f, 1.0f, gpu_cb_redesign_firs, "%.2f");

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
    /* Notch depth — 0.95 kills dot crawl; 0 = off (rainbow fringes).
     * Requires Luma taps ≥ 23 to take effect. */
    menu_luma[n++] = MI_FLOAT("Notch depth",  &vc->tv.luma_notch_depth, 0.05f, 0.0f, 1.0f, gpu_cb_redesign_firs, "%.2f");
    menu_luma[n++] = MI_FLOAT("Brightness",    &sp->brightness,     0.02f, -1.0f, 1.0f, gpu_cb_update_color_matrix, "%+.2f");
    menu_luma[n++] = MI_FLOAT("Contrast",      &sp->contrast,       0.02f, 0.1f, 3.0f, gpu_cb_update_color_matrix, "%.2f");

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

    /* ================================================================
     * Stage 10: Video amplifier — per-gun bandwidth + gamma
     * ================================================================ */
    n = 0;
    menu_video_amp[n++] = MI_FLOAT("R bandwidth",  &vc->tv.r_bandwidth,  100000.0f, 2000000.0f, 10000000.0f, gpu_cb_reinit_stages, "%.0f");
    menu_video_amp[n++] = MI_FLOAT("G bandwidth",  &vc->tv.g_bandwidth,  100000.0f, 2000000.0f, 10000000.0f, gpu_cb_reinit_stages, "%.0f");
    menu_video_amp[n++] = MI_FLOAT("B bandwidth",  &vc->tv.b_bandwidth,  100000.0f, 2000000.0f, 10000000.0f, gpu_cb_reinit_stages, "%.0f");
    menu_video_amp[n++] = MI_FLOAT("Gamma",        &vc->tv.gamma,        0.02f, 1.5f, 2.8f, gpu_cb_update_color_matrix, "%.2f");

    /* ================================================================
     * Stage 11: Electron beam — spot profile, bloom, convergence, jitter
     * ================================================================ */
    n = 0;
    menu_beam[n++] = MI_FLOAT("Sharpness",        &vc->tv.beam_sharpness,       0.05f, 0.0f, 1.0f, gpu_cb_update_beam_params, "%.2f");
    menu_beam[n++] = MI_FLOAT("Height min",        &vc->tv.beam_height_min,      0.02f, 0.0f, 1.0f, gpu_cb_update_beam_params, "%.2f");
    menu_beam[n++] = MI_FLOAT("Height max",        &vc->tv.beam_height_max,      0.02f, 0.3f, 2.0f, gpu_cb_update_beam_params, "%.2f");
    menu_beam[n++] = MI_FLOAT("Spot size",         &vc->tv.beam_spot_size,       1.0f, 1.0f, 16.0f, gpu_cb_update_beam_params, "%.0f");
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
    /* Beam current loading: busy scanlines sag the HV supply, dimming
     * the whole line. 0 = perfectly regulated (PVM). 0.08 = noticeable
     * banding on consumer sets. 0.20 = failing / overdriven CRT. */
    menu_beam[n++] = MI_FLOAT("Beam current load", &vc->tv.beam_current_load,    0.01f, 0.0f, 0.30f, gpu_cb_update_beam_params, "%.2f");
    /* Display-domain convergence (post-phosphor R/B shift). Applied in
     * crt_display.frag.glsl AFTER the beam's per-gun offsets below. */
    menu_beam[n++] = MI_FLOAT("Display conv",      &vc->tv.convergence_static,   0.05f, 0.0f, 2.0f, gpu_cb_update_beam_params, "%.2f");
    menu_beam[n++] = MI_FLOAT("Display conv dyn",  &vc->tv.convergence_dynamic,  0.02f, 0.0f, 2.0f, gpu_cb_update_beam_params, "%.2f");
    menu_beam[n++] = MI_FLOAT("Conv R X",          &vc->tv.conv_r_x,            0.5f, -10.0f, 10.0f, gpu_cb_update_beam_params, "%+.1f");
    menu_beam[n++] = MI_FLOAT("Conv R Y",          &vc->tv.conv_r_y,            0.5f, -5.0f, 5.0f, gpu_cb_update_beam_params, "%+.1f");
    menu_beam[n++] = MI_FLOAT("Conv B X",          &vc->tv.conv_b_x,            0.5f, -10.0f, 10.0f, gpu_cb_update_beam_params, "%+.1f");
    menu_beam[n++] = MI_FLOAT("Conv B Y",          &vc->tv.conv_b_y,            0.5f, -5.0f, 5.0f, gpu_cb_update_beam_params, "%+.1f");
    menu_beam[n++] = MI_FLOAT("H jitter",          &vc->tv.h_jitter,             0.001f, 0.0f, 0.02f, gpu_cb_update_beam_params, "%.3f");
    menu_beam[n++] = MI_FLOAT("V jitter",          &vc->tv.v_jitter,             0.001f, 0.0f, 0.02f, gpu_cb_update_beam_params, "%.3f");
    menu_beam[n++] = MI_FLOAT("Hum bar",           &vc->tv.hum_bar_amplitude,    0.01f, 0.0f, 0.20f, gpu_cb_update_beam_params, "%.2f");
    menu_beam[n++] = MI_FLOAT("RF interference",   &vc->tv.rf_interference,      0.5f, 0.0f, 5.0f, gpu_cb_update_beam_params, "%.1f");
    menu_beam[n++] = MI_FLOAT("Geometry warp",     &vc->tv.geometry_warp,        0.2f, 0.0f, 5.0f, gpu_cb_update_beam_params, "%.1f");
    menu_beam[n++] = MI_FLOAT("Top band shift",    &vc->tv.top_band_shift,       0.2f, -12.0f, 12.0f, gpu_cb_update_beam_params, "%+.1f");
    menu_beam[n++] = MI_FLOAT("Top edge skew",     &vc->tv.top_edge_skew,        0.2f, -12.0f, 12.0f, gpu_cb_update_beam_params, "%+.1f");
    menu_beam[n++] = MI_FLOAT("Top band start",    &vc->tv.top_band_start,       1.0f, 0.0f, 239.0f, gpu_cb_update_beam_params, "%.0f");
    menu_beam[n++] = MI_FLOAT("Top band end",      &vc->tv.top_band_end,         1.0f, 0.0f, 239.0f, gpu_cb_update_beam_params, "%.0f");
    menu_beam[n++] = MI_FLOAT("Top edge width",    &vc->tv.top_edge_width,       0.01f, 0.01f, 0.30f, gpu_cb_update_beam_params, "%.2f");
    menu_beam[n++] = MI_FLOAT("Focus breathing",   &vc->tv.focus_breathing,      0.02f, 0.0f, 0.3f, gpu_cb_update_beam_params, "%.2f");
    menu_beam[n++] = MI_FLOAT("Scanline wobble",   &vc->tv.scanline_wobble,      0.05f, 0.0f, 1.0f, gpu_cb_update_beam_params, "%.2f");

    /* ================================================================
     * Stage 12: Phosphor screen — mask, persistence, subpixel
     * ================================================================ */
    n = 0;
    menu_phosphor[n++] = MI_CYCLIC("Mask type",      &vc->tv.mask_type, 0.0f, 2.0f, gpu_cb_update_beam_params, "%d");
    menu_phosphor[n++] = MI_FLOAT("Mask pitch px",   &vc->tv.mask_pitch_px,       0.5f, 1.0f, 20.0f, gpu_cb_update_beam_params, "%.1f");
    menu_phosphor[n++] = MI_FLOAT("Mask strength",   &vc->tv.mask_strength,       0.05f, 0.0f, 1.0f, gpu_cb_update_beam_params, "%.2f");
    /* P22 phosphor persistence: scales the per-channel blend weights below.
     * 2.0ms is the P22 reference; higher = more afterimage smear. Extended
     * range lets users crank persistence to mask dot crawl during motion. */
    menu_phosphor[n++] = MI_FLOAT("Persistence ms",  &vc->tv.persistence_ms,      1.0f, 0.5f, 50.0f, gpu_cb_update_beam_params, "%.1f");
    menu_phosphor[n++] = MI_FLOAT("Persist R",       &vc->tv.persistence_r,       0.02f, 0.5f, 1.0f, gpu_cb_update_beam_params, "%.2f");
    menu_phosphor[n++] = MI_FLOAT("Persist G",       &vc->tv.persistence_g,       0.02f, 0.5f, 1.0f, gpu_cb_update_beam_params, "%.2f");
    menu_phosphor[n++] = MI_FLOAT("Persist B",       &vc->tv.persistence_b,       0.02f, 0.5f, 1.0f, gpu_cb_update_beam_params, "%.2f");
    menu_phosphor[n++] = MI_CYCLIC("Subpixel",       &vc->tv.subpixel_layout,     0.0f, 2.0f, gpu_cb_update_beam_params, "OFF|RGB|BGR");

    /* ================================================================
     * Stage 13: CRT glass — halation, tint, barrel distortion
     * ================================================================ */
    n = 0;
    menu_glass[n++] = MI_FLOAT("Halation",      &vc->tv.halation,    0.02f, 0.0f, 0.4f, gpu_cb_update_beam_params, "%.2f");
    menu_glass[n++] = MI_FLOAT("Halo tint R",   &vc->tv.halation_tint_r, 0.05f, 0.0f, 2.0f, gpu_cb_update_beam_params, "%.2f");
    menu_glass[n++] = MI_FLOAT("Halo tint G",   &vc->tv.halation_tint_g, 0.05f, 0.0f, 2.0f, gpu_cb_update_beam_params, "%.2f");
    menu_glass[n++] = MI_FLOAT("Halo tint B",   &vc->tv.halation_tint_b, 0.05f, 0.0f, 2.0f, gpu_cb_update_beam_params, "%.2f");
    menu_glass[n++] = MI_FLOAT("Glass tint",    &vc->tv.glass_tint,  0.02f, 0.5f, 1.0f, gpu_cb_update_beam_params, "%.2f");
    menu_glass[n++] = MI_FLOAT("Barrel H",      &vc->tv.barrel,      0.005f, 0.0f, 0.15f, gpu_cb_update_beam_params, "%.3f");
    menu_glass[n++] = MI_FLOAT("Barrel V",      &vc->tv.barrel_v,    0.005f, 0.0f, 0.15f, gpu_cb_update_beam_params, "%.3f");
    menu_glass[n++] = MI_FLOAT("Keystone",      &vc->tv.keystone,    0.005f, -0.1f, 0.1f, gpu_cb_update_beam_params, "%+.3f");
    menu_glass[n++] = MI_FLOAT("Rotation",      &vc->tv.rotation,    0.002f, -0.05f, 0.05f, gpu_cb_update_beam_params, "%+.3f");
    menu_glass[n++] = MI_FLOAT("Skew X",        &vc->tv.skew_x,      0.005f, -0.1f, 0.1f, gpu_cb_update_beam_params, "%+.3f");
    menu_glass[n++] = MI_FLOAT("Skew Y",        &vc->tv.skew_y,      0.005f, -0.1f, 0.1f, gpu_cb_update_beam_params, "%+.3f");
    menu_glass[n++] = MI_FLOAT("HV sag",        &vc->tv.hv_sag,      0.02f, 0.0f, 0.5f, gpu_cb_update_beam_params, "%.2f");
    /* Service-menu raster controls (CRT HPOS/VPOS/HSIZE/VSIZE).
     * HSIZE/VSIZE < 1.0 shrinks the image inside the tube (you see the
     * physical beam edge); > 1.0 overscans off the visible tube face. */
    menu_glass[n++] = MI_FLOAT("H position",    &vc->tv.h_pos,       0.01f, -0.3f, 0.3f, gpu_cb_update_beam_params, "%+.2f");
    menu_glass[n++] = MI_FLOAT("V position",    &vc->tv.v_pos,       0.01f, -0.3f, 0.3f, gpu_cb_update_beam_params, "%+.2f");
    menu_glass[n++] = MI_FLOAT("H size",        &vc->tv.h_size,      0.02f, 0.5f, 1.5f, gpu_cb_update_beam_params, "%.2f");
    menu_glass[n++] = MI_FLOAT("V size",        &vc->tv.v_size,      0.02f, 0.5f, 1.5f, gpu_cb_update_beam_params, "%.2f");
    /* §6.3 Glass glare — external reflection of the viewer's room on
     * the outer glass face. Amount is zero by default so existing
     * presets don't gain a reflection they didn't ask for. */
    /* Glare amount step is deliberately large (0.03) so one press
     * produces a visible change — the shader's 2.5× Fresnel gain
     * makes values above ~0.05 clearly visible on any screen. */
    menu_glass[n++] = MI_FLOAT("Glare amount",  &vc->tv.glass_glare,         0.03f, 0.0f, 1.00f, gpu_cb_update_beam_params, "%.2f");
    menu_glass[n++] = MI_FLOAT("Glare light X", &vc->tv.glass_glare_light_x, 0.05f, 0.0f, 1.00f, gpu_cb_update_beam_params, "%.2f");
    menu_glass[n++] = MI_FLOAT("Glare light Y", &vc->tv.glass_glare_light_y, 0.05f, 0.0f, 1.00f, gpu_cb_update_beam_params, "%.2f");
    menu_glass[n++] = MI_FLOAT("Glare size",    &vc->tv.glass_glare_size,    0.02f, 0.02f, 0.60f, gpu_cb_update_beam_params, "%.2f");
    menu_glass[n++] = MI_FLOAT("Glare temp (K)",&vc->tv.glass_glare_temp_k,  200.0f, 0.0f, 10000.0f, gpu_cb_update_beam_params, "%.0f");

    /* ================================================================
     * Stage 14: Environment — vignette, ambient, noise, output
     * ================================================================ */
    n = 0;
    menu_env[n++] = MI_FLOAT("Vignette",      &vc->tv.vignette,       0.02f, 0.0f, 0.4f, gpu_cb_update_beam_params, "%.2f");
    menu_env[n++] = MI_FLOAT("Ambient",        &vc->tv.ambient_light,  0.01f, 0.0f, 0.25f, gpu_cb_update_beam_params, "%.2f");
    menu_env[n++] = MI_FLOAT("Black floor",    &vc->tv.black_floor,    0.005f, 0.0f, 0.10f, gpu_cb_update_beam_params, "%.3f");
    menu_env[n++] = MI_FLOAT("Noise level",    &vc->tv.noise_level,    0.005f, 0.0f, 0.10f, gpu_cb_update_beam_params, "%.3f");
    menu_env[n++] = MI_FLOAT("HDR gain",       &vc->tv.hdr_gain,       0.1f, 0.5f, 3.0f, gpu_cb_update_beam_params, "%.1f");

    /* ================================================================
     * APU analog (CPU-side DAC parameters)
     * ================================================================ */
    n = 0;
    menu_apu[n++] = MI_FLOAT("DAC nonlinear", &ctx->nes->apu.analog.dac_nonlinearity, 0.05f, 0.0f, 1.0f, gpu_cb_audio_prepare, "%.2f");
    menu_apu[n++] = MI_FLOAT("Saturation",    &ctx->nes->apu.analog.saturation,       0.05f, 0.0f, 1.0f, gpu_cb_audio_prepare, "%.2f");
    menu_apu[n++] = MI_FLOAT("Noise floor",   &ctx->nes->apu.analog.noise_floor,      0.001f, 0.0f, 0.03f, gpu_cb_audio_prepare, "%.3f");
    menu_apu[n++] = MI_FLOAT("60 Hz hum",     &ctx->nes->apu.analog.hum_60hz,         0.001f, 0.0f, 0.03f, gpu_cb_audio_prepare, "%.3f");
    menu_apu[n++] = MI_FLOAT("DMC crosstalk", &ctx->nes->apu.analog.dmc_bus_crosstalk, 0.05f, 0.0f, 1.0f, gpu_cb_audio_prepare, "%.2f");
    menu_apu[n++] = MI_FLOAT("Output gain",   &ctx->nes->apu.analog.output_gain,      0.05f, 0.1f, 3.0f, gpu_cb_audio_prepare, "%.2f");

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
    menu_audio_chain[n++] = MI_FLOAT("Coupling R",      &ac->coupling_cap.resistance, 5.0f, 10.0f, 200.0f, gpu_cb_audio_prepare, "%.0f");
    menu_audio_chain[n++] = MI_FLOAT("Amp BW R",        &ac->amp_bandwidth.resistance, 5.0f, 10.0f, 500.0f, gpu_cb_audio_prepare, "%.0f");

    /* ================================================================
     * Video submenu — organized by signal chain stage
     * ================================================================ */
    n = 0;
    menu_presets_video_idx = n;
    menu_video[n++] = MI_SUB("Presets",         menu_presets,      1 + preset_count);
    menu_video[n++] = MI_SUB("1. DAC",          menu_dac,          6);
    menu_video[n++] = MI_SUB("2. Console",      menu_console,      3);
    menu_video[n++] = MI_SUB("3. Cable",        menu_cable,        7);
    menu_video[n++] = MI_SUB("5. Comb filter",  menu_comb,         4);
    menu_video[n++] = MI_SUB("6. Chroma",       menu_chroma,       6);
    menu_video[n++] = MI_SUB("7. Luma",         menu_luma,         6);
    menu_video[n++] = MI_SUB("8. Color decode", menu_color_decode, 9);
    menu_video[n++] = MI_SUB("9. Video amp",    menu_video_amp,    4);
    menu_video[n++] = MI_SUB("10. Beam",        menu_beam,         30);
    menu_video[n++] = MI_SUB("11. Phosphor",    menu_phosphor,     8);
    menu_video[n++] = MI_SUB("12. Glass",       menu_glass,        21);
    menu_video[n++] = MI_SUB("13. Environment", menu_env,          5);
    int video_count = n;

    /* ================================================================
     * Audio submenu
     * ================================================================ */
    n = 0;
    menu_audio_top[n++] = MI_SUB("APU DAC",      menu_apu,         6);
    menu_audio_top[n++] = MI_SUB("Chain stages",  menu_audio_chain, 8);

    /* ================================================================
     * Root menu
     * ================================================================ */
    preset_menu_root[0] = MI_SUB("Video",   menu_video,    video_count);
    preset_menu_root[1] = MI_SUB("Audio",   menu_audio_top, 2);
    preset_menu_root[2] = MI_SUB("Presets", menu_presets,   1 + preset_count);
}

/* ============================================================================
 * Overlay compositing (chain vis only — OSD rendered in main.c)
 * ============================================================================ */

void preset_composite_overlays(PresetCtx *ctx) {
    const uint8_t (*pal)[3] = ctx->nes->ppu.color_palette
                              ? ctx->nes->ppu.color_palette
                              : ppu_palette_2c02;

    /* Chain visualiser overlay. */
    if (chain_vis_is_open(ctx->chain_vis)) {
        chain_vis_update(ctx->chain_vis, *ctx->current_preset);
        int ov_w, ov_h;
        const uint8_t *ov = chain_vis_get_overlay(ctx->chain_vis, &ov_w, &ov_h);
        if (ov) {
            uint8_t *rgb = ctx->nes->ppu.framebuffer;
            uint16_t *idx = ctx->nes->ppu.index_framebuffer;
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

/* The editor uses the same physical values and update paths as the OSD. */
void preset_register_debug_controls(PresetCtx *ctx, DebugServer *server) {
    TVDisplayParams *tv = &ctx->video_chain->tv;
    DebugControl controls[] = {
        {"Luma bandwidth (Hz)", "Decoder", &tv->luma_bandwidth, 500000, 8000000, gpu_cb_redesign_firs},
        {"Chroma bandwidth (Hz)", "Decoder", &tv->chroma_bandwidth, 100000, 3000000, gpu_cb_redesign_firs},
        {"Saturation", "Decoder", &tv->saturation, 0, 2, gpu_cb_update_color_matrix},
        {"Hue (degrees)", "Decoder", &tv->hue_offset, -180, 180, gpu_cb_update_color_matrix},
        {"Sharpness", "Beam", &tv->beam_sharpness, 0, 1, gpu_cb_update_beam_params},
        {"Dark beam height", "Beam", &tv->beam_height_min, 0.1f, 2, gpu_cb_update_beam_params},
        {"Bright beam height", "Beam", &tv->beam_height_max, 0.1f, 3, gpu_cb_update_beam_params},
        {"Persistence (ms)", "Phosphor", &tv->persistence_ms, 0, 100, gpu_cb_update_beam_params},
        {"Red lifetime scale", "Phosphor", &tv->persistence_r, 0, 1, gpu_cb_update_beam_params},
        {"Green lifetime scale", "Phosphor", &tv->persistence_g, 0, 1, gpu_cb_update_beam_params},
        {"Blue lifetime scale", "Phosphor", &tv->persistence_b, 0, 1, gpu_cb_update_beam_params},
        {"Mask pitch (pixels)", "Phosphor", &tv->mask_pitch_px, 1, 12, NULL},
        {"Linear brightness", "Phosphor", &tv->hdr_gain, 0.5f, 3, NULL},
        {"Mask strength", "Phosphor", &tv->mask_strength, 0, 1, NULL},
        {"Glass curvature", "Glass", &tv->barrel, 0, 0.1f, NULL},
        {"Halation", "Glass", &tv->halation, 0, 0.5f, NULL},
        {"Room light", "Glass", &tv->ambient_light, 0, 0.2f, NULL},
        {"Cable length (m)", "Connection", &ctx->video_chain->cable.length_meters, 0, 20, gpu_cb_update_rc_params},
        {"RF hum", "Connection", &ctx->video_chain->console_psu_hum, 0, 0.1f, gpu_cb_reinit_stages},
        {"RF noise floor (dBm)", "Connection", &ctx->video_chain->rf.noise_floor_dbm, -90, -30, gpu_cb_reinit_stages},
    };
    debug_server_set_controls(server, controls, (int)(sizeof(controls)/sizeof(controls[0])));
}
