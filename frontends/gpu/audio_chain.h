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

/* RF sound buzz (stage 11). The set detects sound from the intercarrier,
 * the beat of the vision and sound carriers: its amplitude carries the
 * vision carrier's AM, which is the picture itself, and its phase the
 * modulator's incidental phase modulation. The limiter removes the AM only
 * to its AM rejection, so the picture's field and line structure reaches
 * the audio as buzz, and the phase modulation as ticks where the level
 * steps at the blanking edges. The waveform is made from the frame's
 * per-line level (audio_chain_rf_buzz) and enters at the detector, before
 * the set's de-emphasis. */
typedef struct {
    bool  enabled;
    float am_rejection;     /* detector output per unit of AM index over full deviation: 10^(-dB/20) */
    float icpm_rad;         /* modulator carrier phase shift from blanking to white */
    float full_deviation;   /* detector output for full deviation, mixer units */
    float deviation_hz;     /* sound carrier peak deviation: 25 kHz System M, 50 kHz PAL */
    float line_hz;          /* 15734.264 (M) or 15625 (PAL) */
    int   lines;            /* lines per frame: 262 or 312 */
} AudioRFSoundStage;

/* One frame's picture as the audio sees it: the mean video level of each
 * line, 0 at blanking and 1 at white, 0 on the vertical blanking lines. */
#define AUDIO_FRAME_LINES 320
typedef struct {
    float level[AUDIO_FRAME_LINES];
    int   lines;
    float line_phase;       /* line-rate oscillator, carried between blocks by the caller */
} AudioVideoFrame;

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
    AudioRFSoundStage   rf_sound;           /* Stage 11: intercarrier buzz on RF */

    /* RF sound: the set's de-emphasis time constant in microseconds (75 for
     * System M, 50 for PAL; ITU-R BT.470), 0 for a baseband connection. The
     * NES modulator applies no pre-emphasis, so on RF the cable slot carries
     * this low-pass instead of a lead's negligible capacitance. */
    float rf_deemphasis_us;

    /* Processing rate of the band-limited APU callback, in Hz. */
    float sample_rate;
    uint32_t bypass_mask;       /* runtime diagnostics, eleven stage bits */
} AudioChain;

/* The console supply's ripple on +5 V and the hum it puts on the jack,
 * from the PSU deck's closed form (audio_format.h): the reservoir's
 * sawtooth against the regulator's rejection while it is in, the notch
 * that passes when the reservoir's trough falls under the dropout, and
 * the NES-001 gate's supply gain to the jack. Famicom and Dendy have no
 * modelled supply path into their sound and get no hum from it. */
typedef struct {
    float raw_min_v;        /* reservoir trough */
    float raw_pp_v;         /* reservoir ripple, peak to peak */
    float rail_ripple_v;    /* +5 V component at twice mains, peak */
    float jack_v;           /* the same at the jack, peak */
    float harmonic_2, harmonic_3;
    bool  dropout;          /* the regulator drops out in the trough */
} AudioPsuDerived;
AudioPsuDerived audio_psu_derive(float adaptor_vac, float reservoir_uf, float load_ma,
                                 float rejection_db, float mains_hz, int console_variant);

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
/* The same with a per-sample signal added at the detector point, after the
 * console and before the set: the RF sound buzz of the block's frame
 * (NULL = none). The GPU path takes the same array. */
void audio_chain_process_aux(const AudioChain *chain, AudioState *state,
                             const float *input, const float *aux, float *output, int count);
/* The frame's per-line level from the PPU's picture codes (256 x 240, the
 * 9-bit code with emphasis in bits 6-8) in a 262- or 312-line frame with
 * the picture from line 0; the audio needs only the field structure. */
void audio_video_frame_from_codes(AudioVideoFrame *frame, const uint16_t *codes, int region);
/* The RF sound buzz for a block of count samples spanning one frame. */
void audio_chain_rf_buzz(const AudioChain *chain, AudioVideoFrame *frame, float *aux, int count);

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
#define AUDIO_SPEAKER_TOSHIBA_14AF43 7 /* AN5276, 1000 uF into 4x7 cm 8 ohm ovals (service manual) */
#define AUDIO_SPEAKER_JVC_AV27D    8   /* two 5x12 cm ovals, 5 W each (service manual); amplifier not identified */
#define AUDIO_SPEAKER_WEGA_27FS    9   /* two 6x12 cm drivers on a TDA8947J bridge, 10 W each (service manual) */
#define AUDIO_SPEAKER_COMMODORE_1702 10 /* one 10 cm 8 ohm driver on an AN5265 through 1000 uF (service manual) */
#define AUDIO_SPEAKER_ZENITH_19    11  /* one PM speaker on a 2 W module (owner's manual, class estimate) */
#define AUDIO_SPEAKER_RCA_CONSOLE  12  /* two 5 in woofers and two 2 in tweeters, 16 ohm, 50 Hz to 15 kHz (spec sheet) */
#define AUDIO_SPEAKER_PVM_20M4U    13  /* 0.8 W mono speaker on an AN5265 (brochure, service manual) */
#define AUDIO_SPEAKER_NEC_XM29     14  /* two 9x5.5 cm 16 ohm ovals on a TA8211AH, 2.5 W each (service manual) */
#define AUDIO_SPEAKER_LAST         14

#endif /* AUDIO_CHAIN_H */
