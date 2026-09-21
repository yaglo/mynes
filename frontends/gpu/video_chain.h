/* Signal/receiver and CRT controls. Connection routing is defined below;
 * current stage order and units are documented in gpu-pipeline-reference.md. */

#ifndef VIDEO_CHAIN_H
#define VIDEO_CHAIN_H

#include "signal_format.h"
#include <stdbool.h>
#include <math.h>

/* ============================================================================
 * Connection types
 * ============================================================================ */

typedef enum {
    VIDEO_CONN_RF = 0,          /* NES → RF modulator → coax → TV tuner */
    VIDEO_CONN_COMPOSITE,       /* NES → RCA cable → TV composite input */
    VIDEO_CONN_SVIDEO,          /* NES (mod) → S-Video cable → TV S-Video */
    VIDEO_CONN_COMPONENT,       /* NES (mod) → component cables → TV */
    VIDEO_CONN_RGB,             /* NES (mod) → RGB SCART → TV */
    VIDEO_CONN_DIRECT,          /* No cable (test/reference mode) */
    VIDEO_CONN_COUNT
} VideoConnectionType;

/* RF/composite/Y-C require carrier demodulation. Component/RGB/Direct use
 * the ideal separated-output modification: integrate the measured DAC's
 * DC and quadratures, then form gun voltages before the same CRT stages.
 * An ideal YPbPr encode/decode cancels; its cable/ADC losses are not modeled. */
static inline bool video_connection_uses_signal_decode(VideoConnectionType c) {
    return c == VIDEO_CONN_RF || c == VIDEO_CONN_COMPOSITE || c == VIDEO_CONN_SVIDEO;
}

/* ============================================================================
 * Comb filter types
 * ============================================================================ */

typedef enum {
    VIDEO_COMB_NONE = 0,        /* Notch/bandwidth Y/C separation; no line comb */
    VIDEO_COMB_1LINE,           /* Two scanlines, 1H delay */
    VIDEO_COMB_2LINE,           /* Legacy adaptive three-line separator */
    VIDEO_COMB_3LINE,           /* Three scanlines, centred 2H delay store */
    VIDEO_COMB_BYPASS,          /* S-Video input: Y/C already separated */
} VideoCombType;

/* Map a VideoCombType enum value to the comb-filter compute shader's
 * internal "mode" number. The shader treats 0 as "bypass" (C = 0) and
 * 1/2/3 as active 1-/2-/3-line combs. VIDEO_COMB_NONE and
 * VIDEO_COMB_BYPASS both map to shader mode 0 (the shader produces no
 * chroma); only the 1/2/3-line values produce actual Y/C separation.
 * Returns 0 for out-of-range input, same as the shader's bypass. */
static inline unsigned video_chain_comb_shader_mode(VideoCombType t) {
    switch (t) {
        case VIDEO_COMB_1LINE: return 1;
        case VIDEO_COMB_2LINE: return 2;
        case VIDEO_COMB_3LINE: return 3;
        case VIDEO_COMB_NONE:
        case VIDEO_COMB_BYPASS:
        default:               return 0;
    }
}

/* Does the comb filter actually separate Y/C at this mode? False for
 * NONE and BYPASS (both produce C=0 in the shader); true for 1/2/3 line. */
static inline bool video_chain_comb_mode_separates(VideoCombType t) {
    return video_chain_comb_shader_mode(t) > 0u;
}

/* ============================================================================
 * Chroma aux buffer layout
 * ============================================================================
 *
 * The chroma demod and the following I / Q FIR lowpasses all ping-pong
 * through the 4-slot aux[] array. The concrete slot numbers depend on
 * whether the comb filter is actively producing a separated chroma
 * signal (which lives in aux[0]):
 *
 *   comb OFF           comb ON (1/2/3-line)
 *   ─────────          ─────────────────────
 *   aux[0] = I raw     aux[0] = C (comb out)
 *   aux[1] = Q raw     aux[1] = I raw
 *   aux[2] = I filt    aux[2] = Q raw
 *   aux[3] = Q filt    aux[3] = I filt
 *                      aux[0] = Q filt (wrap; C no longer needed)
 *
 * Historically these offsets were scattered as bare ternary expressions
 * at every dispatch site (if (comb_active) ... else ...). Centralising
 * them here makes the routing inspectable and unit-testable, and keeps
 * every future change in lockstep across demod, I-FIR, Q-FIR, and the
 * final matrix-decode read. */
typedef struct {
    int i_raw;         /* aux[] slot modulator writes I output to       */
    int q_raw;         /* aux[] slot modulator writes Q output to       */
    int i_filt;        /* aux[] slot chroma I FIR writes to             */
    int q_filt;        /* aux[] slot chroma Q FIR writes to             */
    /* Matrix-decode reads the filtered I and Q from (i_filt, q_filt). */
} ChromaAuxLayout;

static inline ChromaAuxLayout video_chain_chroma_aux_layout(bool comb_active) {
    ChromaAuxLayout L;
    if (comb_active) {
        L.i_raw  = 1;   /* aux[0] holds C from comb; demod writes I→1, Q→2 */
        L.q_raw  = 2;
        L.i_filt = 3;
        L.q_filt = 0;   /* wraps into slot 0 since C no longer read */
    } else {
        L.i_raw  = 0;
        L.q_raw  = 1;
        L.i_filt = 2;
        L.q_filt = 3;
    }
    return L;
}

/* ============================================================================
 * Phosphor mask types
 * ============================================================================ */

typedef enum {
    VIDEO_MASK_SHADOW = 0,      /* Shadow mask: RGB dot triads */
    VIDEO_MASK_APERTURE_GRILLE, /* Aperture grille: vertical RGB stripes (Trinitron) */
    VIDEO_MASK_SLOT,            /* Slot mask: rectangular RGB groups */
} VideoMaskType;

/* ============================================================================
 * Cable parameters (shared between video and audio cables)
 * ============================================================================ */

typedef struct {
    float length_meters;        /* physical cable length */
    float resistance_per_m;     /* series resistance (Ω/m), typically 0.1-2.0 */
    float capacitance_per_m;    /* shunt capacitance (F/m), typically 50-100 pF/m */
    int   num_sections;         /* RC ladder approximation order (2-8) */
    float connector_resistance; /* total contact R at both ends (Ω) */
    float impedance;            /* characteristic impedance (Ω), typically 75 */
    float shield_effectiveness; /* 0=none, 1=perfect */
    int   ghost_delay;          /* impedance reflection delay in signal samples */
    float ghost_level;          /* ghost amplitude (0=none, 0.10=visible) */
} CableParams;

/* ============================================================================
 * RF modulator parameters
 * ============================================================================ */

typedef struct {
    bool  enabled;              /* only true for RF connection */
    float carrier_freq;         /* Ch 3 = 61.25 MHz, Ch 4 = 67.25 MHz */
    float carrier_level_dbm;    /* sync-tip carrier power; 0 legacy -> -20 dBm */
    float mod_bandwidth;        /* equivalent detected-video lowpass edge, Hz */
    float noise_floor_dbm;      /* total additive channel noise before video filtering */
    float agc_attack_ms;        /* AGC attack time constant */
    float agc_release_ms;       /* AGC release time constant */
} RFModulatorParams;

static inline float video_rf_noise_rms(const RFModulatorParams *rf) {
    float carrier=rf->carrier_level_dbm!=0 ? rf->carrier_level_dbm : -20;
    // Complex AWGN divides total power equally between I and Q. Convert
    // a power difference in dB to amplitude with /20, never the old /26.
    float noise=rf->noise_floor_dbm!=0 ? rf->noise_floor_dbm : -70;
    return .70710678118f*powf(10,(noise-carrier)/20);
}

/* ============================================================================
 * TV / display parameters
 * ============================================================================ */

typedef struct {
    /* Chroma demodulator. */
    float chroma_bandwidth;     /* I-channel BW, Hz (0.5-1.5 MHz — "wide" chroma) */
    /* Q-channel bandwidth.  NTSC specs I = 1.3 MHz, Q = 0.5 MHz; most
     * consumer TVs used equiband I/Q (Q = chroma_bandwidth).  0 here
     * means "match chroma_bandwidth" — back-compat with presets that
     * predate separate Q bandwidth. */
    float chroma_q_bandwidth;   /* Hz (0.3-1.5 MHz), 0 = equal-band */
    float luma_bandwidth;       /* Hz (3.0-6.0 MHz) */
    float hue_offset;           /* degrees (tint control) */
    float saturation;           /* multiplier (color control) */
    float decoder_red_gain;     /* R-Y gain offset; 0 = unity, +0.15 = red push */
    float decoder_blue_gain;    /* B-Y gain offset; independent of white balance */
    float fir_ringing;          /* FIR window blend: 0=Hamming, 1=rect (Gibbs ringing) */
    float luma_peaking;         /* TV sharpness: 0=off, 0.3=moderate, 0.8=aggressive edge boost */
    /* Subcarrier notch depth in Y FIR. 0.95 = -26 dB (clean, default,
     * kills dot crawl + cross-color). 0.0 = notch disabled (subcarrier
     * passes into Y → consumer-TV rainbow fringes + dot crawl).
     * Only effective with fir_y_n ≥ 23 (shorter FIRs can't host a
     * clean notch and the notch is skipped). */
    float luma_notch_depth;
    float rf_interference;      /* RF interference jitter in signal samples (0=none) */
    float geometry_warp;        /* horizontal geometry distortion (0=perfect, 3=visible) */

    /* Matrix decode. */
    float color_temperature;    /* Kelvin (3200-9300) */
    float r_drive, g_drive, b_drive;   /* per-gun gain (0.8-1.2) */
    float r_cutoff, g_cutoff, b_cutoff; /* per-gun black level (±0.05) */

    /* Video amplifier. */
    float r_bandwidth, g_bandwidth, b_bandwidth; /* per-gun BW (5-8 MHz) */
    float gamma;                /* CRT phosphor gamma (2.2-2.5) */

    /* Beam. */
    float beam_sharpness;       /* dark-beam focus control (0-2, higher narrows) */
    float beam_height_min;      /* min scanline height (dark) */
    float beam_height_max;      /* max scanline height (bright) */
    float beam_fwhm_min;        /* low-current spot FWHM in scanlines; 0 = legacy focus */
    float beam_fwhm_max;        /* white-current spot FWHM in scanlines; 0 = legacy height */
    float beam_spot_size;       /* horizontal blur in signal samples (1-12) */
    float beam_spot_growth;     /* fractional horizontal sigma growth at white current */
    float convergence_static;   /* fixed R/B offset (0-1) */
    float convergence_dynamic;  /* edge-dependent offset (0-1) */
    float conv_r_x;             /* red horizontal offset in signal samples */
    float conv_r_y;             /* red vertical offset in output rows */
    float conv_b_x;             /* blue horizontal offset in signal samples */
    float conv_b_y;             /* blue vertical offset in output rows */
    float h_jitter;             /* horizontal scan jitter amplitude */
    float v_jitter;             /* vertical frame jitter amplitude */

    /* Phosphor. */
    VideoMaskType mask_type;
    int phosphor_gamut;         /* 0=709 compatibility, 1=nominal 525, 2=nominal 625 */
    float mask_triads;          /* RGB triads across the tube; 0 uses legacy pixel pitch */
    float mask_pitch_px;        /* phosphor cell spacing in drawable pixels */
    float mask_strength;        /* phosphor mask blend (0=off, 0.6=visible, 1.0=full) */
    int   subpixel_layout;      /* 0=none, 1=RGB stripe, 2=BGR stripe */
    float persistence_ms;       /* phosphor decay time (1-3 ms for P22) */

    /* Glass. */
    float halation;             /* light scattering in glass (0-0.3) */
    float glass_tint;           /* multiplier (0.6-0.9) */
    float barrel;               /* curvature (0=flat, 0.05=classic) */

    /* Environment. */
    float overscan;             /* bezel crop fraction per edge (0=none, 0.05=5%) */
    float vignette;             /* corner darkening (0-0.3) */
    float ambient_light;        /* reflected room light (0=dark, 0.15=lit room) */

    /* Black level. */
    float black_floor;          /* minimum output level (PVM: 0.005, consumer: 0.02-0.03) */

    /* Noise. */
    float noise_level;          /* receiver output voltage noise, before gun transfer and spot spread */

    /* Mains hum. */
    float hum_bar_amplitude;    /* hum bar strength (0=none, 0.08=visible band) */

    /* Beam bloom. */
    float bloom_gamma;          /* bloom nonlinearity (1.0=linear, 1.8=late bloom) */

    /* Beam physics. */
    float edge_focus;           /* focus degradation at edges (0=perfect, 0.3=visible) */
    float velocity_dim;         /* edge dimming from beam velocity (0=none, 0.15=visible) */

    /* Horizontal beam-edge behaviour. Modeled at CPU waveform-gen time
     * (waveform_apply_beam_edges) so the FIR stage sees the right data.
     *
     * beam_edge_fade:
     *   Force the leftmost / rightmost N NES pixels of each scanline to
     *   blanking level (0). Represents back-porch settle + front-porch
     *   decay where the beam hasn't reached active-signal voltage.
     *   Combined with the per-line FIR zero-pad this produces a soft
     *   luma fade at the screen's left and right edges. 0 = off.
     *   Typical: 0.5 … 2.0 NES pixels.
     *
     * beam_edge_overshoot:
     *   Single-cycle amplitude boost on the first/last M samples after
     *   the fade region. Models deflection-amp ringing on retrace
     *   turn-around — a brief brightening or darkening at the extreme
     *   edges. 0 = clean edge, 0.3 = visibly ringing.
     *
     * burst_lock_drift:
     *   Chroma hue error, in degrees, applied uniformly across the
     *   leftmost burst_lock_drift_width NES pixels of each scanline.
     *   Models a weak / short color-burst where the decoder PLL hasn't
     *   settled yet, so the first few columns decode with a phase
     *   offset. 0 = perfect lock. Typical: ±5 … ±20°.
     *
     * burst_lock_drift_width:
     *   How many NES pixels on the left are affected by the drift,
     *   expressed in NES pixel units. 0 = off. Typical: 4 … 16 px. */
    float beam_edge_fade;
    float beam_edge_overshoot;
    float burst_lock_drift;
    float burst_lock_drift_width;

    /* Post-amplifier video-rail loading. A causal 12 us RC follows gun
     * current, reducing emission after bright patches and recovering after
     * dark ones. Generic supply model: 0=regulated, .08=visible, .20=worn. */
    float beam_current_load;
    /* Incomplete video DC restoration: causal luma-dependent bias error.
     * Separate from supply/gain sag; 0 disables horizontal dark/bright wakes. */
    float video_black_droop;
    float video_recovery_us;    /* bias recovery time; 0 selects nominal 18 us */

    /* Barrel distortion. */
    float barrel_v;             /* vertical curvature (0=same as barrel) */

    /* Additional geometry (real CRTs aren't perfectly calibrated). */
    float keystone;             /* trapezoidal distortion (-0.1 to +0.1) */
    float rotation;             /* image rotation in radians (-0.05 to +0.05) */
    float skew_x;               /* horizontal parallelogram shear (-0.1 to +0.1) */
    float skew_y;               /* vertical parallelogram shear (-0.1 to +0.1) */
    float hv_sag;               /* load-dependent raster size: +contracts, -expands */

    /* CRT service-menu geometry (HPOS/VPOS/HSIZE/VSIZE).
     * Size 1.0 = raster fills the tube; <1.0 shows the tube edge; >1.0 overscan.
     * Pos 0.0 = centered; ±0.5 shifts a full half-tube in either direction. */
    float h_pos;                /* horizontal raster shift (-0.2 to +0.2 typical) */
    float v_pos;                /* vertical raster shift (-0.2 to +0.2 typical) */
    float h_size;               /* horizontal raster size (0.5 - 1.5) */
    float v_size;               /* vertical raster size (0.5 - 1.5) */

    /* PSU-driven beam instability (vertical wobble + focus modulation). */
    float focus_breathing;      /* focus growth with local/regulated gun load (0-0.2) */
    float scanline_wobble;      /* per-line sinusoidal curvature from HV ripple (0-0.5) */
    /* Localized top-of-raster geometry faults from vertical retrace /
     * yoke settle. Lets a band of scanlines drift or kink without
     * globally warping the whole screen.
     *
     * top_band_shift:
     *   full-line horizontal offset applied to scanlines in the band.
     *   Units are beam-output pixels. 0 = off.
     *
     * top_edge_skew:
     *   additional left-edge-only offset in the same band. Models the
     *   classic consumer-TV fault where the first part of a line starts
     *   in the wrong place while the center remains mostly correct.
     *
     * top_band_start / top_band_end:
     *   scanline range in NES line numbers (0..239).
     *
     * top_edge_width:
     *   normalized width of the left-edge-localized skew region. */
    float top_band_shift;
    float top_edge_skew;
    float top_band_start;
    float top_band_end;
    float top_edge_width;

    /* Motion-adaptive 3D comb filter. */
    float motion_threshold;     /* luma delta for static/moving classification (0.05-0.15) */

    /* Halation bloom per-channel tint — biases the phosphor halo colour.
     * Because the halation shader already blurs RGB uniformly, red/green/
     * blue SOURCES already produce red/green/blue halos (colour preserved
     * by uniform blur). This tint is the *additional* bias that colours
     * NEUTRAL highlights: P22 consumer sets bloom white-to-green-warm
     * because the green phosphor's lateral emission dominates. Set to
     * (0.9, 1.2, 0.7) for P22 green-warm bias, (1,1,1) for clean
     * uniform bloom (aperture-grille pro monitors). All-zero → treated
     * as (1,1,1) by the shader so pre-existing presets look unchanged. */
    float halation_tint_r;
    float halation_tint_g;
    float halation_tint_b;

    /* Phosphor persistence per-channel (P22 colour difference). */
    float persistence_r;        /* red persistence weight (0.0-1.0, relative to green) */
    float persistence_g;        /* green persistence weight (1.0 = reference) */
    float persistence_b;        /* blue persistence weight (0.0-1.0, relative to green) */

    /* Colour killer. */
    float color_killer;         /* luma threshold for chroma kill (0=disabled, 0.08=old TV) */

    /* HDR gain. */
    float hdr_gain;             /* output multiplier (1.0=normal, 1.5=compensate mask, 2.0=bright) */

    /* Per-phosphor gamma offset (§3.6). P22 R/G/B have slightly
     * different response curves; these are added to the global
     * encoder exponent per channel. Zero = no per-channel difference
     * (legacy behavior). Typical P22: R +0.02, G -0.01, B +0.03. */
    float phosphor_gamma_offset_r;
    float phosphor_gamma_offset_g;
    float phosphor_gamma_offset_b;

    /* Secondary electron scattering (§4.8). Electrons bouncing off
     * the shadow mask hit adjacent phosphors of other colors, softly
     * desaturating everything. Blend each channel toward the pixel's
     * luminance mean by this amount. 0 = pristine, 0.05 = consumer,
     * 0.12 = well-worn cheap set. */
    float secondary_scatter;

    /* Glass internal reflection pedestal (§5.6). Light bouncing
     * between the inner glass surface and the aluminum backing raises
     * the effective black level proportional to local brightness.
     * Different from ambient_light (constant pedestal). */
    float glass_reflection;

    /* Anti-glare sub-pixel blur (§5.6). Matte-screen treatment
     * slightly blurs the image as well as scattering reflections.
     * Amount in texels of the composite texture (0 = glossy, 0.5 =
     * heavy matte). */
    float antiglare_blur;

    /* EMI brightness gradient (§5.9). Horizontal-deflection field
     * coupling makes one edge of the active raster slightly brighter
     * than the other. Amplitude of the peak lift (0 = none, 0.05 =
     * visible on flat-field test pattern). */
    float emi_gradient;

    /* Degauss residual tint (§6.1). Spatially smooth per-corner
     * color deviation; max at the corners. Amplitude of the largest
     * per-channel offset (0 = clean degauss, 0.03 = typical
     * mid-life). */
    float degauss_tint;

    /* Phosphor grain (§5.1). Fine high-frequency multiplicative
     * noise, static per tube. Seed hashes spatial position so the
     * pattern doesn't swim. 0 = none, 0.02 = visible on flat fields. */
    float phosphor_grain;

    /* Cathode aging / non-uniformity (§5.3). Center of the screen
     * dims vs edges; the three guns also age at different rates.
     * cathode_center_dim: fraction the center is darker than the
     * rim (0 = uniform, 0.15 = well-used tube).
     * cathode_gain_r/g/b: per-gun global gain (1 = no aging). */
    float cathode_center_dim;
    float cathode_gain_r;
    float cathode_gain_g;
    float cathode_gain_b;

    /* §4.9 APL-dependent black level — the DC-restoration circuit in
     * the video amp shifts the black level with average picture
     * level. apl_black_lift = amplitude (0 = flat, 0.08 = consumer
     * cheap-set). Bright scenes lift the black floor, dark scenes
     * push it down. The slow EMA tracker lives in GPURenderCtx. */
    float apl_black_lift;

    /* §5.2 Thermal-mask doming approximation. Short version of the
     * effect — per-channel multi-second EMA of the frame's color
     * load tints the whole image as the guns drift apart in
     * temperature. thermal_dome_amount scales the resulting tint.
     * 0 = cold-mask accurate, 0.06 = ~20 minutes of red-heavy
     * content. The trackers live in GPURenderCtx. */
    float thermal_dome_amount;

    /* §4.2 Astigmatic corner focus. Regular edge_focus widens the
     * beam uniformly near edges; this adds a directional bias so
     * the beam stretches along the radial axis at corners (as on
     * real CRTs where the yoke focus error is anisotropic).
     * 0 = isotropic blur, 0.3 = pronounced radial streak. */
    float corner_astigmatism;

    /* §5.4 Phosphor chromaticity shift with drive level. Each gun's
     * spectral peak shifts slightly at high drive (green turns
     * slightly bluer, red stays near-constant but differs between
     * fresh and decayed light, blue saturates). Amplitude of the
     * overall drive-dependent hue rotation. 0 = stable gamut,
     * 0.08 = measurable on a spectroradiometer. */
    float chromaticity_drive_shift;

    /* §4.1 Velocity modulation — beam acceleration/deceleration at
     * transitions. 0 = off, 0.3 = visible edge warping (high-end sets). */
    float velocity_mod;

    /* §5.7 Asymmetric video-amp rise/fall (simplified — approximated
     * by a derivative-sign bias rather than a proper asymmetric IIR).
     * 0 = symmetric FIR, 0.3 = visibly softer rises than falls. */
    float asym_rise_fall;

    /* §5.7 Vertical (scanline-to-scanline) video-amp smearing from
     * cathode-cap capacitance memory. 0 = none, 0.15 = visible faint
     * downward ghost of sharp horizontal edges. */
    float vertical_smear;

    /* §6.2 Microphonic audio-coupled raster wobble. Speaker
     * vibrations jiggle the electron gun assembly, producing
     * sub-pixel image wobble correlated with bass frequencies.
     * Amplitude is the peak raster offset as fraction of screen;
     * 0 = no wobble, 0.006 = typical arcade cabinet. */
    float microphonic_amount;

    /* §6.3 Glass-face glare — the actual specular reflection of the
     * viewer's environment on the tube front. Distinct from
     * ambient_light (DC pedestal) and glass_reflection (internal
     * bouncing) and halation (phosphor-driven bloom): this is a
     * reflection of the ROOM on the outer glass surface, Fresnel-
     * weighted so it peaks at the edges.
     *
     * glass_glare: master amplitude. 0 = clean, 0.15 = sun-lit room.
     * glass_glare_light_x / _y: simulated light source position in
     *     tube-face UV space (0,0 = top-left, 1,1 = bottom-right).
     *     Defaults 0.3 / 0.25 put an overhead lamp slightly off-axis.
     * glass_glare_size: angular size of the specular highlight,
     *     0 = point source, 0.3 = soft window reflection.
     * glass_glare_temp_k: colour temperature tint for the reflected
     *     light (warm tungsten ≈ 3200 K, cool daylight ≈ 6500 K,
     *     0 = no tint). */
    float glass_glare;
    float glass_glare_light_x;
    float glass_glare_light_y;
    float glass_glare_size;
    float glass_glare_temp_k;
} TVDisplayParams;

/* Legacy presets keep their spot widths. New controls expose FWHM directly,
 * avoiding three overlapping controls with different hidden scale factors. */
static inline float video_beam_sigma(const TVDisplayParams *tv, bool bright) {
    float fwhm = bright ? tv->beam_fwhm_max : tv->beam_fwhm_min;
    if (fwhm > 0.0f) return fmaxf(0.05f, fminf(fwhm / 2.354820045f, 1.0f));
    return bright ? fmaxf(0.20f, 0.30f + tv->beam_height_max * 0.33f)
        : fmaxf(0.10f, (0.35f - tv->beam_sharpness * 0.20f)
                       * (0.4f + tv->beam_height_min * 0.8f));
}

/* ============================================================================
 * Complete video chain configuration
 * ============================================================================ */

typedef struct {
    VideoConnectionType connection;
    VideoCombType       comb_type;
    /* Comb notch depth override (0..1). 0 = use the per-comb-type default
     * baked into video_gpu.c. Higher = cleaner Y/C separation (PVM look);
     * lower = more subcarrier bleeds into Y (consumer-TV cross-color,
     * rainbow fringes on sharp luma edges, visible dot crawl). */
    float               comb_notch_depth;
    SignalFormat         signal_fmt;

    /* Stage parameters. */
    CableParams         cable;
    RFModulatorParams   rf;
    TVDisplayParams     tv;

    /* Console output stage. */
    float console_coupling_R;   /* output impedance (Ω) */
    float console_coupling_C;   /* coupling cap (F) */
    float console_amp_bw;       /* amp bandwidth (Hz) */
    float console_phase_distortion_ns; /* nonlinear output impedance, 0 disables */
    float console_psu_hum;      /* PSU hum amplitude */
} VideoChain;

/* A short lead's lumped shunt capacitance driven by the parallel source
 * and termination resistances. This is a first-order approximation, not
 * measured skin/dielectric loss or a transmission-line solution. */
static inline float video_cable_bandwidth(const VideoChain *c) {
    float source=c->console_coupling_R>0 ? c->console_coupling_R : 75.0f;
    float load=c->cable.impedance>0 ? c->cable.impedance : 75.0f;
    float length=fmaxf(c->cable.length_meters,0.0f);
    float r=source*load/(source+load)+length*fmaxf(c->cable.resistance_per_m,0.0f)
           +fmaxf(c->cable.connector_resistance,0.0f);
    float tau=r*length*fmaxf(c->cable.capacitance_per_m,0.0f);
    return tau>0 ? fminf(200e6f,1.0f/(6.28318530718f*tau)) : 200e6f;
}
static inline float video_console_bandwidth(const VideoChain *c) {
    // Old files accidentally stored the AUDIO 14 kHz corner in this field.
    return c->console_amp_bw>=100000.0f ? c->console_amp_bw : 6e6f;
}

/* Initialize video chain with defaults for a given connection type. */
void video_chain_init_preset(VideoChain *chain, VideoConnectionType conn,
                              VideoCombType comb, int region);

/* Query which stages are active for the current connection type. */
bool video_chain_stage_active(const VideoChain *chain, int stage);

#endif /* VIDEO_CHAIN_H */
