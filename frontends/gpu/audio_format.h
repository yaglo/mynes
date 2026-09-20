/* Nominal console filter corners and generic speaker profiles. */
#ifndef AUDIO_FORMAT_H
#define AUDIO_FORMAT_H
#include <stdint.h>

typedef struct {
    double hp1_hz;      /* coupling cap high-pass (DC blocking): 16-90 Hz */
    double hp2_hz;      /* feedback network high-pass: ~440 Hz */
    double lp_hz;       /* amplifier bandwidth low-pass: 8000-14000 Hz */
} AudioFilterCorners;

/* Predefined corners per hardware variant. */
#define AUDIO_CORNERS_FAMICOM       ((AudioFilterCorners){ 16.0,  440.0, 10000.0 })
#define AUDIO_CORNERS_NES_FRONT     ((AudioFilterCorners){ 90.0,  440.0, 14000.0 })
#define AUDIO_CORNERS_NES_TOP       ((AudioFilterCorners){ 90.0,  440.0, 12000.0 })
#define AUDIO_CORNERS_DENDY         ((AudioFilterCorners){ 37.0,  440.0,  8000.0 })

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
