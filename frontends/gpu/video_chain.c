/*
 * Video Signal Chain -- Initialization and Stage Activation
 * ==========================================================
 *
 * Sets up the video chain with physically-accurate default values
 * based on connection type (RF/Composite/S-Video/Component/RGB/Direct)
 * and TV region (NTSC/PAL). Cable, RF modulator, and TV display
 * parameters are initialized to model real consumer hardware of the
 * NES era (1985-1995 CRT televisions with shadow mask phosphors).
 *
 * Component values sourced from:
 *   - RG-59 coax datasheet (Belden 8241)
 *   - Generic RCA/BNC cable measurements
 *   - NTSC/EIA-170A broadcast standard
 *   - P22 phosphor persistence data (JEDEC)
 *   - CIE D65 illuminant (6500 K)
 */

#include "video_chain.h"
#include <string.h>

/* ============================================================================
 * Cable presets per connection type
 * ============================================================================
 *
 * Physical cable parameters for typical consumer cables of each type.
 * Values represent the dominant parasitic that shapes the signal:
 * distributed capacitance forms a low-pass filter with the source/load
 * impedance.
 *
 *   RF:        RG-59 coax, 1.5 m (console to TV, short run)
 *   Composite: Cheap molded RCA cable, 2 m (typical console distance)
 *   S-Video:   Quality mini-DIN cable, 1 m (shorter, better shielding)
 *   Component: Quality RCA triple, 1 m (matched set)
 *   RGB:       SCART or BNC, 0.5 m (monitor/PVM distance)
 *   Direct:    No physical cable (reference/test mode)
 */
static const CableParams cable_presets[VIDEO_CONN_COUNT] = {
    [VIDEO_CONN_RF] = {
        .length_meters      = 1.5f,
        .resistance_per_m   = 0.5f,
        .capacitance_per_m  = 67e-12f,      /* 67 pF/m (RG-59 spec) */
        .num_sections       = 4,
        .connector_resistance = 0.5f,
        .impedance          = 75.0f,
        .shield_effectiveness = 0.85f,
    },
    [VIDEO_CONN_COMPOSITE] = {
        .length_meters      = 2.0f,
        .resistance_per_m   = 1.0f,
        .capacitance_per_m  = 80e-12f,      /* 80 pF/m (cheap RCA) */
        .num_sections       = 3,
        .connector_resistance = 1.0f,
        .impedance          = 75.0f,
        .shield_effectiveness = 0.60f,
    },
    [VIDEO_CONN_SVIDEO] = {
        .length_meters      = 1.0f,
        .resistance_per_m   = 0.3f,
        .capacitance_per_m  = 50e-12f,      /* 50 pF/m (quality cable) */
        .num_sections       = 2,
        .connector_resistance = 0.3f,
        .impedance          = 75.0f,
        .shield_effectiveness = 0.90f,
    },
    [VIDEO_CONN_COMPONENT] = {
        .length_meters      = 1.0f,
        .resistance_per_m   = 0.3f,
        .capacitance_per_m  = 50e-12f,      /* 50 pF/m (quality cable) */
        .num_sections       = 2,
        .connector_resistance = 0.3f,
        .impedance          = 75.0f,
        .shield_effectiveness = 0.90f,
    },
    [VIDEO_CONN_RGB] = {
        .length_meters      = 0.5f,
        .resistance_per_m   = 0.2f,
        .capacitance_per_m  = 50e-12f,      /* 50 pF/m (quality cable) */
        .num_sections       = 2,
        .connector_resistance = 0.2f,
        .impedance          = 75.0f,
        .shield_effectiveness = 0.95f,
    },
    [VIDEO_CONN_DIRECT] = {
        /* No cable -- all zeros. */
        .length_meters      = 0.0f,
        .resistance_per_m   = 0.0f,
        .capacitance_per_m  = 0.0f,
        .num_sections       = 0,
        .connector_resistance = 0.0f,
        .impedance          = 0.0f,
        .shield_effectiveness = 0.0f,
    },
};

/* ============================================================================
 * Initialization
 * ============================================================================ */

void video_chain_init_preset(VideoChain *chain, VideoConnectionType conn,
                              VideoCombType comb, int region) {
    memset(chain, 0, sizeof(VideoChain));

    chain->connection = conn;
    chain->comb_type  = comb;

    /* Signal format from region (NTSC/PAL). */
    signal_format_init(&chain->signal_fmt, region);

    /* ---- Stage 2: Console output ---- */
    /* NES composite output: 75 ohm source impedance, 10 uF DC-blocking
     * coupling capacitor (fc ~ 0.21 Hz), amplifier BW ~ 6 MHz. */
    chain->console_coupling_R = 75.0f;          /* ohm */
    chain->console_coupling_C = 10.0e-6f;       /* 10 uF */
    chain->console_amp_bw     = 6.0e6f;         /* 6 MHz */
    chain->console_psu_hum    = 0.0f;           /* clean by default */

    /* ---- Stage 3: Cable ---- */
    chain->cable = cable_presets[conn];

    /* ---- Stage 4: RF modulator ---- */
    chain->rf.enabled        = (conn == VIDEO_CONN_RF);
    chain->rf.carrier_freq   = 61.25e6f;        /* Channel 3 (61.25 MHz) */
    chain->rf.mod_bandwidth  = 3.0e6f;          /* +/- 3 MHz vestigial sideband */
    chain->rf.noise_floor_dbm = -60.0f;         /* -60 dBm thermal noise */
    chain->rf.agc_attack_ms  = 100.0f;          /* 100 ms attack */
    chain->rf.agc_release_ms = 1000.0f;         /* 1 s release */

    /* ---- Stages 5-9: TV display ---- */
    TVDisplayParams *tv = &chain->tv;

    /* Chroma demodulator bandwidth depends on connection quality.
     * Composite/RF: the TV's comb filter output is BW-limited to ~ 1 MHz
     * to suppress cross-color. S-Video and above get a wider passband
     * because Y/C separation is done at the source. */
    if (conn <= VIDEO_CONN_COMPOSITE) {
        tv->chroma_bandwidth = 1.0e6f;          /* 1.0 MHz */
    } else {
        tv->chroma_bandwidth = 1.5e6f;          /* 1.5 MHz */
    }

    /* Luma bandwidth: RF is worst (tuner BW limit), composite is better
     * (direct baseband), S-Video and above get full video bandwidth. */
    switch (conn) {
    case VIDEO_CONN_RF:
        tv->luma_bandwidth = 3.0e6f;            /* 3.0 MHz (tuner limited) */
        break;
    case VIDEO_CONN_COMPOSITE:
        tv->luma_bandwidth = 4.5e6f;            /* 4.5 MHz (NTSC baseband) */
        break;
    default:
        tv->luma_bandwidth = 6.0e6f;            /* 6.0 MHz (full BW) */
        break;
    }

    tv->hue_offset  = 0.0f;                     /* no tint adjustment */
    tv->saturation  = 1.0f;                     /* unity color */

    /* Matrix decode: D65 white point, unity gun drives, zero cutoff. */
    tv->color_temperature = 6500.0f;            /* D65 */
    tv->r_drive   = 1.0f;
    tv->g_drive   = 1.0f;
    tv->b_drive   = 1.0f;
    tv->r_cutoff  = 0.0f;
    tv->g_cutoff  = 0.0f;
    tv->b_cutoff  = 0.0f;

    /* Video amplifier: matched per-gun bandwidth, standard CRT gamma. */
    tv->r_bandwidth = 6.0e6f;
    tv->g_bandwidth = 6.0e6f;
    tv->b_bandwidth = 6.0e6f;
    tv->gamma       = 2.2f;

    /* Beam: moderate sharpness, mild convergence error. */
    tv->beam_sharpness       = 0.7f;
    tv->beam_height_min      = 0.0f;
    tv->beam_height_max      = 1.0f;
    tv->beam_spot_size       = 5.0f;  /* horizontal blur sigma in signal samples */
    tv->convergence_static   = 0.1f;            /* fixed R/B offset */
    tv->convergence_dynamic  = 0.15f;           /* edge-dependent offset */
    tv->h_jitter             = 0.0f;
    tv->v_jitter             = 0.0f;

    /* Phosphor: shadow mask, P22 persistence. */
    tv->mask_type     = VIDEO_MASK_SHADOW;
    tv->mask_pitch_px = 0.50f;                  /* 0.50 mm dot pitch */
    tv->mask_strength = 0.5f;                   /* moderate mask visibility */
    tv->hdr_gain = 0.0f;                        /* off by default */
    tv->persistence_ms = 2.0f;                  /* 2.0 ms (P22 phosphor) */

    /* Glass: mild halation, neutral tint, gentle barrel curvature. */
    tv->halation   = 0.1f;
    tv->glass_tint = 1.0f;                      /* neutral */
    tv->barrel     = 0.02f;                     /* mild curvature */

    /* Environment: gentle vignette, dark room. */
    tv->vignette      = 0.15f;
    tv->ambient_light = 0.0f;                   /* dark room default */

    /* Black level. */
    tv->black_floor   = 0.02f;                  /* typical consumer CRT */

    /* Noise. */
    tv->noise_level   = 0.01f;                  /* mild default */

    /* Mains hum. */
    tv->hum_bar_amplitude = 0.0f;               /* none by default */

    /* Beam bloom. */
    tv->bloom_gamma = 1.5f;                     /* moderate nonlinearity */

    /* Service-menu CRT geometry: centered, 1:1 raster. */
    tv->h_pos  = 0.0f;
    tv->v_pos  = 0.0f;
    tv->h_size = 1.0f;
    tv->v_size = 1.0f;
    tv->top_band_shift = 0.0f;
    tv->top_edge_skew  = 0.0f;
    tv->top_band_start = 18.0f;
    tv->top_band_end   = 34.0f;
    tv->top_edge_width = 0.08f;
}

/* ============================================================================
 * Stage activation query
 * ============================================================================
 *
 * Returns whether a given pipeline stage is active for the current
 * connection type. The GPU dispatch loop uses this to skip kernel
 * invocations for stages that don't apply.
 *
 * Stage numbering matches the header comment in video_chain.h:
 *   1  = 2C02 DAC
 *   2  = Console output
 *   3  = Cable / transmission
 *   4  = RF modulator + demodulator
 *   5  = TV input (coupling + AGC)
 *   6  = Comb filter / Y-C separator
 *   7  = Chroma demodulator
 *   8  = Luma processing
 *   9  = Matrix decode (YIQ -> RGB)
 *   10 = Video amplifier
 *   11 = Electron beam
 *   12 = Phosphor screen
 *   13 = CRT glass
 *   14 = Environment
 */
bool video_chain_stage_active(const VideoChain *chain, int stage) {
    VideoConnectionType c = chain->connection;

    switch (stage) {
    /* Stages 1-2: always active (DAC + console output). */
    case 1:
    case 2:
        return true;

    /* Stage 3: cable -- active unless direct connection. */
    case 3:
        return c != VIDEO_CONN_DIRECT;

    /* Stage 4: RF modulator/demodulator -- RF path only. */
    case 4:
        return c == VIDEO_CONN_RF;

    /* Stage 5: TV input (coupling + AGC) -- active unless direct. */
    case 5:
        return c != VIDEO_CONN_DIRECT;

    /* Stage 6: Comb filter / Y-C separator.
     * RF and Composite need comb filtering to separate Y and C.
     * S-Video bypasses (Y/C already separated at source).
     * Component and RGB skip entirely (no composite signal). */
    case 6:
        return c == VIDEO_CONN_RF || c == VIDEO_CONN_COMPOSITE;

    /* Stage 7: Chroma demodulator.
     * Active for any connection carrying modulated chroma:
     * RF, Composite, and S-Video all have QAM-encoded color.
     * Component carries Cb/Cr baseband, RGB has no chroma encoding. */
    case 7:
        return c == VIDEO_CONN_RF || c == VIDEO_CONN_COMPOSITE
            || c == VIDEO_CONN_SVIDEO;

    /* Stage 8: Luma processing -- always active. */
    case 8:
        return true;

    /* Stage 9: Matrix decode (YIQ/YCbCr -> RGB).
     * Not needed for RGB connections (signal is already RGB).
     * All other connections require color space conversion. */
    case 9:
        return c != VIDEO_CONN_RGB && c != VIDEO_CONN_DIRECT;

    /* Stages 10-14: display domain -- always active. */
    case 10:
    case 11:
    case 12:
    case 13:
    case 14:
        return true;

    default:
        return false;
    }
}
