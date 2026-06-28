/*
 * Physical Presets — Historically-Accurate Analog Signal Chain Configurations
 * ============================================================================
 *
 * A PhysicalPreset bundles every parameter needed to configure both the
 * video and audio signal chains for a specific real-world NES viewing
 * setup. Each field (resistor, capacitor, cable, CRT spec, audio stage)
 * is physically plausible.
 *
 * Presets live on disk as JSON files under presets/. The GPU frontend
 * scans that directory at startup, loads each file into a PhysicalPreset
 * via preset_json_load(), and offers them in the OSD menu + P-key cycle.
 */

#ifndef PRESETS_H
#define PRESETS_H

#include "video_chain.h"
#include "audio_chain.h"

/* ============================================================================
 * Console variant enumeration (matches audio_chain.h defines)
 * ============================================================================ */

typedef enum {
    PRESET_CONSOLE_FAMICOM   = AUDIO_CONSOLE_FAMICOM,
    PRESET_CONSOLE_NES_FRONT = AUDIO_CONSOLE_NES_FRONT,
    PRESET_CONSOLE_NES_TOP   = AUDIO_CONSOLE_NES_TOP,
    PRESET_CONSOLE_DENDY     = AUDIO_CONSOLE_DENDY,
} PresetConsoleVariant;

/* ============================================================================
 * Physical preset structure
 * ============================================================================
 * Bundles every parameter needed to configure both the video and audio
 * signal chains for a particular physical setup.
 */

typedef struct {
    /* ---- Identity ---- */
    char name[128];                 /* short name for UI display */
    char description[512];          /* one-line description of the setup */

    /* ---- Signal path topology ---- */
    VideoConnectionType connection;
    VideoCombType       comb_type;
    PresetConsoleVariant console_variant;
    int                 speaker_type;   /* AUDIO_SPEAKER_* */
    int                 region;         /* SIGNAL_REGION_NTSC or SIGNAL_REGION_PAL */

    /* ---- Cable parameters ---- */
    CableParams         video_cable;
    CableParams         audio_cable;

    /* ---- TV / CRT display ---- */
    TVDisplayParams     tv;

    /* ---- Video console output stage ---- */
    float console_coupling_R;       /* output impedance (ohms) */
    float console_coupling_C;       /* coupling cap (farads) */
    float console_amp_bw;           /* amp bandwidth (Hz) */
    float console_psu_hum;          /* PSU hum into video signal */

    /* ---- RF modulator (RF presets only) ---- */
    RFModulatorParams   rf;

    /* ---- Signal decode overrides ---- */
    float brightness;               /* Y offset (default 0.0) */
    float contrast;                 /* Y multiplier (default 1.0) */
    float chroma_gain;              /* post-demod I/Q multiplier (0=use default 1.3) */
    /* Comb-filter notch depth (1-line / 2-line / 3-line comb modes only).
     * 1.0 = perfect Y/C separation (PVM look, no cross-color)
     * 0.85 = decent consumer TV (~16 dB rejection)
     * 0.65 = cheap set (~9 dB, noticeable dot crawl + rainbow fringes)
     * 0.00 = no rejection (raw composite in Y)
     * Zero in JSON → falls back to a per-mode default in video_gpu.c. */
    float comb_notch_depth;

    /* ---- Audio-specific overrides ---- */
    float audio_psu_hum_amplitude;  /* peak hum voltage (0-0.02) */
    float audio_noise_floor;        /* peak noise level (0-0.02) */
    float audio_saturation_drive;   /* 1.0 = linear, 4.0 = heavy */
    float audio_cable_length_m;     /* audio cable length (may differ from video) */
} PhysicalPreset;

#endif /* PRESETS_H */
