/* Nominal console filter corners and generic speaker profiles. */
#ifndef AUDIO_FORMAT_H
#define AUDIO_FORMAT_H
#include <stdint.h>

typedef struct {
    double hp1_hz;      /* console coupling high-pass */
    double lp2_hz;      /* second low-pass (NES-001: C4 at the output pin on the gate's output resistance); 0 = none */
    double lp_hz;       /* amplifier bandwidth low-pass */
} AudioFilterCorners;

/* Corners per hardware variant.
 * NES-001: from N. Schenk's schematic through ngspice
 * (tools/circuits/nes001_audio.cir): C23 1 uF against the 20k/12k mixing
 * resistors and the 74HC04 gate's summing node gives 16.8 Hz (rainwarrior
 * measured about 16 Hz, nesdev thread 17745); R6 47k with C21 220 pF in the
 * feedback gives 15.4 kHz, and C4 0.01 uF at the output pin on the gate's
 * output resistance a second pole, 15.9 kHz at the assumed 1k (8 kHz at 2k,
 * 53 kHz at 300 ohm). The jack's 1 uF into the TV's input is the separate
 * TV coupling stage.
 * Famicom (HVC-001): C2 1 uF against R3//R4//R5 (10k, 20k, 12k) gives 37 Hz
 * (lidnariq; rainwarrior measured about 32 Hz). Its low-pass sits in the RF
 * modulator's sound path and is a generic figure.
 * NES-101 (top loader) and Dendy: no schematic here; the NES-101 takes the
 * NES-001 values and the Dendy the Famicom high-pass with a generic
 * low-pass. */
#define AUDIO_CORNERS_FAMICOM       ((AudioFilterCorners){ 37.0,     0.0, 10000.0 })
#define AUDIO_CORNERS_NES_FRONT     ((AudioFilterCorners){ 16.8, 15900.0, 15400.0 })
#define AUDIO_CORNERS_NES_TOP       ((AudioFilterCorners){ 16.8, 15900.0, 15400.0 })
#define AUDIO_CORNERS_DENDY         ((AudioFilterCorners){ 37.0,     0.0,  8000.0 })

/* Speaker model parameters. */
typedef struct {
    float resonance_hz;     /* fundamental resonance (200-400 Hz for small TV) */
    float bandwidth_low;    /* usable low-frequency limit (Hz) */
    float bandwidth_high;   /* usable high-frequency limit (Hz) */
    float cabinet_q;        /* cabinet resonance Q factor (0.5-5.0) */
    float cone_breakup_hz;  /* frequency where cone breaks up (~5 kHz) */
} SpeakerParams;

/* Predefined speaker profiles. */
#define SPEAKER_SMALL_TV     ((SpeakerParams){ 350.0f, 400.0f, 6000.0f, 2.0f, 5000.0f })
#define SPEAKER_CONSOLE_TV   ((SpeakerParams){ 150.0f,  80.0f, 10000.0f, 1.5f, 6000.0f })
#define SPEAKER_PVM          ((SpeakerParams){ 100.0f,  60.0f, 15000.0f, 0.8f, 8000.0f })
#define SPEAKER_ARCADE       ((SpeakerParams){ 200.0f, 100.0f, 8000.0f, 3.0f, 4000.0f })
#define SPEAKER_HEADPHONES   ((SpeakerParams){  20.0f,  20.0f, 20000.0f, 0.7f, 15000.0f })
#define SPEAKER_FAMICOM_RF   ((SpeakerParams){ 400.0f, 500.0f, 4000.0f, 2.5f, 3500.0f })

#endif
