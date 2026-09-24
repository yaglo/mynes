/*
 * Preset Validation Tests — sanity-check every JSON preset in presets/
 * =====================================================================
 *
 * Scans presets/ at startup, loads each file, and verifies:
 *   - identity strings are populated
 *   - enums fall inside their defined ranges
 *   - all physical parameters sit within plausible bounds
 *   - no float field is NaN or infinite
 *   - names are unique
 *   - connection-specific invariants (RF has RF config, etc.)
 *
 * Build: gcc -O2 -lm -I.. test_presets.c ../video_chain.c -o test_presets
 * Run:   ./test_presets   (run from repo root so presets/ is visible)
 */

#include "../presets.h"
#include "../preset_json.h"
#include <stdio.h>
#include <math.h>
#include <string.h>
#include <stdlib.h>

/* Up to 64 presets loaded from presets/. */
#define MAX_PRESETS 64
static PhysicalPreset presets[MAX_PRESETS];
static char           preset_names_buf[MAX_PRESETS][128];
static char           preset_paths_buf[MAX_PRESETS][512];
static int            preset_count = 0;

static int tests_run = 0;
static int tests_passed = 0;

#define RUN_TEST(fn) do { \
    tests_run++; \
    printf("TEST %s: ", #fn); \
    if (fn()) { tests_passed++; printf("PASS\n"); } \
    else { printf("FAILED\n"); } \
} while(0)

#define CHECK_RANGE(preset, field_expr, lo, hi, label) do { \
    float _v = (float)(field_expr); \
    if (!(_v >= (float)(lo) && _v <= (float)(hi))) { \
        printf("\n  FAIL: [%s] %s = %g, not in [%g, %g]", \
               (preset)->name ? (preset)->name : "?", label, \
               (double)_v, (double)(lo), (double)(hi)); \
        return 0; \
    } \
} while(0)

#define CHECK_FINITE(preset, field_expr, label) do { \
    float _v = (float)(field_expr); \
    if (isnan(_v) || isinf(_v)) { \
        printf("\n  FAIL: [%s] %s is NaN/inf (%g)", \
               (preset)->name ? (preset)->name : "?", label, (double)_v); \
        return 0; \
    } \
} while(0)

/* Look up a loaded preset by filename slug (substring match). */
static const PhysicalPreset *find_preset_by_slug(const char *needle) {
    for (int i = 0; i < preset_count; i++) {
        if (strstr(preset_paths_buf[i], needle)) return &presets[i];
    }
    return NULL;
}

/* ============================================================================
 * 1. Loadability / identity
 * ============================================================================ */

static int test_all_presets_loadable(void) {
    if (preset_count <= 0) {
        printf("\n  FAIL: no presets found in presets/");
        return 0;
    }
    for (int i = 0; i < preset_count; i++) {
        const PhysicalPreset *p = &presets[i];
        if (p->name[0] == '\0') {
            printf("\n  FAIL: preset %s has null/empty name", preset_paths_buf[i]);
            return 0;
        }
        if (p->description[0] == '\0') {
            printf("\n  FAIL: preset %s has null/empty description", p->name);
            return 0;
        }
        /* The loader cuts a description at the field's size; a shipped one
         * that fills it has been cut. */
        if (strlen(p->description) >= sizeof(p->description) - 1) {
            printf("\n  FAIL: preset %s: description cut at %zu bytes", p->name, sizeof(p->description) - 1);
            return 0;
        }
    }
    return 1;
}

/* ============================================================================
 * 2. Enum ranges
 * ============================================================================ */

static int test_connection_types_valid(void) {
    for (int i = 0; i < preset_count; i++) {
        const PhysicalPreset *p = &presets[i];
        if ((int)p->connection < 0 || (int)p->connection >= (int)VIDEO_CONN_COUNT) {
            printf("\n  FAIL: [%s] connection = %d, not in [0, %d)",
                   p->name, (int)p->connection, (int)VIDEO_CONN_COUNT);
            return 0;
        }
    }
    return 1;
}

static int test_comb_types_valid(void) {
    for (int i = 0; i < preset_count; i++) {
        const PhysicalPreset *p = &presets[i];
        int c = (int)p->comb_type;
        if (c < 0 || c > 4) {
            printf("\n  FAIL: [%s] comb_type = %d, not in [0, 4]", p->name, c);
            return 0;
        }
    }
    return 1;
}

static int test_mask_types_valid(void) {
    for (int i = 0; i < preset_count; i++) {
        const PhysicalPreset *p = &presets[i];
        int m = (int)p->tv.mask_type;
        if (m < 0 || m > 2) {
            printf("\n  FAIL: [%s] mask_type = %d, not in [0, 2]", p->name, m);
            return 0;
        }
    }
    return 1;
}

static int test_region_valid(void) {
    for (int i = 0; i < preset_count; i++) {
        const PhysicalPreset *p = &presets[i];
        if (p->region != SIGNAL_REGION_NTSC && p->region != SIGNAL_REGION_PAL) {
            printf("\n  FAIL: [%s] region = %d, not NTSC(%d) or PAL(%d)",
                   p->name, p->region, SIGNAL_REGION_NTSC, SIGNAL_REGION_PAL);
            return 0;
        }
    }
    return 1;
}

/* ============================================================================
 * 3. Physical parameter ranges — cable
 * ============================================================================ */

static int test_cable_params_physical(void) {
    for (int i = 0; i < preset_count; i++) {
        const PhysicalPreset *p = &presets[i];
        const CableParams *c = &p->video_cable;
        CHECK_RANGE(p, c->length_meters,       0.5,    20.0,   "cable.length_meters");
        CHECK_RANGE(p, c->resistance_per_m,    0.0,    5.0,    "cable.resistance_per_m");
        CHECK_RANGE(p, c->capacitance_per_m,   10e-12, 200e-12,"cable.capacitance_per_m");
        CHECK_RANGE(p, c->impedance,           50.0,   100.0,  "cable.impedance");
        CHECK_RANGE(p, c->shield_effectiveness,0.0,    1.0,    "cable.shield_effectiveness");
        CHECK_RANGE(p, c->ghost_level,         0.0,    0.5,    "cable.ghost_level");
    }
    return 1;
}

/* ============================================================================
 * 4. Physical parameter ranges — TV / CRT
 * ============================================================================ */

static int test_tv_params_physical(void) {
    for (int i = 0; i < preset_count; i++) {
        const PhysicalPreset *p = &presets[i];
        const TVDisplayParams *t = &p->tv;

        CHECK_RANGE(p, t->luma_bandwidth,    1e6,    10e6,   "tv.luma_bandwidth");
        CHECK_RANGE(p, t->chroma_bandwidth,  0.1e6,  3e6,    "tv.chroma_bandwidth");
        CHECK_RANGE(p, t->gamma,             1.5,    3.0,    "tv.gamma");
        CHECK_RANGE(p, t->color_temperature, 3000.0, 10000.0,"tv.color_temperature");
        CHECK_RANGE(p, t->r_drive,           0.5,    1.5,    "tv.r_drive");
        CHECK_RANGE(p, t->g_drive,           0.5,    1.5,    "tv.g_drive");
        CHECK_RANGE(p, t->b_drive,           0.5,    1.5,    "tv.b_drive");

        CHECK_RANGE(p, t->monitor_model, 0, 1, "tv.monitor_model");
        CHECK_RANGE(p, t->phosphor_gamut, 0, 3, "tv.phosphor_gamut");
        /* mask_pitch_px is in pixel units per recent refactor; typical 0.5-20 */
        if (!(t->mask_pitch_px > 0.0f)) {
            printf("\n  FAIL: [%s] tv.mask_pitch_px = %g must be > 0",
                   p->name, (double)t->mask_pitch_px);
            return 0;
        }
        CHECK_RANGE(p, t->mask_pitch_px,   0.5,   20.0,   "tv.mask_pitch_px");
        CHECK_RANGE(p, t->mask_strength,   0.0,   1.0,    "tv.mask_strength");
        CHECK_RANGE(p, t->persistence_ms,  0.0,   50.0,   "tv.persistence_ms");
        CHECK_RANGE(p, t->beam_sharpness,  0.0,   2.0,    "tv.beam_sharpness");
        CHECK_RANGE(p, video_beam_sigma(t, true)*2.354820045f, 0.12f, 2.36f, "tv.white_beam_fwhm");
        CHECK_RANGE(p, t->barrel,          0.0,   0.2,    "tv.barrel");
        CHECK_RANGE(p, t->barrel_v,        0.0,   0.2,    "tv.barrel_v");
        CHECK_RANGE(p, t->halation,        0.0,   0.5,    "tv.halation");
        CHECK_RANGE(p, t->halation_sigma,  0.0,   0.05,   "tv.halation_sigma");
        CHECK_RANGE(p, t->glass_tint,      0.3,   1.0,    "tv.glass_tint");
        CHECK_RANGE(p, t->vignette,        0.0,   0.5,    "tv.vignette");
        CHECK_RANGE(p, t->ambient_light,   0.0,   0.3,    "tv.ambient_light");
        CHECK_RANGE(p, t->black_floor,     0.0,   0.2,    "tv.black_floor");
        CHECK_RANGE(p, t->noise_level,     0.0,   0.2,    "tv.noise_level");
        CHECK_RANGE(p, t->motion_threshold,0.0,   0.3,    "tv.motion_threshold");
        CHECK_RANGE(p, t->keystone,       -0.15,  0.15,   "tv.keystone");
        CHECK_RANGE(p, t->rotation,       -0.1,   0.1,    "tv.rotation");
        CHECK_RANGE(p, t->skew_x,         -0.15,  0.15,   "tv.skew_x");
        CHECK_RANGE(p, t->skew_y,         -0.15,  0.15,   "tv.skew_y");
        CHECK_RANGE(p, t->hv_sag,         -0.5,   0.5,    "tv.hv_sag");
        CHECK_RANGE(p, t->focus_breathing, 0.0,   0.3,    "tv.focus_breathing");
        CHECK_RANGE(p, t->video_black_droop, 0.0, 0.5,   "tv.video_black_droop");
        CHECK_RANGE(p, t->video_recovery_us, 0.0, 100.0, "tv.video_recovery_us");
        CHECK_RANGE(p, t->scanline_wobble, 0.0,   1.0,    "tv.scanline_wobble");
        CHECK_RANGE(p, t->top_band_shift, -20.0, 20.0,    "tv.top_band_shift");
        CHECK_RANGE(p, t->top_edge_skew,  -20.0, 20.0,    "tv.top_edge_skew");
        CHECK_RANGE(p, t->top_band_start,   0.0, 239.0,   "tv.top_band_start");
        CHECK_RANGE(p, t->top_band_end,     0.0, 239.0,   "tv.top_band_end");
        CHECK_RANGE(p, t->top_edge_width,   0.01, 0.5,    "tv.top_edge_width");
        CHECK_RANGE(p, t->overscan,        0.0,   0.1,    "tv.overscan");
        CHECK_RANGE(p, t->luma_peaking,    0.0,   1.0,    "tv.luma_peaking");
        CHECK_RANGE(p, t->rgb_bandwidth_3db, 0, 1, "tv.rgb_bandwidth_3db");
        CHECK_RANGE(p, t->aperture_max_db, 0.0, 12.0, "tv.aperture_max_db");
        CHECK_RANGE(p, t->h_afc_tau_ms, 0.0, 10.0, "tv.h_afc_tau_ms");
        CHECK_RANGE(p, t->h_pos,          -0.5,   0.5,    "tv.h_pos");
        CHECK_RANGE(p, t->v_pos,          -0.5,   0.5,    "tv.v_pos");
        CHECK_RANGE(p, t->h_size,          0.3,   2.0,    "tv.h_size");
        CHECK_RANGE(p, t->v_size,          0.3,   2.0,    "tv.v_size");
    }
    return 1;
}

/* ============================================================================
 * 5. NaN / Inf sweep across every float field
 * ============================================================================ */

static int test_no_nan_or_inf(void) {
    for (int i = 0; i < preset_count; i++) {
        const PhysicalPreset *p = &presets[i];

        CHECK_FINITE(p, p->console_coupling_R,       "console_coupling_R");
        CHECK_FINITE(p, p->console_coupling_C,       "console_coupling_C");
        CHECK_FINITE(p, p->console_amp_bw,           "console_amp_bw");
        CHECK_FINITE(p, p->console_psu_hum,          "console_psu_hum");
        CHECK_FINITE(p, p->brightness,               "brightness");
        CHECK_FINITE(p, p->contrast,                 "contrast");
        CHECK_FINITE(p, p->chroma_gain,              "chroma_gain");
        CHECK_FINITE(p, p->psu.adaptor_vac,          "psu.adaptor_vac");
        CHECK_FINITE(p, p->psu.reservoir_uf,         "psu.reservoir_uf");
        CHECK_FINITE(p, p->psu.load_ma,              "psu.load_ma");
        CHECK_FINITE(p, p->psu.regulator_rejection_db, "psu.regulator_rejection_db");
        CHECK_FINITE(p, p->audio_noise_floor,        "audio_noise_floor");
        CHECK_FINITE(p, p->audio_saturation_drive,   "audio_saturation_drive");
        CHECK_FINITE(p, p->audio_cable_length_m,     "audio_cable_length_m");

        const CableParams *vc = &p->video_cable;
        CHECK_FINITE(p, vc->length_meters,         "video_cable.length_meters");
        CHECK_FINITE(p, vc->resistance_per_m,      "video_cable.resistance_per_m");
        CHECK_FINITE(p, vc->capacitance_per_m,     "video_cable.capacitance_per_m");
        CHECK_FINITE(p, vc->connector_resistance,  "video_cable.connector_resistance");
        CHECK_FINITE(p, vc->impedance,             "video_cable.impedance");
        CHECK_FINITE(p, vc->shield_effectiveness,  "video_cable.shield_effectiveness");
        CHECK_FINITE(p, vc->ghost_level,           "video_cable.ghost_level");

        CHECK_FINITE(p, p->rf.carrier_freq,        "rf.carrier_freq");
        CHECK_FINITE(p, p->rf.mod_bandwidth,       "rf.mod_bandwidth");
        CHECK_FINITE(p, p->rf.noise_floor_dbm,     "rf.noise_floor_dbm");
        CHECK_FINITE(p, p->rf.agc_attack_ms,       "rf.agc_attack_ms");
        CHECK_FINITE(p, p->rf.agc_release_ms,      "rf.agc_release_ms");

        const TVDisplayParams *t = &p->tv;
        CHECK_FINITE(p, t->chroma_bandwidth,   "tv.chroma_bandwidth");
        CHECK_FINITE(p, t->luma_bandwidth,     "tv.luma_bandwidth");
        CHECK_FINITE(p, t->hue_offset,         "tv.hue_offset");
        CHECK_FINITE(p, t->saturation,         "tv.saturation");
        CHECK_FINITE(p, t->gamma,              "tv.gamma");
        CHECK_FINITE(p, t->beam_sharpness,     "tv.beam_sharpness");
        CHECK_FINITE(p, t->beam_spot_size,     "tv.beam_spot_size");
        CHECK_FINITE(p, t->mask_pitch_px,      "tv.mask_pitch_px");
        CHECK_FINITE(p, t->mask_strength,      "tv.mask_strength");
        CHECK_FINITE(p, t->halation,           "tv.halation");
        CHECK_FINITE(p, t->glass_tint,         "tv.glass_tint");
        CHECK_FINITE(p, t->barrel,             "tv.barrel");
        CHECK_FINITE(p, t->barrel_v,           "tv.barrel_v");
        CHECK_FINITE(p, t->hdr_gain,           "tv.hdr_gain");
        CHECK_FINITE(p, t->keystone,           "tv.keystone");
        CHECK_FINITE(p, t->rotation,           "tv.rotation");
        CHECK_FINITE(p, t->skew_x,             "tv.skew_x");
        CHECK_FINITE(p, t->skew_y,             "tv.skew_y");
        CHECK_FINITE(p, t->hv_sag,             "tv.hv_sag");
        CHECK_FINITE(p, t->top_band_shift,     "tv.top_band_shift");
        CHECK_FINITE(p, t->top_edge_skew,      "tv.top_edge_skew");
        CHECK_FINITE(p, t->top_band_start,     "tv.top_band_start");
        CHECK_FINITE(p, t->top_band_end,       "tv.top_band_end");
        CHECK_FINITE(p, t->top_edge_width,     "tv.top_edge_width");
        CHECK_FINITE(p, t->h_pos,              "tv.h_pos");
        CHECK_FINITE(p, t->v_pos,              "tv.v_pos");
        CHECK_FINITE(p, t->h_size,             "tv.h_size");
        CHECK_FINITE(p, t->v_size,             "tv.v_size");
    }
    return 1;
}

/* ============================================================================
 * 6. Name uniqueness
 * ============================================================================ */

static int test_preset_names_unique(void) {
    for (int i = 0; i < preset_count; i++) {
        for (int j = i + 1; j < preset_count; j++) {
            if (strcmp(presets[i].name, presets[j].name) == 0) {
                printf("\n  FAIL: presets '%s' and '%s' share name '%s'",
                       preset_paths_buf[i], preset_paths_buf[j],
                       presets[i].name);
                return 0;
            }
        }
    }
    return 1;
}

/* ============================================================================
 * 7. Connection-specific invariants
 * ============================================================================ */

static int test_rf_presets_have_rf_config(void) {
    int rf_checked = 0;
    for (int i = 0; i < preset_count; i++) {
        const PhysicalPreset *p = &presets[i];
        if (p->connection != VIDEO_CONN_RF) continue;
        rf_checked++;
        if (!p->rf.enabled) {
            printf("\n  FAIL: [%s] RF connection but rf.enabled = false", p->name);
            return 0;
        }
        if (!(p->rf.carrier_freq > 50e6f)) {
            printf("\n  FAIL: [%s] RF carrier_freq = %g Hz, must be > 50 MHz",
                   p->name, (double)p->rf.carrier_freq);
            return 0;
        }
    }
    if (rf_checked == 0) {
        printf("\n  (no RF presets found — skipping)");
    }
    return 1;
}

static int test_arcade_is_rgb(void) {
    const PhysicalPreset *p = find_preset_by_slug("arcade");
    if (!p) {
        printf("\n  (no 'arcade' preset found — skipping)");
        return 1;
    }
    if (p->connection != VIDEO_CONN_RGB) {
        printf("\n  FAIL: [%s] arcade connection = %d, expected VIDEO_CONN_RGB (%d)",
               p->name, (int)p->connection, (int)VIDEO_CONN_RGB);
        return 0;
    }
    return 1;
}

static int test_pvm_is_svideo_or_composite(void) {
    /* PVM presets in the catalogue should be S-Video or composite.
     * Real PVMs accepted both; composite is plausible if the preset is
     * modelling the simpler input. RF or Direct would be wrong. */
    for (int i = 0; i < preset_count; i++) {
        if (!strstr(preset_paths_buf[i], "pvm")) continue;
        VideoConnectionType c = presets[i].connection;
        if (c != VIDEO_CONN_SVIDEO && c != VIDEO_CONN_COMPOSITE) {
            printf("\n  FAIL: [%s] PVM preset conn=%d (expected S-Video or composite)",
                   presets[i].name, (int)c);
            return 0;
        }
    }
    return 1;
}

static int test_pvm_documented_response_settings(void) {
    const PhysicalPreset *p=find_preset_by_slug("sony_pvm_14l2.json");
    if(!p) return 0;
    CHECK_RANGE(p,p->tv.h_afc_tau_ms,1,1,"Sony AFC specification");
    CHECK_RANGE(p,p->tv.aperture_max_db,6,6,"Sony aperture range");
    CHECK_RANGE(p,p->tv.rgb_bandwidth_3db,1,1,"Sony bandwidth definition");
    CHECK_RANGE(p,p->tv.r_bandwidth,10e6,10e6,"Sony R bandwidth");
    CHECK_RANGE(p,p->tv.g_bandwidth,10e6,10e6,"Sony G bandwidth");
    CHECK_RANGE(p,p->tv.b_bandwidth,10e6,10e6,"Sony B bandwidth");
    /* Generic presets which omit these fields retain their existing laws. */
    p=find_preset_by_slug("studio_pvm.json");
    if(!p) return 0;
    CHECK_RANGE(p,p->tv.h_afc_tau_ms,0,0,"legacy AFC");
    CHECK_RANGE(p,p->tv.aperture_max_db,0,0,"legacy aperture");
    CHECK_RANGE(p,p->tv.rgb_bandwidth_3db,0,0,"legacy bandwidth");
    return 1;
}

/* ============================================================================
 * Main
 * ============================================================================ */

int main(int argc, char **argv) {
    const char *dir = (argc >= 2) ? argv[1] : "presets";
    preset_count = preset_json_scan_dir(dir,
        preset_names_buf, preset_paths_buf, MAX_PRESETS);

    if (preset_count <= 0) {
        fprintf(stderr, "No presets found in %s (CWD dependent).\n", dir);
        fprintf(stderr, "Run from the repo root, or pass the dir as argv[1].\n");
        return 1;
    }

    for (int i = 0; i < preset_count; i++) {
        if (!preset_json_load(&presets[i], preset_paths_buf[i])) {
            fprintf(stderr, "Failed to parse %s\n", preset_paths_buf[i]);
            return 1;
        }
    }

    printf("=== Preset Validation Tests (%d preset(s) in %s) ===\n\n",
           preset_count, dir);

    printf("--- Loadability ---\n");
    RUN_TEST(test_all_presets_loadable);

    printf("\n--- Enum ranges ---\n");
    RUN_TEST(test_connection_types_valid);
    RUN_TEST(test_comb_types_valid);
    RUN_TEST(test_mask_types_valid);
    RUN_TEST(test_region_valid);

    printf("\n--- Physical parameter ranges ---\n");
    RUN_TEST(test_cable_params_physical);
    RUN_TEST(test_tv_params_physical);

    printf("\n--- Numerical sanity ---\n");
    RUN_TEST(test_no_nan_or_inf);

    printf("\n--- Identity ---\n");
    RUN_TEST(test_preset_names_unique);

    printf("\n--- Connection invariants ---\n");
    RUN_TEST(test_rf_presets_have_rf_config);
    RUN_TEST(test_arcade_is_rgb);
    RUN_TEST(test_pvm_is_svideo_or_composite);
    RUN_TEST(test_pvm_documented_response_settings);

    printf("\n=== Results: %d/%d tests passed ===\n", tests_passed, tests_run);
    return (tests_passed == tests_run) ? 0 : 1;
}
