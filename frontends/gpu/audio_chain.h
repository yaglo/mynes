/* Streaming console → cable → speaker audio, after APU anti-alias resampling.
 * CPU and GPU backends share coefficients and state. No second decimator. */

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

/* Amplifier rail window (stage 4, with the soft clip): the NES-001's 74HC04
 * gate swings at most VCC - 2 V (data sheet, linear amplifier), a hard limit
 * with a short knee, expressed in units of the mixer output. */
typedef struct {
    bool  enabled;
    float window;           /* peak swing in mixer units */
    float knee;             /* share of the window over which the limit rounds off */
} AudioRailStage;

/* PSU hum injection (stage 5). */
typedef struct {
    bool  enabled;
    float amplitude;        /* peak voltage of mains hum (0-0.02) */
    float frequency;        /* 60 Hz (NTSC) or 50 Hz (PAL) */
    float harmonic_2;       /* 2nd harmonic amplitude (120/100 Hz) */
    float harmonic_3;       /* 3rd harmonic amplitude (180/150 Hz) */
} AudioHumStage;

/* Noise floor (stage 6). */
typedef struct {
    bool  enabled;
    float amplitude;        /* peak noise level (0-0.02) */
} AudioNoiseStage;

/* Speaker model (stage 9). */
typedef struct {
    bool  enabled;
    SpeakerParams params;   /* from audio_format.h */
    /* Precomputed biquad coefficients (set by audio_chain_prepare). */
    float biquad_resonance[5]; /* a0, a1, a2, b1, b2 for resonance peak */
    float biquad_rolloff[5];   /* a0, a1, a2, b1, b2 for high-freq rolloff */
} AudioSpeakerStage;

/* ============================================================================
 * Complete audio chain configuration
 * ============================================================================ */

typedef struct {
    /* Hardware variant determines default component values. */
    int console_variant;    /* 0=Famicom, 1=NES Front, 2=NES Top, 3=Dendy */

    /* Signal stages (in processing order). */
    AudioRCStage        coupling_cap;       /* Stage 1: DC blocking */
    AudioRCStage        feedback_network;   /* Stage 2: output-pin low-pass (NES-001 C4 on the gate's output resistance) */
    AudioRCStage        amp_bandwidth;      /* Stage 3: amp LP */
    AudioSaturationStage amp_saturation;    /* Stage 4: soft clip (legacy tanh drive) */
    AudioRailStage      rail_clip;          /* Stage 4: gate rail window */
    AudioHumStage       psu_hum;            /* Stage 5: mains hum */
    AudioNoiseStage     noise_floor;        /* Stage 6: thermal noise */
    AudioRCStage        cable;              /* Stage 7: cable capacitance, or the set's sound de-emphasis on RF */
    AudioRCStage        tv_input_coupling;  /* Stage 8: TV input cap */
    AudioRCStage        speaker_coupling;   /* Stage 10: amplifier output capacitor into the driver */
    AudioSpeakerStage   speaker;            /* Stage 9: speaker model */

    /* RF sound: the set's de-emphasis time constant in microseconds (75 for
     * System M, 50 for PAL; ITU-R BT.470), 0 for a baseband connection. The
     * NES modulator applies no pre-emphasis, so on RF the cable slot carries
     * this low-pass instead of a lead's negligible capacitance. */
    float rf_deemphasis_us;

    /* Processing rate of the band-limited APU callback, in Hz. */
    float sample_rate;
    uint32_t bypass_mask;       /* runtime diagnostics, ten stage bits */
} AudioChain;

/* ============================================================================
 * Preset initialization
 * ============================================================================ */

/* Initialize nominal console filter corners and a generic speaker model. */
void audio_chain_init_preset(AudioChain *chain, int console_variant,
                              int speaker_type, int region);

/* Prepare the chain for processing: compute IIR coefficients from
 * the physical component values. Call after changing any R/C value. */
void audio_chain_prepare(AudioChain *chain);

/* State is independent of frame boundaries and can move between backends. */
typedef struct {
    float rc[6][2];             /* previous input/output per RC stage */
    float speaker[2][2];        /* transposed direct-form II delays */
    float hum_phase;
    uint32_t rng;
} AudioState;

typedef struct {
    uint32_t count, flags, pad[2];
    float rc[6][4];             /* a, b, highpass, enabled */
    float speaker[2][8];        /* five biquad coefficients, padded to vec4 */
    float effects[4];           /* drive, hum amplitude, phase step, noise */
    float harmonics[4];         /* relative second/third hum harmonics */
    float clip[4];              /* rail window, knee */
} AudioParams;

#define AUDIO_STREAM_RATE 44100
#define AUDIO_BLOCK_CAPACITY 2048
void audio_chain_params(const AudioChain *chain, int count, AudioParams *params);
void audio_chain_process(const AudioChain *chain, AudioState *state,
                         const float *input, float *output, int count);

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
#define AUDIO_SPEAKER_PVM_14L2     6   /* AN5278, 100 uF into a 7x5 cm driver (service manual) */
#define AUDIO_SPEAKER_TOSHIBA_14AF43 7 /* AN5276, 1000 uF into 8 ohm 5 W drivers (service manual) */
#define AUDIO_SPEAKER_LAST         7

#endif /* AUDIO_CHAIN_H */
