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
 * output resistance a second pole: with the gate as a MOS pair fitted to
 * the data sheet (gfs 35 mA/V, Gol 20) the deck's response is matched by a
 * 21.5 kHz pole within 0.06 dB to 12 kHz. The jack's 1 uF into the TV's
 * input is the separate TV coupling stage.
 * Famicom (HVC-001): C2 1 uF against R3//R4//R5 (10k, 20k, 12k) gives 37 Hz
 * (lidnariq; rainwarrior measured about 32 Hz). Its low-pass sits in the RF
 * modulator's sound path and is a generic figure.
 * NES-101 (top loader) and Dendy: no schematic here; the NES-101 takes the
 * NES-001 values and the Dendy the Famicom high-pass with a generic
 * low-pass. */
#define AUDIO_CORNERS_FAMICOM       ((AudioFilterCorners){ 37.0,     0.0, 10000.0 })
#define AUDIO_CORNERS_NES_FRONT     ((AudioFilterCorners){ 16.7, 21500.0, 15400.0 })
#define AUDIO_CORNERS_NES_TOP       ((AudioFilterCorners){ 16.7, 21500.0, 15400.0 })

/* Jack volts per unit of the core's mixer output. uXe and lidnariq saw
 * about 0.3 V of swing on the APU pins (nesdev thread 56); the pulse pin's
 * full mixer value is 0.2585 and the NES-001 gate takes it to the jack at
 * -47k/20.1k with its finite loop gain (tools/circuits/nes001_audio.cir:
 * 0.52 V peak), so one unit is about 2.0 V at the jack. An estimate, not a
 * measurement: it scales every setting given in volts. */
#define AUDIO_JACK_VOLTS_PER_UNIT   2.0f
#define AUDIO_CORNERS_DENDY         ((AudioFilterCorners){ 37.0,     0.0,  8000.0 })

/* The NES-001 supply and its way into the sound, the circuit of
 * tools/circuits/nes001_psu.cir (Electronix trace of the RF/AV module)
 * as audio_psu_derive runs it: the adaptor through the transformer's
 * source resistance (assumed) and the bridge's two conducting diodes
 * (the deck's generic exponential diodes) into the reservoir, the
 * console and the modulator (40 mA, assumed) drawing from it, and the
 * 7805 with its rejection until its input falls within the dropout (TI
 * data sheet, 2 V typical at 1 A). The 74HC04 gate carries the rail to
 * the jack at SUPPLY_GAIN_DB (nes001_audio.cir: about 2.8 V/V, flat to
 * 1 kHz, +6.4 dB at the line rate). test_audio holds the derivation to
 * the deck's sweep in golden/nes001_psu.h. */
#define AUDIO_NES001_PSU_SOURCE_OHM  0.8f
#define AUDIO_NES001_PSU_DIODE_IS    1e-9f   /* the deck's generic diode: IS, N, RS */
#define AUDIO_NES001_PSU_DIODE_N     1.7f
#define AUDIO_NES001_PSU_DIODE_RS    0.05f
#define AUDIO_NES001_PSU_MODULATOR_A 0.04f
#define AUDIO_NES001_PSU_DROPOUT_V   2.0f
#define AUDIO_NES001_SUPPLY_GAIN_DB  8.91f
/* Nominal NES-001 supply: the NES-002 adaptor (9 VAC at 1.3 A on its
 * plate, nearer 10 VAC at the console's draw), the module's 2200 uF
 * reservoir, about 600 mA on +5 V, the 7805's typical rejection. A preset
 * that leaves the PSU block at zero gets these. */
#define AUDIO_NES001_ADAPTOR_VAC     10.0f
#define AUDIO_NES001_RESERVOIR_UF    2200.0f
#define AUDIO_NES001_LOAD_MA         600.0f
#define AUDIO_NES001_REJECTION_DB    73.0f
/* The gate's rail window: the 74HC04 swings at most VCC - 2 V as a linear
 * amplifier (Nexperia data sheet), 3 V peak to peak on the 5 V rail. */
#define AUDIO_NES001_GATE_WINDOW_V   3.0f

/* Speaker model parameters. */
typedef struct {
    float resonance_hz;     /* fundamental resonance (200-400 Hz for small TV) */
    float bandwidth_low;    /* usable low-frequency limit (Hz) */
    float bandwidth_high;   /* usable high-frequency limit (Hz) */
    float cabinet_q;        /* cabinet resonance Q factor (0.5-5.0) */
    float cone_breakup_hz;  /* frequency where cone breaks up (~5 kHz) */
    float coupling_hp_hz;   /* amplifier output capacitor into the driver; 0 = none */
} SpeakerParams;

/* Predefined speaker profiles. */
#define SPEAKER_SMALL_TV     ((SpeakerParams){ 350.0f, 400.0f, 6000.0f, 2.0f, 5000.0f })
#define SPEAKER_CONSOLE_TV   ((SpeakerParams){ 150.0f,  80.0f, 10000.0f, 1.5f, 6000.0f })
#define SPEAKER_PVM          ((SpeakerParams){ 100.0f,  60.0f, 15000.0f, 0.8f, 8000.0f })
#define SPEAKER_ARCADE       ((SpeakerParams){ 200.0f, 100.0f, 8000.0f, 3.0f, 4000.0f })
#define SPEAKER_HEADPHONES   ((SpeakerParams){  20.0f,  20.0f, 20000.0f, 0.7f, 15000.0f })
#define SPEAKER_FAMICOM_RF   ((SpeakerParams){ 400.0f, 500.0f, 4000.0f, 2.5f, 3500.0f })

#endif
