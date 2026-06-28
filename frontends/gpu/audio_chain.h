/*
 * Audio Signal Chain — Stage Definitions
 * ========================================
 *
 * Defines the audio signal path from APU DAC output to speaker output
 * as a sequence of composable kernel stages. Each stage maps to one or
 * more GPU compute dispatches.
 *
 * The chain is:
 *   [CPU: APU channels → DAC → mixer → float samples]
 *     ↓ upload to GPU
 *   Stage 1: Coupling capacitor (RC high-pass, DC blocking)
 *   Stage 2: Feedback network (RC high-pass, bass shaping)
 *   Stage 3: Amplifier bandwidth (RC low-pass)
 *   Stage 4: Amplifier saturation (pointwise, tanh soft-clip)
 *   Stage 5: PSU hum injection (pointwise, additive sine)
 *   Stage 6: Noise floor (pointwise, additive white noise)
 *   Stage 7: Cable capacitance (RC low-pass, length-dependent)
 *   Stage 8: TV input coupling (RC high-pass)
 *   Stage 9: Speaker model (FIR or biquad chain)
 *   Stage 10: Decimation (FIR, 1.79 MHz → 48 kHz)
 *     ↓ readback to CPU → audio output
 *
 * NOTE: This is the VERIFICATION path — CPU audio is the primary
 * real-time output. The GPU chain exists for A/B testing, offline
 * rendering, and future latency-hiding use.
 *
 * Every stage can be individually bypassed (enabled flag). Connection
 * type determines which cable/TV stages are active.
 */

#ifndef AUDIO_CHAIN_H
#define AUDIO_CHAIN_H

#include "audio_format.h"
#include <stdbool.h>

/* ============================================================================
 * Stage parameter structures
 * ============================================================================
 * Each struct holds the PHYSICAL component values (R, C in SI units).
 * The chain runner converts these to IIR/FIR coefficients at the
 * processing sample rate before dispatching GPU kernels.
 */

/* RC filter stage (used by stages 1, 2, 3, 7, 8). */
typedef struct {
    bool  enabled;
    bool  is_highpass;      /* true = HP (coupling cap), false = LP (bandwidth) */
    float resistance;       /* ohms */
    float capacitance;      /* farads */
    /* Precomputed IIR coefficients (set by audio_chain_prepare). */
    float a, b;
} AudioRCStage;

/* Amplifier saturation (stage 4). */
typedef struct {
    bool  enabled;
    float drive;            /* 1.0 = linear, 4.0 = heavy compression */
} AudioSaturationStage;

/* PSU hum injection (stage 5). */
typedef struct {
    bool  enabled;
    float amplitude;        /* peak voltage of mains hum (0-0.02) */
    float frequency;        /* 60 Hz (NTSC) or 50 Hz (PAL) */
    float harmonic_2;       /* 2nd harmonic amplitude (120/100 Hz) */
    float harmonic_3;       /* 3rd harmonic amplitude (180/150 Hz) */
    float phase;            /* streaming phase state (updated per frame) */
} AudioHumStage;

/* Noise floor (stage 6). */
typedef struct {
    bool  enabled;
    float amplitude;        /* peak noise level (0-0.02) */
    uint32_t rng_state;     /* xorshift32 seed (streaming) */
} AudioNoiseStage;

/* Speaker model (stage 9). */
typedef struct {
    bool  enabled;
    SpeakerParams params;   /* from audio_format.h */
    /* Precomputed biquad coefficients (set by audio_chain_prepare). */
    float biquad_resonance[5]; /* a0, a1, a2, b1, b2 for resonance peak */
    float biquad_rolloff[5];   /* a0, a1, a2, b1, b2 for high-freq rolloff */
} AudioSpeakerStage;

/* Decimation (stage 10). */
typedef struct {
    bool  enabled;
    int   tap_count;        /* FIR taps (65-129) */
    int   decimation_ratio; /* input_rate / output_rate */
    float *taps;            /* FIR coefficients (allocated) */
} AudioDecimationStage;

/* ============================================================================
 * Complete audio chain configuration
 * ============================================================================ */

typedef struct {
    /* Hardware variant determines default component values. */
    int console_variant;    /* 0=Famicom, 1=NES Front, 2=NES Top, 3=Dendy */

    /* Signal stages (in processing order). */
    AudioRCStage        coupling_cap;       /* Stage 1: DC blocking */
    AudioRCStage        feedback_network;   /* Stage 2: bass shaping */
    AudioRCStage        amp_bandwidth;      /* Stage 3: amp LP */
    AudioSaturationStage amp_saturation;    /* Stage 4: soft clip */
    AudioHumStage       psu_hum;            /* Stage 5: mains hum */
    AudioNoiseStage     noise_floor;        /* Stage 6: thermal noise */
    AudioRCStage        cable;              /* Stage 7: cable capacitance */
    AudioRCStage        tv_input_coupling;  /* Stage 8: TV input cap */
    AudioSpeakerStage   speaker;            /* Stage 9: speaker model */
    AudioDecimationStage decimation;        /* Stage 10: downsample */

    /* Processing rate (= CPU clock rate). */
    float sample_rate;
} AudioChain;

/* ============================================================================
 * Preset initialization
 * ============================================================================ */

/* Initialize audio chain with physically-accurate default values for a
 * given hardware variant + speaker type. */
void audio_chain_init_preset(AudioChain *chain, int console_variant,
                              int speaker_type, int region);

/* Prepare the chain for processing: compute IIR/FIR coefficients from
 * the physical component values. Call after changing any R/C value. */
void audio_chain_prepare(AudioChain *chain);

/* Free dynamically allocated resources (FIR taps). */
void audio_chain_destroy(AudioChain *chain);

/* ============================================================================
 * Console variant + speaker type enums
 * ============================================================================ */

#define AUDIO_CONSOLE_FAMICOM      0
#define AUDIO_CONSOLE_NES_FRONT    1
#define AUDIO_CONSOLE_NES_TOP      2
#define AUDIO_CONSOLE_DENDY        3

#define AUDIO_SPEAKER_SMALL_TV     0
#define AUDIO_SPEAKER_CONSOLE_TV   1
#define AUDIO_SPEAKER_PVM          2
#define AUDIO_SPEAKER_ARCADE       3
#define AUDIO_SPEAKER_HEADPHONES   4
#define AUDIO_SPEAKER_FAMICOM_RF   5

#endif /* AUDIO_CHAIN_H */
