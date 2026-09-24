/*
 * JSON Preset Save/Load Tests
 * ============================
 *
 * Exercises the preset_json.h header-only module:
 *   - Round-trip fidelity for every field in PhysicalPreset
 *   - Every built-in preset round-trips bit-for-bit (within float tolerance)
 *   - Enums serialise as strings ("composite", "1line", "shadow")
 *   - String enum values parse correctly on load
 *   - Backward compatibility with numeric enum values
 *   - Missing fields default to zero (memset contract)
 *   - Malformed inputs fail gracefully (no crashes)
 *   - Directory scan picks up only .json files
 *   - String escapes ("\"", "\\") round-trip through save/load
 *   - Every enum value of every enum type round-trips
 *
 * Build: gcc -O2 -lm -I.. test_preset_json.c ../presets.c -o test_preset_json
 * Run:   ./test_preset_json
 */

#include "../preset_json.h"
#include <stdio.h>
#include <string.h>
#include <math.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>

static int tests_run = 0;
static int tests_passed = 0;

#define ASSERT_NEAR(actual, expected, tol, msg) do { \
    float _a = (float)(actual), _e = (float)(expected), _t = (float)(tol); \
    if (fabsf(_a - _e) > _t) { \
        printf("  FAIL: %s: got %.6f, expected %.6f (tol %.6f)\n", msg, _a, _e, _t); \
        return 0; \
    } \
} while(0)

#define ASSERT_EQ_INT(actual, expected, msg) do { \
    long _a = (long)(actual), _e = (long)(expected); \
    if (_a != _e) { \
        printf("  FAIL: %s: got %ld, expected %ld\n", msg, _a, _e); \
        return 0; \
    } \
} while(0)

#define ASSERT_EQ_STR(actual, expected, msg) do { \
    const char *_a = (actual), *_e = (expected); \
    if (!_a || !_e || strcmp(_a, _e) != 0) { \
        printf("  FAIL: %s: got \"%s\", expected \"%s\"\n", msg, \
               _a ? _a : "(null)", _e ? _e : "(null)"); \
        return 0; \
    } \
} while(0)

#define ASSERT_TRUE(cond, msg) do { \
    if (!(cond)) { printf("  FAIL: %s\n", msg); return 0; } \
} while(0)

#define ASSERT_FALSE(cond, msg) do { \
    if (cond) { printf("  FAIL: %s\n", msg); return 0; } \
} while(0)

#define RUN_TEST(fn) do { \
    tests_run++; \
    printf("TEST %s: ", #fn); \
    if (fn()) { tests_passed++; printf("PASS\n"); } \
    else { printf("FAILED\n"); } \
} while(0)

#define FLOAT_TOL 1e-4f

/* ============================================================================
 * Helpers
 * ============================================================================ */

/* Populate a preset with distinctive non-default values. Every field gets a
 * unique value so we can detect per-field corruption. */
static void make_distinctive_preset(PhysicalPreset *p)
{
    memset(p, 0, sizeof(*p));

    strncpy(p->name,        "Distinctive Test Preset",
            sizeof(p->name) - 1);
    strncpy(p->description, "Every field has a unique recognisable value",
            sizeof(p->description) - 1);

    p->connection      = VIDEO_CONN_SVIDEO;
    p->comb_type       = VIDEO_COMB_3LINE;
    p->console_variant = PRESET_CONSOLE_NES_TOP;
    p->speaker_type    = AUDIO_SPEAKER_PVM;
    p->region          = SIGNAL_REGION_PAL;

    /* Video cable. */
    p->video_cable.length_meters        = 3.14f;
    p->video_cable.resistance_per_m     = 0.17f;
    p->video_cable.capacitance_per_m    = 77e-12f;
    p->video_cable.num_sections         = 5;
    p->video_cable.connector_resistance = 0.42f;
    p->video_cable.impedance            = 75.5f;
    p->video_cable.shield_effectiveness = 0.91f;
    p->video_cable.ghost_delay          = 13;
    p->video_cable.ghost_level          = 0.055f;

    /* Audio cable. */
    p->audio_cable.length_meters        = 2.71f;
    p->audio_cable.resistance_per_m     = 0.22f;
    p->audio_cable.capacitance_per_m    = 63e-12f;
    p->audio_cable.num_sections         = 3;
    p->audio_cable.connector_resistance = 0.33f;
    p->audio_cable.impedance            = 74.5f;
    p->audio_cable.shield_effectiveness = 0.77f;
    p->audio_cable.ghost_delay          = 7;
    p->audio_cable.ghost_level          = 0.022f;

    /* TV / CRT display. */
    p->tv.chroma_bandwidth  = 750000.0f;
    p->tv.luma_bandwidth    = 5250000.0f;
    p->tv.hue_offset        = 3.5f;
    p->tv.saturation        = 1.15f;
    p->tv.fir_ringing       = 0.25f;
    p->tv.luma_peaking      = 0.45f;
    p->tv.aperture_max_db = 6.0f;
    p->tv.h_afc_tau_ms = 1.0f;
    p->tv.rf_interference   = 0.08f;
    p->tv.geometry_warp     = 2.5f;

    p->tv.color_temperature = 7500.0f;
    p->tv.decoder_red_gain=.17f; p->tv.decoder_blue_gain=-.03f;
    p->tv.monitor_model=1; p->tv.phosphor_gamut=3; p->tv.beam_spot_growth=.4f;
    p->tv.r_drive = 1.05f;
    p->tv.g_drive = 0.98f;
    p->tv.b_drive = 1.03f;
    p->tv.r_cutoff = 0.01f;
    p->tv.g_cutoff = -0.005f;
    p->tv.b_cutoff = 0.02f;

    p->tv.rgb_bandwidth_3db=1;
    p->tv.r_bandwidth = 5600000.0f;
    p->tv.g_bandwidth = 5900000.0f;
    p->tv.b_bandwidth = 5300000.0f;
    p->tv.gamma       = 2.35f;

    p->tv.beam_sharpness      = 0.65f;
    p->tv.beam_height_min     = 0.55f;
    p->tv.beam_height_max     = 0.85f;
    p->tv.beam_spot_size      = 4.2f;
    p->tv.beam_fwhm_min = .37f; p->tv.beam_fwhm_max = .89f;
    p->tv.convergence_static  = 0.18f;
    p->tv.convergence_dynamic = 0.11f;
    p->tv.conv_r_x = 2.8f;
    p->tv.conv_r_y = 0.9f;
    p->tv.conv_b_x = -2.3f;
    p->tv.conv_b_y = -0.7f;
    p->tv.h_jitter = 0.35f;
    p->tv.v_jitter = 0.0025f;

    p->tv.mask_type       = VIDEO_MASK_APERTURE_GRILLE;
    p->tv.mask_triads     = 550.0f;
    p->tv.mask_pitch_px   = 2.5f;
    p->tv.mask_strength   = 0.66f;
    p->tv.subpixel_layout = 2;
    p->tv.persistence_tail_ms = 12.5;
    p->tv.persistence_tail_weight = 0.025;
    p->rf.if_asymmetry = 0.8;
    p->rf.tuning_offset_hz = 75000;
    p->vhs.enabled = 1;
    p->vhs.model = 2;
    p->vhs.white_clip_pct = 190;
    p->vhs.dark_clip_pct = 35;
    p->vhs.fm_sync_hz = 3.5e6f;
    p->vhs.fm_white_hz = 4.6e6f;
    p->vhs.rf_cnr_dbhz = 93.25f;
    p->vhs.tape_tilt_db_per_mhz = -1.5f;
    p->vhs.mod_noise_hz = 2500;
    p->vhs.head_b_noise_db = 0.6f;
    p->vhs.chroma_noise_ire = 0.9f;
    p->vhs.dropout_scale = 2.5f;
    p->vhs.doc = 1;
    p->vhs.doc_threshold_db = -12;
    p->vhs.canceller_split_hz = 7e5f;
    p->vhs.canceller_limit_ire = 4;
    p->vhs.sharpness = 0.3f;
    p->vhs.detail_limit_ire = 12;
    p->vhs.apc_loop_hz = 800;
    p->vhs.yc_delay_ns = -40;
    p->vhs.bow_scale = 1.5f;
    p->vhs.tbe_varying_ns = 60;
    p->vhs.tbe_slow_fraction = 0.3f;
    p->vhs.tbe_slow_tau_ms = 500;
    p->vhs.line_jitter_ns = 7;
    p->vhs.switch_lines_before_vsync = 7;
    p->vhs.skew_ba_ns = 1500;
    p->vhs.skew_ab_ns = -120;
    p->vhs.deck_seed = 7;
    p->tv.h_pll_hz = 300;
    p->tv.h_pll_damping = 0.8f;
    p->tv.h_pll_vblank_gain = 2.0f;

    p->tv.persistence_ms  = 1.7f;

    p->tv.halation   = 0.09f;
    p->tv.halation_sigma = 0.025451f;
    p->tv.glass_tint = 0.78f;
    p->tv.barrel     = 0.045f;

    p->tv.vignette      = 0.12f;
    p->tv.ambient_light = 0.09f;

    p->tv.black_floor = 0.025f;
    p->tv.noise_level = 0.04f;
    p->tv.hum_bar_amplitude = 0.03f;
    p->tv.bloom_gamma = 1.6f;
    p->tv.edge_focus = 0.22f;
    p->tv.velocity_dim = 0.12f;
    p->tv.barrel_v = 0.055f;
    p->tv.motion_threshold = 0.07f;
    p->tv.persistence_r = 0.88f;
    p->tv.persistence_g = 1.0f;
    p->tv.persistence_b = 0.71f;
    p->tv.color_killer = 0.04f;
    p->tv.hdr_gain = 1.3f;
    p->tv.luma_peaking = 0.18f;
    p->tv.overscan = 0.05f;
    p->tv.keystone = 0.03f;
    p->tv.rotation = 0.012f;
    p->tv.skew_x = -0.02f;
    p->tv.skew_y = 0.015f;
    p->tv.hv_sag = 0.22f;
    p->tv.focus_breathing = 0.14f;
    p->tv.video_black_droop = 0.18f;
    p->tv.video_recovery_us = 18;
    p->tv.scanline_wobble = 0.35f;
    p->tv.top_band_shift = 3.5f;
    p->tv.top_edge_skew = -2.25f;
    p->tv.top_band_start = 18.0f;
    p->tv.top_band_end = 34.0f;
    p->tv.top_edge_width = 0.09f;

    /* Console output stage. */
    p->console_coupling_R = 75.0f;
    p->console_coupling_C = 12e-6f;
    p->console_phase_distortion_ns = 27.0f;
    p->console_amp_bw     = 15500.0f;
    p->console_psu_hum    = 0.004f;

    /* RF. */
    p->rf.carrier_level_dbm=-23;
    p->rf.enabled         = true;
    p->rf.carrier_freq    = 61250000.0f;
    p->rf.mod_bandwidth   = 3000000.0f;
    p->rf.noise_floor_dbm = -65.5f;
    p->rf.agc_attack_ms   = 12.5f;
    p->rf.agc_release_ms  = 120.0f;

    /* Signal decode overrides. */
    p->brightness  = 0.03f;
    p->contrast    = 0.96f;
    p->chroma_gain = 1.22f;

    /* Console supply and audio overrides. */
    p->psu.adaptor_vac = 9.5f;
    p->psu.reservoir_uf = 1000.0f;
    p->psu.load_ma = 700.0f;
    p->psu.regulator_rejection_db = 62.0f;
    p->rf.sound_am_rejection_db = 40.0f;
    p->rf.icpm_deg = 6.0f;
    p->audio_noise_floor       = 0.0015f;
    p->audio_saturation_drive  = 1.4f;
    p->audio_cable_length_m    = 2.5f;
}

/* Compare two presets field-by-field. Returns 1 if equal within tolerance,
 * 0 on mismatch (with a printed FAIL message). */
static int compare_presets(const PhysicalPreset *a, const PhysicalPreset *b,
                           const char *context)
{
    (void)context;

    ASSERT_NEAR(b->rf.carrier_level_dbm,a->rf.carrier_level_dbm,FLOAT_TOL,"RF carrier level");
    ASSERT_NEAR(b->tv.decoder_red_gain,a->tv.decoder_red_gain,FLOAT_TOL,"decoder red gain");
    ASSERT_NEAR(b->tv.decoder_blue_gain,a->tv.decoder_blue_gain,FLOAT_TOL,"decoder blue gain");
    ASSERT_NEAR(b->tv.beam_spot_growth,a->tv.beam_spot_growth,FLOAT_TOL,"beam spot growth");
    ASSERT_EQ_INT(b->tv.monitor_model,a->tv.monitor_model,"monitor model");
    ASSERT_EQ_INT(b->tv.phosphor_gamut,a->tv.phosphor_gamut,"phosphor gamut");
    ASSERT_EQ_INT(b->tv.rgb_bandwidth_3db,a->tv.rgb_bandwidth_3db,"RGB bandwidth definition");
    /* Identity. */
    ASSERT_EQ_STR(b->name, a->name, "name");
    ASSERT_EQ_STR(b->description, a->description, "description");

    /* Topology. */
    ASSERT_EQ_INT(b->connection,      a->connection,      "connection");
    ASSERT_EQ_INT(b->comb_type,       a->comb_type,       "comb_type");
    ASSERT_EQ_INT(b->console_variant, a->console_variant, "console_variant");
    ASSERT_EQ_INT(b->speaker_type,    a->speaker_type,    "speaker_type");
    ASSERT_EQ_INT(b->region,          a->region,          "region");

    /* Video cable. */
    ASSERT_NEAR(b->video_cable.length_meters,        a->video_cable.length_meters,        FLOAT_TOL, "video_cable.length_meters");
    ASSERT_NEAR(b->video_cable.resistance_per_m,     a->video_cable.resistance_per_m,     FLOAT_TOL, "video_cable.resistance_per_m");
    ASSERT_NEAR(b->video_cable.capacitance_per_m * 1e12f, a->video_cable.capacitance_per_m * 1e12f, FLOAT_TOL, "video_cable.capacitance_per_m");
    ASSERT_EQ_INT(b->video_cable.num_sections,       a->video_cable.num_sections,                    "video_cable.num_sections");
    ASSERT_NEAR(b->video_cable.connector_resistance, a->video_cable.connector_resistance, FLOAT_TOL, "video_cable.connector_resistance");
    ASSERT_NEAR(b->video_cable.impedance,            a->video_cable.impedance,            FLOAT_TOL, "video_cable.impedance");
    ASSERT_NEAR(b->video_cable.shield_effectiveness, a->video_cable.shield_effectiveness, FLOAT_TOL, "video_cable.shield_effectiveness");
    ASSERT_EQ_INT(b->video_cable.ghost_delay,        a->video_cable.ghost_delay,                     "video_cable.ghost_delay");
    ASSERT_NEAR(b->video_cable.ghost_level,          a->video_cable.ghost_level,          FLOAT_TOL, "video_cable.ghost_level");

    /* Audio cable. */
    ASSERT_NEAR(b->audio_cable.length_meters,        a->audio_cable.length_meters,        FLOAT_TOL, "audio_cable.length_meters");
    ASSERT_NEAR(b->audio_cable.resistance_per_m,     a->audio_cable.resistance_per_m,     FLOAT_TOL, "audio_cable.resistance_per_m");
    ASSERT_NEAR(b->audio_cable.capacitance_per_m * 1e12f, a->audio_cable.capacitance_per_m * 1e12f, FLOAT_TOL, "audio_cable.capacitance_per_m");
    ASSERT_EQ_INT(b->audio_cable.num_sections,       a->audio_cable.num_sections,                    "audio_cable.num_sections");
    ASSERT_NEAR(b->audio_cable.connector_resistance, a->audio_cable.connector_resistance, FLOAT_TOL, "audio_cable.connector_resistance");
    ASSERT_NEAR(b->audio_cable.impedance,            a->audio_cable.impedance,            FLOAT_TOL, "audio_cable.impedance");
    ASSERT_NEAR(b->audio_cable.shield_effectiveness, a->audio_cable.shield_effectiveness, FLOAT_TOL, "audio_cable.shield_effectiveness");
    ASSERT_EQ_INT(b->audio_cable.ghost_delay,        a->audio_cable.ghost_delay,                     "audio_cable.ghost_delay");
    ASSERT_NEAR(b->audio_cable.ghost_level,          a->audio_cable.ghost_level,          FLOAT_TOL, "audio_cable.ghost_level");

    /* TV -- bandwidths stored with 1 decimal, so tol is looser. */
    ASSERT_NEAR(b->tv.chroma_bandwidth,   a->tv.chroma_bandwidth,   1.0f,      "tv.chroma_bandwidth");
    ASSERT_NEAR(b->tv.luma_bandwidth,     a->tv.luma_bandwidth,     1.0f,      "tv.luma_bandwidth");
    ASSERT_NEAR(b->tv.hue_offset,         a->tv.hue_offset,         FLOAT_TOL, "tv.hue_offset");
    ASSERT_NEAR(b->tv.saturation,         a->tv.saturation,         FLOAT_TOL, "tv.saturation");
    ASSERT_NEAR(b->tv.fir_ringing,        a->tv.fir_ringing,        FLOAT_TOL, "tv.fir_ringing");
    ASSERT_NEAR(b->tv.rf_interference,    a->tv.rf_interference,    FLOAT_TOL, "tv.rf_interference");
    ASSERT_NEAR(b->tv.geometry_warp,      a->tv.geometry_warp,      FLOAT_TOL, "tv.geometry_warp");

    ASSERT_NEAR(b->tv.color_temperature,  a->tv.color_temperature,  1.0f,      "tv.color_temperature");
    ASSERT_NEAR(b->tv.r_drive,            a->tv.r_drive,            FLOAT_TOL, "tv.r_drive");
    ASSERT_NEAR(b->tv.g_drive,            a->tv.g_drive,            FLOAT_TOL, "tv.g_drive");
    ASSERT_NEAR(b->tv.b_drive,            a->tv.b_drive,            FLOAT_TOL, "tv.b_drive");
    ASSERT_NEAR(b->tv.r_cutoff,           a->tv.r_cutoff,           FLOAT_TOL, "tv.r_cutoff");
    ASSERT_NEAR(b->tv.g_cutoff,           a->tv.g_cutoff,           FLOAT_TOL, "tv.g_cutoff");
    ASSERT_NEAR(b->tv.b_cutoff,           a->tv.b_cutoff,           FLOAT_TOL, "tv.b_cutoff");

    ASSERT_NEAR(b->tv.r_bandwidth,        a->tv.r_bandwidth,        1.0f,      "tv.r_bandwidth");
    ASSERT_NEAR(b->tv.g_bandwidth,        a->tv.g_bandwidth,        1.0f,      "tv.g_bandwidth");
    ASSERT_NEAR(b->tv.b_bandwidth,        a->tv.b_bandwidth,        1.0f,      "tv.b_bandwidth");
    ASSERT_NEAR(b->tv.gamma,              a->tv.gamma,              FLOAT_TOL, "tv.gamma");

    ASSERT_NEAR(b->tv.beam_sharpness,     a->tv.beam_sharpness,     FLOAT_TOL, "tv.beam_sharpness");
    ASSERT_NEAR(b->tv.beam_height_min,    a->tv.beam_height_min,    FLOAT_TOL, "tv.beam_height_min");
    ASSERT_NEAR(b->tv.beam_height_max,    a->tv.beam_height_max,    FLOAT_TOL, "tv.beam_height_max");
    ASSERT_NEAR(b->tv.beam_fwhm_min,a->tv.beam_fwhm_min,FLOAT_TOL,"tv.beam_fwhm_min");
    ASSERT_NEAR(b->tv.beam_fwhm_max,a->tv.beam_fwhm_max,FLOAT_TOL,"tv.beam_fwhm_max");
    ASSERT_NEAR(b->tv.beam_spot_size,     a->tv.beam_spot_size,     FLOAT_TOL, "tv.beam_spot_size");
    ASSERT_NEAR(b->tv.convergence_static, a->tv.convergence_static, FLOAT_TOL, "tv.convergence_static");
    ASSERT_NEAR(b->tv.convergence_dynamic,a->tv.convergence_dynamic,FLOAT_TOL, "tv.convergence_dynamic");
    ASSERT_NEAR(b->tv.conv_r_x,           a->tv.conv_r_x,           FLOAT_TOL, "tv.conv_r_x");
    ASSERT_NEAR(b->tv.conv_r_y,           a->tv.conv_r_y,           FLOAT_TOL, "tv.conv_r_y");
    ASSERT_NEAR(b->tv.conv_b_x,           a->tv.conv_b_x,           FLOAT_TOL, "tv.conv_b_x");
    ASSERT_NEAR(b->tv.conv_b_y,           a->tv.conv_b_y,           FLOAT_TOL, "tv.conv_b_y");
    ASSERT_NEAR(b->tv.h_jitter,           a->tv.h_jitter,           FLOAT_TOL, "tv.h_jitter");
    ASSERT_NEAR(b->tv.v_jitter,           a->tv.v_jitter,           FLOAT_TOL, "tv.v_jitter");

    ASSERT_EQ_INT(b->tv.mask_type,        a->tv.mask_type,                     "tv.mask_type");
    ASSERT_NEAR(b->tv.mask_pitch_px,      a->tv.mask_pitch_px,      FLOAT_TOL, "tv.mask_pitch_px");
    ASSERT_NEAR(b->tv.mask_triads,      a->tv.mask_triads,      FLOAT_TOL, "tv.mask_triads");
    ASSERT_NEAR(b->tv.mask_strength,      a->tv.mask_strength,      FLOAT_TOL, "tv.mask_strength");
    ASSERT_EQ_INT(b->tv.subpixel_layout,  a->tv.subpixel_layout,               "tv.subpixel_layout");
    ASSERT_NEAR(b->tv.persistence_tail_ms,a->tv.persistence_tail_ms,FLOAT_TOL,"tv.persistence_tail_ms");
    ASSERT_NEAR(b->tv.persistence_tail_weight,a->tv.persistence_tail_weight,FLOAT_TOL,"tv.persistence_tail_weight");
    ASSERT_NEAR(b->rf.if_asymmetry,a->rf.if_asymmetry,FLOAT_TOL,"rf.if_asymmetry");
    ASSERT_NEAR(b->rf.tuning_offset_hz,a->rf.tuning_offset_hz,FLOAT_TOL,"rf.tuning_offset_hz");
    ASSERT_EQ_INT(b->vhs.enabled,a->vhs.enabled,"vhs.enabled");
    ASSERT_EQ_INT(b->vhs.model,a->vhs.model,"vhs.model");
    ASSERT_EQ_INT(b->vhs.doc,a->vhs.doc,"vhs.doc");
    ASSERT_EQ_INT(b->vhs.deck_seed,a->vhs.deck_seed,"vhs.deck_seed");
    ASSERT_NEAR(b->vhs.white_clip_pct,a->vhs.white_clip_pct,FLOAT_TOL,"vhs.white_clip_pct");
    ASSERT_NEAR(b->vhs.dark_clip_pct,a->vhs.dark_clip_pct,FLOAT_TOL,"vhs.dark_clip_pct");
    ASSERT_NEAR(b->vhs.fm_sync_hz,a->vhs.fm_sync_hz,FLOAT_TOL,"vhs.fm_sync_hz");
    ASSERT_NEAR(b->vhs.fm_white_hz,a->vhs.fm_white_hz,FLOAT_TOL,"vhs.fm_white_hz");
    ASSERT_NEAR(b->vhs.rf_cnr_dbhz,a->vhs.rf_cnr_dbhz,FLOAT_TOL,"vhs.rf_cnr_dbhz");
    ASSERT_NEAR(b->vhs.tape_tilt_db_per_mhz,a->vhs.tape_tilt_db_per_mhz,FLOAT_TOL,"vhs.tape_tilt_db_per_mhz");
    ASSERT_NEAR(b->vhs.mod_noise_hz,a->vhs.mod_noise_hz,FLOAT_TOL,"vhs.mod_noise_hz");
    ASSERT_NEAR(b->vhs.head_b_noise_db,a->vhs.head_b_noise_db,FLOAT_TOL,"vhs.head_b_noise_db");
    ASSERT_NEAR(b->vhs.chroma_noise_ire,a->vhs.chroma_noise_ire,FLOAT_TOL,"vhs.chroma_noise_ire");
    ASSERT_NEAR(b->vhs.dropout_scale,a->vhs.dropout_scale,FLOAT_TOL,"vhs.dropout_scale");
    ASSERT_NEAR(b->vhs.doc_threshold_db,a->vhs.doc_threshold_db,FLOAT_TOL,"vhs.doc_threshold_db");
    ASSERT_NEAR(b->vhs.canceller_split_hz,a->vhs.canceller_split_hz,FLOAT_TOL,"vhs.canceller_split_hz");
    ASSERT_NEAR(b->vhs.canceller_limit_ire,a->vhs.canceller_limit_ire,FLOAT_TOL,"vhs.canceller_limit_ire");
    ASSERT_NEAR(b->vhs.sharpness,a->vhs.sharpness,FLOAT_TOL,"vhs.sharpness");
    ASSERT_NEAR(b->vhs.detail_limit_ire,a->vhs.detail_limit_ire,FLOAT_TOL,"vhs.detail_limit_ire");
    ASSERT_NEAR(b->vhs.apc_loop_hz,a->vhs.apc_loop_hz,FLOAT_TOL,"vhs.apc_loop_hz");
    ASSERT_NEAR(b->vhs.yc_delay_ns,a->vhs.yc_delay_ns,FLOAT_TOL,"vhs.yc_delay_ns");
    ASSERT_NEAR(b->vhs.bow_scale,a->vhs.bow_scale,FLOAT_TOL,"vhs.bow_scale");
    ASSERT_NEAR(b->vhs.tbe_varying_ns,a->vhs.tbe_varying_ns,FLOAT_TOL,"vhs.tbe_varying_ns");
    ASSERT_NEAR(b->vhs.tbe_slow_fraction,a->vhs.tbe_slow_fraction,FLOAT_TOL,"vhs.tbe_slow_fraction");
    ASSERT_NEAR(b->vhs.tbe_slow_tau_ms,a->vhs.tbe_slow_tau_ms,FLOAT_TOL,"vhs.tbe_slow_tau_ms");
    ASSERT_NEAR(b->vhs.line_jitter_ns,a->vhs.line_jitter_ns,FLOAT_TOL,"vhs.line_jitter_ns");
    ASSERT_NEAR(b->vhs.switch_lines_before_vsync,a->vhs.switch_lines_before_vsync,FLOAT_TOL,"vhs.switch_lines_before_vsync");
    ASSERT_NEAR(b->vhs.skew_ba_ns,a->vhs.skew_ba_ns,FLOAT_TOL,"vhs.skew_ba_ns");
    ASSERT_NEAR(b->vhs.skew_ab_ns,a->vhs.skew_ab_ns,FLOAT_TOL,"vhs.skew_ab_ns");
    ASSERT_NEAR(b->tv.h_pll_hz,a->tv.h_pll_hz,FLOAT_TOL,"tv.h_pll_hz");
    ASSERT_NEAR(b->tv.h_pll_damping,a->tv.h_pll_damping,FLOAT_TOL,"tv.h_pll_damping");
    ASSERT_NEAR(b->tv.h_pll_vblank_gain,a->tv.h_pll_vblank_gain,FLOAT_TOL,"tv.h_pll_vblank_gain");

    ASSERT_NEAR(b->tv.persistence_ms,     a->tv.persistence_ms,     FLOAT_TOL, "tv.persistence_ms");

    ASSERT_NEAR(b->tv.halation,           a->tv.halation,           FLOAT_TOL, "tv.halation");
    ASSERT_NEAR(b->tv.halation_sigma,     a->tv.halation_sigma,     FLOAT_TOL, "tv.halation_sigma");
    ASSERT_NEAR(b->tv.glass_tint,         a->tv.glass_tint,         FLOAT_TOL, "tv.glass_tint");
    ASSERT_NEAR(b->tv.barrel,             a->tv.barrel,             FLOAT_TOL, "tv.barrel");

    ASSERT_NEAR(b->tv.vignette,           a->tv.vignette,           FLOAT_TOL, "tv.vignette");
    ASSERT_NEAR(b->tv.ambient_light,      a->tv.ambient_light,      FLOAT_TOL, "tv.ambient_light");

    ASSERT_NEAR(b->tv.black_floor,        a->tv.black_floor,        FLOAT_TOL, "tv.black_floor");
    ASSERT_NEAR(b->tv.noise_level,        a->tv.noise_level,        FLOAT_TOL, "tv.noise_level");
    ASSERT_NEAR(b->tv.hum_bar_amplitude,  a->tv.hum_bar_amplitude,  FLOAT_TOL, "tv.hum_bar_amplitude");
    ASSERT_NEAR(b->tv.bloom_gamma,        a->tv.bloom_gamma,        FLOAT_TOL, "tv.bloom_gamma");
    ASSERT_NEAR(b->tv.edge_focus,         a->tv.edge_focus,         FLOAT_TOL, "tv.edge_focus");
    ASSERT_NEAR(b->tv.velocity_dim,       a->tv.velocity_dim,       FLOAT_TOL, "tv.velocity_dim");
    ASSERT_NEAR(b->tv.barrel_v,           a->tv.barrel_v,           FLOAT_TOL, "tv.barrel_v");
    ASSERT_NEAR(b->tv.motion_threshold,   a->tv.motion_threshold,   FLOAT_TOL, "tv.motion_threshold");
    ASSERT_NEAR(b->tv.persistence_r,      a->tv.persistence_r,      FLOAT_TOL, "tv.persistence_r");
    ASSERT_NEAR(b->tv.persistence_g,      a->tv.persistence_g,      FLOAT_TOL, "tv.persistence_g");
    ASSERT_NEAR(b->tv.persistence_b,      a->tv.persistence_b,      FLOAT_TOL, "tv.persistence_b");
    ASSERT_NEAR(b->tv.color_killer,       a->tv.color_killer,       FLOAT_TOL, "tv.color_killer");
    ASSERT_NEAR(b->tv.hdr_gain,           a->tv.hdr_gain,           FLOAT_TOL, "tv.hdr_gain");
    ASSERT_NEAR(b->tv.luma_peaking,       a->tv.luma_peaking,       FLOAT_TOL, "tv.luma_peaking");
    ASSERT_NEAR(b->tv.aperture_max_db,       a->tv.aperture_max_db,       FLOAT_TOL, "tv.aperture_max_db");
    ASSERT_NEAR(b->tv.h_afc_tau_ms,       a->tv.h_afc_tau_ms,       FLOAT_TOL, "tv.h_afc_tau_ms");
    ASSERT_NEAR(b->tv.overscan,           a->tv.overscan,           FLOAT_TOL, "tv.overscan");
    ASSERT_NEAR(b->tv.keystone,           a->tv.keystone,           FLOAT_TOL, "tv.keystone");
    ASSERT_NEAR(b->tv.rotation,           a->tv.rotation,           FLOAT_TOL, "tv.rotation");
    ASSERT_NEAR(b->tv.skew_x,             a->tv.skew_x,             FLOAT_TOL, "tv.skew_x");
    ASSERT_NEAR(b->tv.skew_y,             a->tv.skew_y,             FLOAT_TOL, "tv.skew_y");
    ASSERT_NEAR(b->tv.hv_sag,             a->tv.hv_sag,             FLOAT_TOL, "tv.hv_sag");
    ASSERT_NEAR(b->tv.focus_breathing,    a->tv.focus_breathing,    FLOAT_TOL, "tv.focus_breathing");
    ASSERT_NEAR(b->tv.video_black_droop,  a->tv.video_black_droop,  FLOAT_TOL, "tv.video_black_droop");
    ASSERT_NEAR(b->tv.video_recovery_us,  a->tv.video_recovery_us,  FLOAT_TOL, "tv.video_recovery_us");
    ASSERT_NEAR(b->tv.scanline_wobble,    a->tv.scanline_wobble,    FLOAT_TOL, "tv.scanline_wobble");
    ASSERT_NEAR(b->tv.top_band_shift,     a->tv.top_band_shift,     FLOAT_TOL, "tv.top_band_shift");
    ASSERT_NEAR(b->tv.top_edge_skew,      a->tv.top_edge_skew,      FLOAT_TOL, "tv.top_edge_skew");
    ASSERT_NEAR(b->tv.top_band_start,     a->tv.top_band_start,     FLOAT_TOL, "tv.top_band_start");
    ASSERT_NEAR(b->tv.top_band_end,       a->tv.top_band_end,       FLOAT_TOL, "tv.top_band_end");
    ASSERT_NEAR(b->tv.top_edge_width,     a->tv.top_edge_width,     FLOAT_TOL, "tv.top_edge_width");

    /* Console output stage. */
    ASSERT_NEAR(b->console_coupling_R,        a->console_coupling_R,        FLOAT_TOL, "console_coupling_R");
    ASSERT_NEAR(b->console_coupling_C * 1e6f, a->console_coupling_C * 1e6f, FLOAT_TOL, "console_coupling_C");
    ASSERT_NEAR(b->console_amp_bw,            a->console_amp_bw,            1.0f,      "console_amp_bw");
    ASSERT_NEAR(a->console_phase_distortion_ns, b->console_phase_distortion_ns, FLOAT_TOL, "console_phase_distortion_ns");
    ASSERT_NEAR(b->console_psu_hum,           a->console_psu_hum,           FLOAT_TOL, "console_psu_hum");

    /* RF. */
    ASSERT_EQ_INT(b->rf.enabled ? 1 : 0, a->rf.enabled ? 1 : 0, "rf.enabled");
    ASSERT_NEAR(b->rf.carrier_freq,     a->rf.carrier_freq,     1.0f,      "rf.carrier_freq");
    ASSERT_NEAR(b->rf.mod_bandwidth,    a->rf.mod_bandwidth,    1.0f,      "rf.mod_bandwidth");
    ASSERT_NEAR(b->rf.noise_floor_dbm,  a->rf.noise_floor_dbm,  FLOAT_TOL, "rf.noise_floor_dbm");
    ASSERT_NEAR(b->rf.agc_attack_ms,    a->rf.agc_attack_ms,    FLOAT_TOL, "rf.agc_attack_ms");
    ASSERT_NEAR(b->rf.agc_release_ms,   a->rf.agc_release_ms,   FLOAT_TOL, "rf.agc_release_ms");

    /* Signal decode. */
    ASSERT_NEAR(b->brightness,  a->brightness,  FLOAT_TOL, "brightness");
    ASSERT_NEAR(b->contrast,    a->contrast,    FLOAT_TOL, "contrast");
    ASSERT_NEAR(b->chroma_gain, a->chroma_gain, FLOAT_TOL, "chroma_gain");

    /* Console supply and audio overrides. */
    ASSERT_NEAR(b->psu.adaptor_vac, a->psu.adaptor_vac, FLOAT_TOL, "psu.adaptor_vac");
    ASSERT_NEAR(b->psu.reservoir_uf, a->psu.reservoir_uf, FLOAT_TOL, "psu.reservoir_uf");
    ASSERT_NEAR(b->psu.load_ma, a->psu.load_ma, FLOAT_TOL, "psu.load_ma");
    ASSERT_NEAR(b->psu.regulator_rejection_db, a->psu.regulator_rejection_db, FLOAT_TOL, "psu.regulator_rejection_db");
    ASSERT_NEAR(b->rf.sound_am_rejection_db, a->rf.sound_am_rejection_db, FLOAT_TOL, "rf.sound_am_rejection_db");
    ASSERT_NEAR(b->rf.icpm_deg, a->rf.icpm_deg, FLOAT_TOL, "rf.icpm_deg");
    ASSERT_NEAR(b->audio_noise_floor,       a->audio_noise_floor,       FLOAT_TOL, "audio_noise_floor");
    ASSERT_NEAR(b->audio_saturation_drive,  a->audio_saturation_drive,  FLOAT_TOL, "audio_saturation_drive");
    ASSERT_NEAR(b->audio_cable_length_m,    a->audio_cable_length_m,    FLOAT_TOL, "audio_cable_length_m");

    return 1;
}

/* Read a whole file into a heap-allocated buffer. Caller must free. */
static char *slurp_file(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n < 0) { fclose(f); return NULL; }
    char *buf = (char *)malloc((size_t)n + 1);
    if (!buf) { fclose(f); return NULL; }
    size_t got = fread(buf, 1, (size_t)n, f);
    buf[got] = '\0';
    fclose(f);
    return buf;
}

/* Write text to a file (for synthetic JSON tests). */
static int write_text_file(const char *path, const char *text)
{
    FILE *f = fopen(path, "w");
    if (!f) return 0;
    fputs(text, f);
    fclose(f);
    return 1;
}

/* ============================================================================
 * 1. test_save_load_roundtrip
 * ============================================================================ */

static int test_save_load_roundtrip(void)
{
    PhysicalPreset src, dst;
    make_distinctive_preset(&src);

    const char *path = "/tmp/test_roundtrip.json";
    ASSERT_TRUE(preset_json_save(&src, path), "save failed");

    ASSERT_TRUE(preset_json_load(&dst, path), "load failed");

    if (!compare_presets(&src, &dst, "roundtrip")) return 0;

    ASSERT_TRUE(write_text_file(path, "{\"vhs\":{\"enabled\":1}}"), "write numeric VHS toggle");
    ASSERT_TRUE(preset_json_load(&dst, path) && dst.vhs.enabled == 1, "numeric VHS compatibility");
    ASSERT_TRUE(write_text_file(path, "{\"vhs\":{\"enabled\":false}}"), "write disabled VHS toggle");
    ASSERT_TRUE(preset_json_load(&dst, path) && dst.vhs.enabled == 0, "boolean VHS disable");

    /* A block written before the FM deck has no model number; preset
     * apply replaces it with the deck defaults. Its old keys are ignored. */
    ASSERT_TRUE(write_text_file(path, "{\"vhs\":{\"enabled\":true,\"timebase_ns\":35,\"luma_noise_rms\":0.018}}"),
                "write pre-2 VHS block");
    ASSERT_TRUE(preset_json_load(&dst, path) && dst.vhs.enabled == 1 && dst.vhs.model == 0 &&
                dst.vhs.rf_cnr_dbhz == 0, "pre-2 VHS block loads as model 0");
    ASSERT_TRUE(write_text_file(path, "{\"vhs\":{\"model\":2,\"doc\":false,\"skew_ba_ns\":1200}}"),
                "write model 2 VHS block");
    ASSERT_TRUE(preset_json_load(&dst, path) && dst.vhs.model == 2 && dst.vhs.doc == 0 &&
                dst.vhs.skew_ba_ns == 1200, "model 2 VHS keys");

    unlink(path);
    return 1;
}

/* ============================================================================
 * 2. test_shipped_presets_roundtrip
 *    Load every preset from presets/, save it back to /tmp, reload, and
 *    verify the struct round-trips identically.
 * ============================================================================ */

static int test_shipped_presets_roundtrip(void)
{
    char names[32][128], paths[32][512];
    int n = preset_json_scan_dir("presets", names, paths, 32);
    if (n <= 0) {
        printf("  SKIP: no presets in presets/ (run from repo root)\n");
        return 1;
    }

    char tmp[256];
    for (int i = 0; i < n; i++) {
        PhysicalPreset src;
        if (!preset_json_load(&src, paths[i])) {
            printf("  FAIL: load shipped preset %s\n", paths[i]);
            return 0;
        }

        snprintf(tmp, sizeof(tmp), "/tmp/test_shipped_%d.json", i);
        if (!preset_json_save(&src, tmp)) {
            printf("  FAIL: save %s\n", names[i]);
            return 0;
        }

        PhysicalPreset dst;
        if (!preset_json_load(&dst, tmp)) {
            printf("  FAIL: reload %s from %s\n", names[i], tmp);
            return 0;
        }

        if (!compare_presets(&src, &dst, src.name[0] ? src.name : names[i])) {
            printf("  (above mismatch was in shipped preset: %s)\n", paths[i]);
            unlink(tmp);
            return 0;
        }
        unlink(tmp);
    }
    return 1;
}

/* ============================================================================
 * 3. test_string_enum_save -- enums serialised as string literals, not ints
 * ============================================================================ */

static int test_string_enum_save(void)
{
    PhysicalPreset p;
    make_distinctive_preset(&p);
    p.connection    = VIDEO_CONN_COMPOSITE;
    p.comb_type     = VIDEO_COMB_1LINE;
    p.tv.mask_type  = VIDEO_MASK_SHADOW;
    p.region        = SIGNAL_REGION_NTSC;

    const char *path = "/tmp/test_string_enum_save.json";
    ASSERT_TRUE(preset_json_save(&p, path), "save failed");

    char *text = slurp_file(path);
    ASSERT_TRUE(text != NULL, "failed to slurp saved file");

    /* Should contain string literals, not integer values. */
    ASSERT_TRUE(strstr(text, "\"connection\": \"composite\"") != NULL,
                "expected connection string \"composite\"");
    ASSERT_TRUE(strstr(text, "\"comb_type\": \"1line\"") != NULL,
                "expected comb_type string \"1line\"");
    ASSERT_TRUE(strstr(text, "\"mask_type\": \"shadow\"") != NULL,
                "expected mask_type string \"shadow\"");
    ASSERT_TRUE(strstr(text, "\"region\": \"ntsc\"") != NULL,
                "expected region string \"ntsc\"");

    /* Should NOT contain integer enum forms. */
    ASSERT_TRUE(strstr(text, "\"connection\": 1") == NULL,
                "connection should not be integer-valued");
    ASSERT_TRUE(strstr(text, "\"comb_type\": 1") == NULL,
                "comb_type should not be integer-valued");
    ASSERT_TRUE(strstr(text, "\"mask_type\": 0") == NULL,
                "mask_type should not be integer-valued");

    free(text);
    unlink(path);
    return 1;
}

/* ============================================================================
 * 4. test_string_enum_load -- load presets with string enum values
 * ============================================================================ */

static int test_string_enum_load(void)
{
    /* Write a JSON with every enum as a string. */
    const char *json =
        "{\n"
        "    \"name\": \"String Enum Test\",\n"
        "    \"description\": \"enums as strings\",\n"
        "    \"connection\": \"svideo\",\n"
        "    \"comb_type\": \"bypass\",\n"
        "    \"region\": \"pal\",\n"
        "    \"console_variant\": 2,\n"
        "    \"speaker_type\": 2,\n"
        "    \"tv\": {\n"
        "        \"mask_type\": \"aperture_grille\",\n"
        "        \"gamma\": 2.4\n"
        "    },\n"
        "    \"rf\": {\n"
        "        \"enabled\": false\n"
        "    }\n"
        "}\n";

    const char *path = "/tmp/test_string_enum_load.json";
    ASSERT_TRUE(write_text_file(path, json), "write file failed");

    PhysicalPreset p;
    ASSERT_TRUE(preset_json_load(&p, path), "load failed");

    ASSERT_EQ_INT(p.connection, VIDEO_CONN_SVIDEO, "connection=svideo");
    ASSERT_EQ_INT(p.comb_type, VIDEO_COMB_BYPASS,  "comb_type=bypass");
    ASSERT_EQ_INT(p.region, SIGNAL_REGION_PAL,     "region=pal");
    ASSERT_EQ_INT(p.tv.mask_type, VIDEO_MASK_APERTURE_GRILLE, "mask_type=aperture_grille");
    ASSERT_EQ_STR(p.name, "String Enum Test", "name round-trip");

    unlink(path);
    return 1;
}

/* ============================================================================
 * 5. test_integer_enum_backward_compat -- old JSONs with integer enums load
 * ============================================================================ */

static int test_integer_enum_backward_compat(void)
{
    /* Old-format JSON: enums as raw integers. The loader's numeric fallback
     * path should accept them. */
    const char *json =
        "{\n"
        "    \"name\": \"Legacy Integer Enums\",\n"
        "    \"description\": \"pre-string-enum preset\",\n"
        "    \"connection\": 1,\n"       /* VIDEO_CONN_COMPOSITE */
        "    \"comb_type\": 2,\n"        /* VIDEO_COMB_2LINE     */
        "    \"region\": 0,\n"           /* SIGNAL_REGION_NTSC   */
        "    \"console_variant\": 1,\n"
        "    \"speaker_type\": 0,\n"
        "    \"tv\": {\n"
        "        \"mask_type\": 2,\n"    /* VIDEO_MASK_SLOT      */
        "        \"gamma\": 2.4\n"
        "    },\n"
        "    \"rf\": {\n"
        "        \"enabled\": false\n"
        "    }\n"
        "}\n";

    const char *path = "/tmp/test_integer_enum.json";
    ASSERT_TRUE(write_text_file(path, json), "write file failed");

    PhysicalPreset p;
    ASSERT_TRUE(preset_json_load(&p, path), "load failed");

    ASSERT_EQ_INT(p.connection, VIDEO_CONN_COMPOSITE, "connection=1 -> COMPOSITE");
    ASSERT_EQ_INT(p.comb_type, VIDEO_COMB_2LINE,      "comb_type=2 -> 2LINE");
    ASSERT_EQ_INT(p.region, SIGNAL_REGION_NTSC,       "region=0 -> NTSC");
    ASSERT_EQ_INT(p.tv.mask_type, VIDEO_MASK_SLOT,    "mask_type=2 -> SLOT");

    unlink(path);
    return 1;
}

/* ============================================================================
 * 6. test_missing_fields_use_defaults -- missing fields are zero
 * ============================================================================ */

static int test_missing_fields_use_defaults(void)
{
    /* Minimal JSON: only identity fields. Everything else should default to 0
     * because preset_json_load does memset(p, 0, sizeof(*p)) before parsing. */
    const char *json =
        "{\n"
        "    \"name\": \"Minimal\",\n"
        "    \"description\": \"only identity fields present\"\n"
        "}\n";

    const char *path = "/tmp/test_minimal.json";
    ASSERT_TRUE(write_text_file(path, json), "write file failed");

    /* Poison the preset with non-zero values first, so we can detect that
     * load() zeroes out unset fields. */
    PhysicalPreset p;
    memset(&p, 0xFF, sizeof(p));

    ASSERT_TRUE(preset_json_load(&p, path), "load failed");

    /* Identity fields populated. */
    ASSERT_EQ_STR(p.name, "Minimal", "name");
    ASSERT_EQ_STR(p.description, "only identity fields present", "description");

    /* Everything else should be zero (memset contract). */
    ASSERT_EQ_INT(p.connection, 0,       "connection default 0");
    ASSERT_EQ_INT(p.comb_type, 0,        "comb_type default 0");
    ASSERT_EQ_INT(p.console_variant, 0,  "console_variant default 0");
    ASSERT_EQ_INT(p.speaker_type, 0,     "speaker_type default 0");
    ASSERT_EQ_INT(p.region, 0,           "region default 0");
    ASSERT_NEAR(p.brightness,  0.0f, 1e-9f, "brightness default 0");
    ASSERT_NEAR(p.contrast,    0.0f, 1e-9f, "contrast default 0");
    ASSERT_NEAR(p.chroma_gain, 1.0f, 1e-9f, "chroma_gain default unity");
    ASSERT_NEAR(p.video_cable.length_meters, 0.0f, 1e-9f, "video cable default 0");
    ASSERT_NEAR(p.audio_cable.length_meters, 0.0f, 1e-9f, "audio cable default 0");
    ASSERT_NEAR(p.tv.gamma,       0.0f, 1e-9f, "tv.gamma default 0");
    ASSERT_NEAR(p.tv.mask_pitch_px, 0.0f, 1e-9f, "tv.mask_pitch_px default 0");
    ASSERT_NEAR(p.tv.hdr_gain,    0.0f, 1e-9f, "tv.hdr_gain default 0");
    ASSERT_EQ_INT(p.tv.monitor_model,0,"legacy TV raster default");
    ASSERT_NEAR(p.tv.halation_sigma, 0.0f, 1e-9f, "legacy scatter kernel default");
    ASSERT_EQ_INT(p.rf.enabled ? 1 : 0, 0, "rf.enabled default false");
    ASSERT_NEAR(p.console_coupling_R, 0.0f, 1e-9f, "console_coupling_R default 0");
    ASSERT_NEAR(p.psu.reservoir_uf, 0.0f, 1e-9f, "psu.reservoir_uf default 0 (nominal NES-001 supply)");

    unlink(path);
    return 1;
}

/* ============================================================================
 * 7. test_malformed_json -- malformed inputs fail gracefully
 * ============================================================================ */

static int test_compact_nested_json(void)
{
    const char *path="/tmp/test_compact_preset.json";
    const char *json="{\"name\":\"Compact, {quoted}: \\\"yes\\\"\",\"tv\":{\"gamma\":2.4,\"mask_type\":\"slot\",\"unknown\":{\"gamma\":9}},\"connection\":\"component\",\"unknown\":[1,{\"contrast\":7}],\"contrast\":1.1,\"rf\":{\"enabled\":true,\"carrier_level_dbm\":-20}}";
    ASSERT_TRUE(write_text_file(path,json),"write compact");
    PhysicalPreset p;
    ASSERT_TRUE(preset_json_load(&p,path),"compact JSON accepted");
    ASSERT_EQ_STR(p.name,"Compact, {quoted}: \"yes\"","quoted punctuation");
    ASSERT_NEAR(p.tv.gamma,2.4,1e-6,"nested unknown cannot replace gamma");
    ASSERT_EQ_INT(p.tv.mask_type,VIDEO_MASK_SLOT,"compact mask");
    ASSERT_EQ_INT(p.connection,VIDEO_CONN_COMPONENT,"return to root");
    ASSERT_NEAR(p.contrast,1.1,1e-6,"unknown array ignored");
    ASSERT_TRUE(p.rf.enabled,"compact bool");
    ASSERT_NEAR(p.rf.carrier_level_dbm,-20,1e-6,"negative number");
    ASSERT_TRUE(write_text_file(path,"{\"name\":\"CRT \\u2014 Caf\\u00e9 \\ud83d\\udcfa\"}"),"write Unicode escapes");
    ASSERT_TRUE(preset_json_load(&p,path),"Unicode JSON accepted");
    ASSERT_EQ_STR(p.name,"CRT — Café 📺","decode BMP and surrogate pair to UTF-8");
    const char *bad[]={"{", "{} junk", "{\"gamma\":1,}", "{\"tv\":{\"gamma\":nan}}", "{\"name\":\"bad\\q\"}", "{\"contrast\":1e999}", "{\"tv\":{\"gamma\":2.}}"};
    PhysicalPreset before=p;
    for (size_t i=0;i<sizeof(bad)/sizeof(*bad);i++) {
        ASSERT_TRUE(write_text_file(path,bad[i]),"write invalid JSON");
        ASSERT_FALSE(preset_json_load(&p,path),"invalid JSON rejected");
        ASSERT_TRUE(!memcmp(&p,&before,sizeof(p)),"failure preserves current preset");
    }
    unlink(path);
    return 1;
}

static int test_malformed_json(void)
{
    PhysicalPreset p;

    /* (a) Completely empty file. */
    const char *path_empty = "/tmp/test_empty.json";
    ASSERT_TRUE(write_text_file(path_empty, ""), "write empty file");
    /* load should reject zero-size file (fsize <= 0 path). */
    ASSERT_FALSE(preset_json_load(&p, path_empty), "empty file should fail load");
    unlink(path_empty);

    /* (b) Non-existent file. */
    ASSERT_FALSE(preset_json_load(&p, "/tmp/definitely_does_not_exist_9999.json"),
                 "nonexistent file should fail load");

    /* (c) Unclosed brace. Parser is lenient, should not crash; return true is
     * acceptable as long as it doesn't segfault. */
    const char *path_unclosed = "/tmp/test_unclosed.json";
    const char *unclosed_json =
        "{\n"
        "    \"name\": \"Unclosed\",\n"
        "    \"description\": \"missing closing brace\",\n"
        "    \"brightness\": 0.5\n";
    ASSERT_TRUE(write_text_file(path_unclosed, unclosed_json), "write unclosed");
    /* Must not crash. Loader may return true or false depending on tolerance. */
    (void)preset_json_load(&p, path_unclosed);
    unlink(path_unclosed);

    /* (d) Truncated file -- valid JSON cut off mid-value. */
    const char *path_trunc = "/tmp/test_truncated.json";
    const char *trunc_json =
        "{\n"
        "    \"name\": \"Truncated\",\n"
        "    \"brightness\": 0.";  /* no closing */
    ASSERT_TRUE(write_text_file(path_trunc, trunc_json), "write truncated");
    /* Must not crash. */
    (void)preset_json_load(&p, path_trunc);
    unlink(path_trunc);

    /* (e) Garbage / binary-ish content. */
    const char *path_garbage = "/tmp/test_garbage.json";
    const char *garbage = "!!! this is not JSON at all !!!\n\xff\xfe random bytes";
    ASSERT_TRUE(write_text_file(path_garbage, garbage), "write garbage");
    /* Must not crash. */
    (void)preset_json_load(&p, path_garbage);
    unlink(path_garbage);

    /* (f) NULL arguments. */
    ASSERT_FALSE(preset_json_load(NULL, "/tmp/anything.json"), "NULL preset");
    ASSERT_FALSE(preset_json_load(&p, NULL),                   "NULL path");

    return 1;
}

/* ============================================================================
 * 8. test_scan_dir -- scan a directory for .json preset files
 * ============================================================================ */

static int test_scan_dir(void)
{
    /* Create a fresh temp dir with 3 .json files and 2 non-json files. */
    const char *dir = "/tmp/test_preset_scan_dir";

    /* Clean up any existing leftover. */
    char cleanup_cmd[512];
    snprintf(cleanup_cmd, sizeof(cleanup_cmd), "rm -rf %s && mkdir -p %s", dir, dir);
    int rc = system(cleanup_cmd);
    (void)rc;

    char path[512];

    /* 3 valid .json files. */
    snprintf(path, sizeof(path), "%s/alpha.json", dir);
    if (!write_text_file(path, "{\"name\":\"Alpha\"}\n")) return 0;
    snprintf(path, sizeof(path), "%s/beta.json", dir);
    if (!write_text_file(path, "{\"name\":\"Beta\"}\n"))  return 0;
    snprintf(path, sizeof(path), "%s/gamma.json", dir);
    if (!write_text_file(path, "{\"name\":\"Gamma\"}\n")) return 0;

    /* 2 non-json files that should be ignored. */
    snprintf(path, sizeof(path), "%s/readme.txt", dir);
    if (!write_text_file(path, "not a preset\n")) return 0;
    snprintf(path, sizeof(path), "%s/config.yaml", dir);
    if (!write_text_file(path, "also not a preset\n")) return 0;

    /* Scan. */
    char names[16][128];
    char paths[16][512];
    int count = preset_json_scan_dir(dir, names, paths, 16);

    ASSERT_EQ_INT(count, 3, "expected exactly 3 .json files");

    /* Names should not include the .json extension. */
    int saw_alpha = 0, saw_beta = 0, saw_gamma = 0;
    for (int i = 0; i < count; i++) {
        if (strcmp(names[i], "alpha") == 0) saw_alpha = 1;
        if (strcmp(names[i], "beta")  == 0) saw_beta  = 1;
        if (strcmp(names[i], "gamma") == 0) saw_gamma = 1;

        /* Path should begin with dir/ and end with .json. */
        char expected_prefix[512];
        snprintf(expected_prefix, sizeof(expected_prefix), "%s/", dir);
        ASSERT_TRUE(strncmp(paths[i], expected_prefix, strlen(expected_prefix)) == 0,
                    "path begins with dir/");
        size_t plen = strlen(paths[i]);
        ASSERT_TRUE(plen >= 5 && strcmp(paths[i] + plen - 5, ".json") == 0,
                    "path ends with .json");
    }
    ASSERT_TRUE(saw_alpha, "saw alpha");
    ASSERT_TRUE(saw_beta,  "saw beta");
    ASSERT_TRUE(saw_gamma, "saw gamma");

    /* Non-existent directory should return 0 without crashing. */
    int none = preset_json_scan_dir("/tmp/this_dir_does_not_exist_xyz", names, paths, 16);
    ASSERT_EQ_INT(none, 0, "nonexistent dir returns 0");

    /* NULL/invalid args. */
    ASSERT_EQ_INT(preset_json_scan_dir(NULL, names, paths, 16), 0, "NULL dir");
    ASSERT_EQ_INT(preset_json_scan_dir(dir, NULL, paths, 16),   0, "NULL names");
    ASSERT_EQ_INT(preset_json_scan_dir(dir, names, NULL, 16),   0, "NULL paths");
    ASSERT_EQ_INT(preset_json_scan_dir(dir, names, paths, 0),   0, "max_count 0");

    /* Cleanup. */
    snprintf(cleanup_cmd, sizeof(cleanup_cmd), "rm -rf %s", dir);
    rc = system(cleanup_cmd);
    (void)rc;

    return 1;
}

/* ============================================================================
 * 9. test_string_escape -- quotes and backslashes round-trip correctly
 * ============================================================================ */

static int test_string_escape(void)
{
    PhysicalPreset src;
    make_distinctive_preset(&src);

    strncpy(src.name,        "Test\"Me",      sizeof(src.name) - 1);
    strncpy(src.description, "C:\\foo\\bar", sizeof(src.description) - 1);

    const char *path = "/tmp/test_escape.json";
    ASSERT_TRUE(preset_json_save(&src, path), "save failed");

    /* Verify raw JSON contains the escape sequences. */
    char *text = slurp_file(path);
    ASSERT_TRUE(text != NULL, "slurp");
    /* name: "Test\"Me" -> in JSON: "Test\"Me" (escaped quote) */
    ASSERT_TRUE(strstr(text, "Test\\\"Me") != NULL,
                "escaped quote in saved JSON");
    /* description: "C:\foo\bar" -> in JSON: "C:\\foo\\bar" */
    ASSERT_TRUE(strstr(text, "C:\\\\foo\\\\bar") != NULL,
                "escaped backslash in saved JSON");
    free(text);

    /* Load and verify the decoded strings match the originals. */
    PhysicalPreset dst;
    ASSERT_TRUE(preset_json_load(&dst, path), "load failed");
    ASSERT_EQ_STR(dst.name, "Test\"Me",      "decoded quoted name");
    ASSERT_EQ_STR(dst.description, "C:\\foo\\bar", "decoded backslash description");

    unlink(path);
    return 1;
}

/* ============================================================================
 * 10. test_all_connection_types -- every VideoConnectionType round-trips
 * ============================================================================ */

static int test_all_connection_types(void)
{
    const VideoConnectionType types[] = {
        VIDEO_CONN_RF,
        VIDEO_CONN_COMPOSITE,
        VIDEO_CONN_SVIDEO,
        VIDEO_CONN_COMPONENT,
        VIDEO_CONN_RGB,
        VIDEO_CONN_DIRECT,
    };
    const int n = (int)(sizeof(types) / sizeof(types[0]));

    for (int i = 0; i < n; i++) {
        PhysicalPreset src, dst;
        make_distinctive_preset(&src);
        src.connection = types[i];

        const char *path = "/tmp/test_connection_type.json";
        if (!preset_json_save(&src, path)) {
            printf("  FAIL: save conn=%d\n", types[i]);
            return 0;
        }
        if (!preset_json_load(&dst, path)) {
            printf("  FAIL: load conn=%d\n", types[i]);
            return 0;
        }
        if ((int)dst.connection != (int)types[i]) {
            printf("  FAIL: connection roundtrip: sent %d, got %d\n",
                   types[i], dst.connection);
            unlink(path);
            return 0;
        }
        unlink(path);
    }
    return 1;
}

/* ============================================================================
 * 11. test_all_comb_types -- every VideoCombType round-trips
 * ============================================================================ */

static int test_all_comb_types(void)
{
    const VideoCombType types[] = {
        VIDEO_COMB_NONE,
        VIDEO_COMB_1LINE,
        VIDEO_COMB_2LINE,
        VIDEO_COMB_3LINE,
        VIDEO_COMB_BYPASS,
    };
    const int n = (int)(sizeof(types) / sizeof(types[0]));

    for (int i = 0; i < n; i++) {
        PhysicalPreset src, dst;
        make_distinctive_preset(&src);
        src.comb_type = types[i];

        const char *path = "/tmp/test_comb_type.json";
        if (!preset_json_save(&src, path)) {
            printf("  FAIL: save comb=%d\n", types[i]);
            return 0;
        }
        if (!preset_json_load(&dst, path)) {
            printf("  FAIL: load comb=%d\n", types[i]);
            return 0;
        }
        if ((int)dst.comb_type != (int)types[i]) {
            printf("  FAIL: comb_type roundtrip: sent %d, got %d\n",
                   types[i], dst.comb_type);
            unlink(path);
            return 0;
        }
        unlink(path);
    }
    return 1;
}

/* ============================================================================
 * 12. test_all_mask_types -- every VideoMaskType round-trips
 * ============================================================================ */

static int test_all_mask_types(void)
{
    const VideoMaskType types[] = {
        VIDEO_MASK_SHADOW,
        VIDEO_MASK_APERTURE_GRILLE,
        VIDEO_MASK_SLOT,
    };
    const int n = (int)(sizeof(types) / sizeof(types[0]));

    for (int i = 0; i < n; i++) {
        PhysicalPreset src, dst;
        make_distinctive_preset(&src);
        src.tv.mask_type = types[i];

        const char *path = "/tmp/test_mask_type.json";
        if (!preset_json_save(&src, path)) {
            printf("  FAIL: save mask=%d\n", types[i]);
            return 0;
        }
        if (!preset_json_load(&dst, path)) {
            printf("  FAIL: load mask=%d\n", types[i]);
            return 0;
        }
        if ((int)dst.tv.mask_type != (int)types[i]) {
            printf("  FAIL: mask_type roundtrip: sent %d, got %d\n",
                   types[i], dst.tv.mask_type);
            unlink(path);
            return 0;
        }
        unlink(path);
    }
    return 1;
}

/* ============================================================================
 * Main
 * ============================================================================ */

static int test_legacy_mask_pitch_key(void) {
    const char *path = "/tmp/test_legacy_mask_pitch.json";
    ASSERT_TRUE(write_text_file(path, "{\n  \"tv\": {\n    \"mask_pitch_mm\": 3.5\n  }\n}\n"), "write legacy preset");
    PhysicalPreset p;
    ASSERT_TRUE(preset_json_load(&p,path), "load legacy preset");
    ASSERT_NEAR(p.tv.mask_pitch_px,3.5f,1e-6f,"legacy pixel-pitch alias");
    unlink(path);
    return 1;
}

int main(void)
{
    printf("=== Preset JSON Save/Load Tests ===\n\n");

    printf("--- Round-trip ---\n");
    RUN_TEST(test_save_load_roundtrip);
    RUN_TEST(test_shipped_presets_roundtrip);

    printf("\n--- Enum serialisation ---\n");
    RUN_TEST(test_string_enum_save);
    RUN_TEST(test_string_enum_load);
    RUN_TEST(test_integer_enum_backward_compat);
    RUN_TEST(test_legacy_mask_pitch_key);

    printf("\n--- Robustness ---\n");
    RUN_TEST(test_missing_fields_use_defaults);
    RUN_TEST(test_malformed_json);
    RUN_TEST(test_compact_nested_json);

    printf("\n--- Directory scan ---\n");
    RUN_TEST(test_scan_dir);

    printf("\n--- String handling ---\n");
    RUN_TEST(test_string_escape);

    printf("\n--- Exhaustive enum coverage ---\n");
    RUN_TEST(test_all_connection_types);
    RUN_TEST(test_all_comb_types);
    RUN_TEST(test_all_mask_types);

    printf("\n=== Results: %d/%d tests passed ===\n", tests_passed, tests_run);
    return (tests_passed == tests_run) ? 0 : 1;
}
